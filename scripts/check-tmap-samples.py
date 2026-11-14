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
"""Audit local HEIF/AVIF samples with the built experimental public C API.

Each file runs in its own process; failures and timeouts do not hide subsequent
files. The JSON report contains hashes and technical descriptions, never image
pixels or EXIF. A successful decode establishes compatibility, not pixel accuracy.
Use a library built from this checkout: the gain-map POD ABI is experimental.
"""

import argparse
import ctypes as C
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import time


class Error(C.Structure):
    _fields_ = [("code", C.c_int), ("subcode", C.c_int), ("message", C.c_char_p)]


class SignedFraction(C.Structure):
    _fields_ = [("numerator", C.c_int32), ("denominator", C.c_uint32)]


class Fraction(C.Structure):
    _fields_ = [("numerator", C.c_uint32), ("denominator", C.c_uint32)]


class GainChannel(C.Structure):
    _fields_ = [("min", SignedFraction), ("max", SignedFraction),
                ("gamma", Fraction), ("base_offset", SignedFraction),
                ("alternate_offset", SignedFraction)]


class GainMetadata(C.Structure):
    _fields_ = [("struct_version", C.c_uint32), ("minimum_version", C.c_uint16),
                ("writer_version", C.c_uint16), ("channel_count", C.c_uint8),
                ("use_base_colour_space", C.c_uint8), ("base_headroom", Fraction),
                ("alternate_headroom", Fraction), ("channels", GainChannel * 3)]


class Nclx(C.Structure):
    _fields_ = [("version", C.c_uint8), ("primaries", C.c_int),
                ("transfer", C.c_int), ("matrix", C.c_int),
                ("full_range", C.c_uint8), ("chromaticities", C.c_float * 8)]


def fourcc(value):
    return value.to_bytes(4, "big").decode("ascii", "replace") if value else None


def file_hash(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def fraction(value):
    return {"numerator": value.numerator, "denominator": value.denominator}


def check(error):
    if error.code:
        raise RuntimeError(f"libheif {error.code}/{error.subcode}: "
                           f"{(error.message or b'').decode('utf-8', 'replace')}")


class Heif:
    def __init__(self, library):
        self.lib = C.CDLL(str(library))
        p, u, i = C.c_void_p, C.c_uint32, C.c_int
        pp, up = C.POINTER(p), C.POINTER(u)
        signatures = {
            "get_version": (C.c_char_p,), "init": (Error, p), "deinit": (None,),
            "context_alloc": (p,), "context_free": (None, p),
            "context_read_from_file": (Error, p, C.c_char_p, p),
            "context_get_number_of_items": (i, p),
            "context_get_list_of_item_IDs": (i, p, up, i),
            "context_get_primary_image_ID": (Error, p, up),
            "context_get_image_handle": (Error, p, u, pp),
            "context_get_item_references": (C.c_size_t, p, u, i, up, C.POINTER(up)),
            "release_item_references": (None, p, C.POINTER(up)),
            "item_get_item_type": (u, p, u), "item_is_item_hidden": (i, p, u),
            "image_handle_release": (None, p),
            "image_handle_get_item_id": (u, p),
            "image_handle_get_width": (i, p), "image_handle_get_height": (i, p),
            "image_handle_get_luma_bits_per_pixel": (i, p),
            "image_handle_get_color_profile_type": (u, p),
            "image_handle_get_raw_color_profile_size": (C.c_size_t, p),
            "image_handle_get_raw_color_profile": (Error, p, p),
            "image_handle_get_nclx_color_profile": (Error, p, C.POINTER(C.POINTER(Nclx))),
            "nclx_color_profile_free": (None, C.POINTER(Nclx)),
            "image_handle_get_gain_map_metadata_status": (i, p),
            "image_handle_get_gain_map_metadata": (Error, p, C.POINTER(GainMetadata)),
            "image_handle_get_tone_map_base_image_handle": (Error, p, pp),
            "image_handle_get_tone_map_gain_map_image_handle": (Error, p, pp),
            "decode_image": (Error, p, pp, i, i, p),
            "decode_tone_map_image_float32": (Error, p, pp, p, C.c_double, i),
            "image_release": (None, p), "image_get_primary_width": (i, p),
            "image_get_primary_height": (i, p), "image_get_colorspace": (i, p),
            "image_get_chroma_format": (i, p),
            "image_get_bits_per_pixel_range": (i, p, i),
        }
        for name, (result, *arguments) in signatures.items():
            fn = getattr(self.lib, "heif_" + name)
            fn.restype, fn.argtypes = result, arguments
            setattr(self, name, fn)

    def item(self, ctx, item_id):
        result = {"id": item_id, "type": fourcc(self.item_get_item_type(ctx, item_id)),
                  "hidden": bool(self.item_is_item_hidden(ctx, item_id)), "references": []}
        index = 0
        while True:
            reference_type = C.c_uint32()
            targets = C.POINTER(C.c_uint32)()
            count = self.context_get_item_references(ctx, item_id, index,
                                                     C.byref(reference_type), C.byref(targets))
            try:
                if not count:
                    break
                if not targets:
                    raise RuntimeError("Item references returned a null target array")
                result["references"].append({"type": fourcc(reference_type.value),
                                             "targets": list(targets[:count])})
            finally:
                self.release_item_references(ctx, C.byref(targets))
            index += 1
        return result

    def describe(self, ctx, handle):
        item_id = self.image_handle_get_item_id(handle)
        result = {"id": item_id, "type": fourcc(self.item_get_item_type(ctx, item_id)),
                  "hidden": bool(self.item_is_item_hidden(ctx, item_id)),
                  "dimensions": [self.image_handle_get_width(handle),
                                 self.image_handle_get_height(handle)],
                  "luma_depth": self.image_handle_get_luma_bits_per_pixel(handle),
                  "colour_type": fourcc(self.image_handle_get_color_profile_type(handle))}
        profile = C.POINTER(Nclx)()
        error = self.image_handle_get_nclx_color_profile(handle, C.byref(profile))
        if not error.code:
            try:
                result["nclx"] = {key: getattr(profile.contents, key)
                                  for key in ("primaries", "transfer", "matrix", "full_range")}
            finally:
                self.nclx_color_profile_free(profile)
        size = self.image_handle_get_raw_color_profile_size(handle)
        if size:
            data = C.create_string_buffer(size)
            check(self.image_handle_get_raw_color_profile(handle, data))
            raw = data.raw
            result["icc"] = {"size": size, "sha256": hashlib.sha256(raw).hexdigest(),
                             "class": raw[12:16].decode("ascii", "replace"),
                             "space": raw[16:20].decode("ascii", "replace"),
                             "pcs": raw[20:24].decode("ascii", "replace")}
            if len(raw) >= 132:
                count = struct.unpack_from(">I", raw, 128)[0]
                if count <= (len(raw) - 132) // 12:
                    result["icc"]["tags"] = [raw[132 + n * 12:136 + n * 12].decode("ascii", "replace")
                                              for n in range(count)]
        return result

    def decode(self, handle, target=None):
        pixels = C.c_void_p()
        try:
            if target is None:
                error = self.decode_image(handle, C.byref(pixels), 99, 99, None)
            else:
                error = self.decode_tone_map_image_float32(handle, C.byref(pixels), None, target, 0)
            check(error)
            if not pixels:
                raise RuntimeError("Successful decode returned no image")
            dimensions = [self.image_get_primary_width(pixels), self.image_get_primary_height(pixels)]
            if dimensions != [self.image_handle_get_width(handle), self.image_handle_get_height(handle)]:
                raise RuntimeError("Decoded dimensions differ from the image handle")
            result = {"status": "pass", "dimensions": dimensions,
                      "colorspace": self.image_get_colorspace(pixels),
                      "chroma": self.image_get_chroma_format(pixels)}
            if result["colorspace"] == 1:
                result["rgb_depth"] = [self.image_get_bits_per_pixel_range(pixels, c) for c in (3, 4, 5)]
            if target is not None and result.get("rgb_depth") != [32, 32, 32]:
                raise RuntimeError("Float32 decode did not return 32-bit RGB")
            return result
        finally:
            if pixels:
                self.image_release(pixels)


def inspect(library, path):
    api = Heif(library)
    check(api.init(None))
    ctx = api.context_alloc()
    if not ctx:
        api.deinit()
        raise RuntimeError("Could not allocate a file context")
    result = {}
    try:
        check(api.context_read_from_file(ctx, str(path).encode(), None))
        count = api.context_get_number_of_items(ctx)
        if count < 0:
            raise RuntimeError("Invalid item count")
        ids = (C.c_uint32 * count)()
        if api.context_get_list_of_item_IDs(ctx, ids, count) != count:
            raise RuntimeError("Incomplete item enumeration")
        primary = C.c_uint32()
        check(api.context_get_primary_image_ID(ctx, C.byref(primary)))
        result = {"library_version": api.get_version().decode(), "primary": primary.value,
                  "items": [api.item(ctx, item_id) for item_id in ids],
                  "tone_maps": []}
        for item_id in ids:
            if api.item_get_item_type(ctx, item_id) != int.from_bytes(b"tmap", "big"):
                continue
            handle = C.c_void_p()
            check(api.context_get_image_handle(ctx, item_id, C.byref(handle)))
            try:
                tone_map = api.describe(ctx, handle)
                result["tone_maps"].append(tone_map)
                metadata = GainMetadata(struct_version=1)
                tone_map["metadata_status"] = api.image_handle_get_gain_map_metadata_status(handle)
                check(api.image_handle_get_gain_map_metadata(handle, C.byref(metadata)))
                tone_map["metadata"] = {key: getattr(metadata, key) for key in
                                        ("minimum_version", "writer_version", "channel_count", "use_base_colour_space")}
                for key in ("base_headroom", "alternate_headroom"):
                    tone_map["metadata"][key] = fraction(getattr(metadata, key))
                tone_map["metadata"]["channels"] = [{key: fraction(getattr(channel, key)) for key in
                                                    ("min", "max", "gamma", "base_offset", "alternate_offset")}
                                                   for channel in metadata.channels[:metadata.channel_count]]
                for role in ("base", "gain_map"):
                    child = C.c_void_p()
                    fn = getattr(api, "image_handle_get_tone_map_" + role + "_image_handle")
                    check(fn(handle, C.byref(child)))
                    try:
                        tone_map[role] = api.describe(ctx, child)
                        tone_map[role]["decode"] = api.decode(child)
                    finally:
                        api.image_handle_release(child)
                tone_map["decode"] = api.decode(handle)
                base, alternate = metadata.base_headroom, metadata.alternate_headroom
                target = alternate.numerator / alternate.denominator
                # Select the fully applied endpoint despite quotient rounding.
                increasing = alternate.numerator * base.denominator > base.numerator * alternate.denominator
                target = max(0.0, math.nextafter(target, math.inf if increasing else -math.inf))
                tone_map["float32_decode"] = api.decode(handle, target)
            finally:
                api.image_handle_release(handle)
        if not result["tone_maps"]:
            handle = C.c_void_p()
            check(api.context_get_image_handle(ctx, primary.value, C.byref(handle)))
            try:
                result["primary_image"] = api.describe(ctx, handle)
                result["primary_image"]["decode"] = api.decode(handle)
            finally:
                api.image_handle_release(handle)
        return result
    except RuntimeError as error:
        result.update(status="fail", error=str(error))
        return result
    finally:
        api.context_free(ctx)
        api.deinit()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--revision", default="unspecified")
    parser.add_argument("--timeout", type=float, default=300)
    parser.add_argument("--expect-files", type=int)
    parser.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("paths", type=Path, nargs="+")
    args = parser.parse_args()
    if args.worker:
        print(json.dumps(inspect(args.library.resolve(), args.paths[0]), allow_nan=False))
        return 0
    if not args.output or args.output.exists():
        parser.error("--output must name a new report file")
    if not args.library.is_file() or not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("A built library and a positive finite timeout are required")
    paths = set()
    for path in args.paths:
        if not path.exists():
            parser.error(f"Input does not exist: {path}")
        if path.is_dir():
            paths.update(p.resolve() for p in path.rglob("*") if p.is_file() and
                         p.suffix.lower() in (".heic", ".heif", ".avif"))
        else:
            paths.add(path.resolve())
    if not paths or (args.expect_files is not None and len(paths) != args.expect_files):
        parser.error(f"Found {len(paths)} files, expected {args.expect_files or 'a nonempty set'}")
    report = {"schema_version": 1, "revision": args.revision, "library_sha256": file_hash(args.library),
              "evidence": "decode compatibility; no independent pixel oracle", "files": []}
    for path in sorted(paths):
        row = {"file": path.name}
        start = time.monotonic()
        try:
            row.update(size=path.stat().st_size, sha256=file_hash(path))
            completed = subprocess.run([sys.executable, str(Path(__file__).resolve()), "--worker",
                                        "--library", str(args.library.resolve()), str(path)],
                                       capture_output=True, text=True, timeout=args.timeout, check=False)
            if completed.returncode:
                raise RuntimeError(f"worker exit {completed.returncode}: {completed.stderr[-4000:]}")
            row.update(json.loads(completed.stdout))
            row.setdefault("status", "pass")
        except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
            row.update(status="fail", error=str(error))
        row["elapsed_seconds"] = round(time.monotonic() - start, 3)
        report["files"].append(row)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        # Checkpoint each completed file, so an interrupted audit remains useful.
        args.output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n")
        print(f"{path.name}: {row['status']}", flush=True)
    report["summary"] = {"files": len(paths), "passed": sum(r["status"] == "pass" for r in report["files"]),
                         "tmap_files": sum(bool(r.get("tone_maps")) for r in report["files"])}
    args.output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n")
    return int(report["summary"]["passed"] != len(paths))


if __name__ == "__main__":
    sys.exit(main())
