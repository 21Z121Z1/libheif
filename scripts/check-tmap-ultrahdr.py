#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libheif contributors
"""Generate synthetic ISO containers and independent HDR pixel references."""

import argparse
from pathlib import Path
import struct
import subprocess


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("ultrahdr_app", type=Path)
parser.add_argument("directory", type=Path,
                    help="directory containing the two libheif synthetic HEIC files")
args = parser.parse_args()
directory = args.directory
app = str(args.ultrahdr_app.resolve())
ours = [directory / f"libheif-{mode}-pq.heic" for mode in ("mono", "rgb")]
google = [directory / f"google-{mode}.{codec}"
          for mode in ("mono", "rgb") for codec in ("heic", "avif")]
raw = [directory / "google-base.rgba8"] + [
    directory / f"google-hdr-{mode}.rgba16f" for mode in ("mono", "rgb")]
references = [Path(str(path) + ".rgba16f") for path in ours + google]
if any(not path.is_file() for path in ours):
    parser.error("Generate the libheif synthetic HEIC files first")
if any(path.exists() for path in raw + google + references):
    parser.error("Refusing to overwrite interoperability fixtures or references")

size = 128
baseline = ((192 / 255 + 0.055) / 1.055) ** 2.4
raw[0].write_bytes(bytes((192, 192, 192, 255)) * size * size)
for mode, path in zip(("mono", "rgb"), raw[1:]):
    gains = (4, 4, 4) if mode == "mono" else (4, 3, 2)
    path.write_bytes(struct.pack("<eeee", *(baseline * g for g in gains), 1)
                     * size * size)
    for codec in ("heic", "avif"):
        output = directory / f"google-{mode}.{codec}"
        subprocess.run([app, "-m", "0", "-p", str(path), "-y", str(raw[0]),
                        "-w", str(size), "-h", str(size), "-a", "4", "-b", "3",
                        "-C", "0", "-c", "0", "-t", "0", "-s", "1",
                        "-M", "0" if mode == "mono" else "1",
                        "-q", "100", "-Q", "100", "-G", "1.5", "-z", str(output)],
                       check=True)
for path, reference in zip(ours + google, references):
    subprocess.run([app, "-m", "1", "-j", str(path), "-o", "0", "-O", "4",
                    "-z", str(reference)], check=True)
    print(f"PASS stock libultrahdr HDR decode {path.name}")
