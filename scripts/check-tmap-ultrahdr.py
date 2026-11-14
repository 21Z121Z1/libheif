#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libheif contributors
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
"""Generate synthetic ISO containers and independent HDR pixel references."""

import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess


def boxes(data, start, end):
    """Read the bounded 32-bit box layout emitted by the pinned test producer."""
    while start < end:
        if end - start < 8:
            raise ValueError("Truncated synthetic box header")
        size, kind = struct.unpack_from(">I4s", data, start)
        if size < 8 or size > end - start:
            raise ValueError("Unexpected synthetic box size")
        yield kind, start, start + size
        start += size


def nclx_only_projection(path):
    """Keep the invalid producer original; project its known sRGB test baseline.

    The pinned HEIF workaround writes ICC alongside defined NCLX, contrary to
    HEIF 6.5.5. A zero ipma property index means no association. Clearing only
    that ICC association creates a separate single-NCLX fixture without moving
    extents or changing coded samples, gain metadata, or the NCLX description.
    This is test construction, not a repair performed by the standard reader.
    """
    original = path.read_bytes()
    data = bytearray(original)
    _, meta_start, meta_end = next(b for b in boxes(data, 0, len(data)) if b[0] == b"meta")
    _, iprp_start, iprp_end = next(b for b in boxes(data, meta_start + 12, meta_end) if b[0] == b"iprp")
    properties = list(boxes(data, iprp_start + 8, iprp_end))
    _, ipco_start, ipco_end = next(b for b in properties if b[0] == b"ipco")
    _, ipma_start, ipma_end = next(b for b in properties if b[0] == b"ipma")
    ipco = list(boxes(data, ipco_start + 8, ipco_end))
    if data[ipma_start + 8:ipma_start + 12] != bytes(4):
        raise ValueError("Unexpected synthetic ipma version or flags")
    count = struct.unpack_from(">I", data, ipma_start + 12)[0]
    position = ipma_start + 16
    removed = []
    baseline_nclx = []
    for _ in range(count):
        if position + 3 > ipma_end:
            raise ValueError("Truncated synthetic ipma entry")
        item, associations = struct.unpack_from(">HB", data, position)
        position += 3
        if position + associations > ipma_end:
            raise ValueError("Truncated synthetic ipma associations")
        for offset in range(position, position + associations):
            index = data[offset] & 0x7f
            if index == 0 or index > len(ipco):
                raise ValueError("Unexpected synthetic property index")
            kind, begin, end = ipco[index - 1]
            if item == 1 and kind == b"colr":
                if data[begin + 8:begin + 12] in (b"prof", b"rICC"):
                    if data[offset] & 0x80:
                        raise ValueError("Unexpected essential baseline ICC")
                    removed.append(offset)
                    data[offset] = 0
                else:
                    baseline_nclx.append(bytes(data[begin + 8:end]))
        position += associations
    if position != ipma_end or len(removed) != 1 or baseline_nclx != [
            b"nclx\x00\x01\x00\x0d\x00\x01\x80"]:
        raise ValueError("Pinned producer's baseline colour layout changed")
    projected = path.with_name(f"{path.stem}-nclx-only{path.suffix}")
    projected.write_bytes(data)
    return {"original": path.name, "projection": projected.name,
            "original_sha256": hashlib.sha256(original).hexdigest(),
            "projection_sha256": hashlib.sha256(data).hexdigest(),
            "changed_bytes": sum(a != b for a, b in zip(original, data)),
            "omitted_icc_association_offset": removed[0]}


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
projected = [path.with_name(f"{path.stem}-nclx-only{path.suffix}") for path in google]
manifest = directory / "google-nclx-only-projections.json"
raw = [directory / "google-base.rgba8"] + [
    directory / f"google-hdr-{mode}.rgba16f" for mode in ("mono", "rgb")]
references = [Path(str(path) + ".rgba16f") for path in ours + google + projected]
if any(not path.is_file() for path in ours):
    parser.error("Generate the libheif synthetic HEIC files first")
if any(path.exists() for path in raw + google + projected + references + [manifest]):
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
manifest.write_text(json.dumps([nclx_only_projection(path) for path in google], indent=2) + "\n")
for path, reference in zip(ours + google + projected, references):
    subprocess.run([app, "-m", "1", "-j", str(path), "-o", "0", "-O", "4",
                    "-z", str(reference)], check=True)
    print(f"PASS stock libultrahdr HDR decode {path.name}")
for original, projection in zip(google, projected):
    if Path(str(original) + ".rgba16f").read_bytes() != Path(str(projection) + ".rgba16f").read_bytes():
        raise ValueError(f"Projection changed stock Google HDR pixels: {original.name}")
    print(f"PASS unchanged stock Google HDR pixels {projection.name}")
