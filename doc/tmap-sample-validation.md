# Reproducible ISO sample validation

The two user-provided archives contain 24 HEIC files each. They are external
fixtures and are not redistributed in the repository or public Actions artifacts.
The archive identities are:

| Archive | SHA-256 | Classification |
| --- | --- | --- |
| `1-ISO_22028-5_HDR_PQ_4-93.zip` | `6461cdcb135316f0301242334c625eec2a7eb0d0a0fe9ad872fc836eeb33c500` | 24 ordinary HDR PQ files, no ISO tmap item |
| `2-ISO_21496-1_Adaptive_HDR_4-93.zip` | `f9246aaf02c1b6d8b6532aeeaabc5f244463511fa62f0757fe77d6dbace71b78` | 24 ISO tmap files with mono gain metadata |

On 2026-10-11, all 48 files passed the native external-sample interoperability
case (481 assertions). All 24 Adaptive HDR files passed separate baseline,
gain-raster, integer reconstruction and fully applied float32 reconstruction;
all 24 PQ primary images decoded successfully. The first per-file report uses
the production reader at `5bfe52a36f2fc74525ba50dc78aa40e4eb3b467c`. The subsequent
writer-brand repair does not change that reader or reconstruction code.

The reconstructed dimensions range from 3088x2316 to 8064x6048, including portrait
images. Adaptive files use ICC-described base/alternate images and CP=2/TC=2
gain storage NCLX, as validated by the native reader's associated-property
checks. The public profile getters can report no profile for an explicitly
associated NCLX (2,2,2,full) tuple; a null `colour_type` in the report therefore
does not prove that the gain item lacks a `colr` property. Little CMS is enabled
for the alternate LUT profiles. This
result does not assert that those files work with Little CMS disabled.

## Run the per-file audit

Build with experimental APIs, the required codecs and Little CMS enabled:

```sh
cmake --preset testing -S . -B build-samples \
  -DCMAKE_BUILD_TYPE=Release -DENABLE_EXPERIMENTAL_FEATURES=ON \
  -DWITH_LIBDE265=ON -DWITH_LCMS2=ON
cmake --build build-samples --target tmap_interop --parallel 2
LIBHEIF_TMAP_INTEROP_DIR=/path/to/extracted-samples \
  build-samples/tests/tmap_interop 'User-provided ISO sample directory interoperability'
python3 scripts/check-tmap-samples.py \
  --library build-samples/libheif/libheif.so \
  --revision "$(git rev-parse HEAD)" --expect-files 48 \
  --output iso-archive-validation.json /path/to/extracted-samples
```

Use the library built from the same checkout as the script: the gain-map POD is
experimental. The Python tool needs only the standard library. It records the
library hash, file hashes, actual item types and ordered references, visibility,
primary item, ICC/NCLX descriptions, rational gain parameters, dimensions and
individual decode results. Float output is checked for 32-bit RGB, and decoded
dimensions must match the handle. The output path must be new; each completed
file checkpoints the report. Exit status is zero only when every file passes.
The optional per-file timeout defaults to 300 seconds. Each file uses a separate
process so an error, crash or timeout does not hide the following files.

The report contains technical descriptions rather than pixels or EXIF. It is
delivered separately to the user. CI exercises the same tool with generated
mono/RGB HEVC files, including a mixed valid/invalid directory that must report
two successes and one failure and return status 1.

## Evidence boundary

Successful decoding is container/codec/colour compatibility evidence. The PQ
archive is not used as an exact pixel oracle for the Adaptive HDR archive: these
are separately encoded files. The independently implemented raw Google math
oracle and the existing Apple/libavif pixel gates retain their separate roles.
This check does not establish complete ISO conformance, arbitrary ICC support,
physical-display behaviour or Photos persistence.
