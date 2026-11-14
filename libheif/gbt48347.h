/*
 * HEIF codec.
 * Copyright (c) 2026 libheif contributors
 *
 * This file is part of libheif.
 *
 * libheif is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * libheif is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libheif. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef LIBHEIF_GBT48347_H
#define LIBHEIF_GBT48347_H

// GB/T 48347-2026 "HDR still images - Dual-layer format" metadata layer.
//
// Scope of this module: the GB/T-specific binary structures that are NOT
// already covered by the ISO 21496-1 gain-map code:
//   - UWAInfo carried by a HEIF 'it35' item (table 28, clause 9.3.6) and by the
//     JPEG UWA Info APP8 (tables 21/23, clause 9.2);
//   - extend_info_extension (table 15) with base/combined mapping blocks
//     (tables 16/17, clause 8.2.2).
//
// The HEIF 'tmap' item payload (table 27, ExtendInfoPart1) is byte-identical to
// the ISO 21496-1 ToneMapImage + GainMapMetadata and is handled by
// gain_map_metadata.h; it is deliberately NOT re-implemented here so that the
// generic ISO path is never relaxed for a GB/T special case.
//
// Standard-interpretation notes (recorded, not silently resolved):
//   - Table 13 (clause 8.2.1) lays the flag byte out as reserved:u(6), flag:u(1),
//     numberofcomponents_flag:u(1) (LSB-first), while tables 24/27 and Annex C.2
//     use the ISO MSB-first flag (bit7 multichannel, bit6 use_base_colour_space).
//     The HEIF/JPEG file embeddings (tables 24/27) follow the ISO layout; this
//     module therefore does not parse the abstract table-13 byte.
//   - The channel loop is printed as "i < number_of_components-1" in tables
//     13/24/27; with number_of_components in {1,3} that is a drafting
//     inconsistency. The file embeddings follow ISO 21496-1 (1 or 3 channels).
//   - The 3D LUT order N is not given in the table-16/17 syntax; it is derived
//     here from the size-prefixed block length and must be an exact cube.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "error.h"


namespace gbt48347
{

// ITU-T T.35 identifiers fixed by GB/T 48347-2026 clauses 9.2.2a / 9.2.3a.
constexpr uint8_t kCountryCode = 0x26;
constexpr uint16_t kTerminalProvideCode = 0x0004;
constexpr uint16_t kTerminalProvideOrientedCode = 0x0007;

// extended_type values (clauses 9.2.2a / 9.2.3a).
constexpr uint32_t kExtendedTypeBase = 0x01000000;          // file=1 meta=0 enhance=0
constexpr uint32_t kExtendedTypeEnhance = 0x02010100;       // file=2 meta=1 enhance=1
constexpr uint32_t kExtendedTypeEnhanceNoExt = 0x02010000;  // file=2 meta=1 enhance=0

inline uint8_t extended_file_type(uint32_t t) { return static_cast<uint8_t>((t >> 24) & 0xFF); }
inline uint8_t extended_meta_type(uint32_t t) { return static_cast<uint8_t>((t >> 16) & 0xFF); }
inline uint8_t extended_enhance_type(uint32_t t) { return static_cast<uint8_t>((t >> 8) & 0xFF); }


// One base/combined mapping block (tables 16/17). flag==0 means the block
// carries no transform; flag==16 or 32 selects the IEEE-754 binary16/binary32
// element width (GB/T 17966-2024) for the 3x3 matrix and the N^3 LUT.
struct MappingInfo
{
  uint8_t flag = 0;
  uint32_t lut_order = 0;          // N; 0 when no LUT samples follow the matrix
  std::vector<float> matrix;       // 9 row-major entries when flag != 0
  std::vector<float> lut;          // N^3 entries when lut_order != 0
};


// extend_info_extension (table 15): two size-prefixed mapping blocks.
struct ExtendInfoExtension
{
  bool base_present = false;
  MappingInfo base;
  bool combined_present = false;
  MappingInfo combined;
};


// UWAInfo (table 28). The JPEG APP8 framing (app_start_code/app_length/itut35_tag)
// is container-level and is not part of this structure.
struct UWAInfo
{
  uint8_t country_code = kCountryCode;
  uint16_t terminal_provide_code = kTerminalProvideCode;
  uint16_t terminal_provide_oriented_code = kTerminalProvideOrientedCode;
  uint8_t extended_frame_number = 2;
  uint32_t extended_type = kExtendedTypeEnhance;

  bool metadata_present = false;   // meta_type == 1
  // Redundant total from the wire. Its exact scope (whether it includes the two
  // 16-bit size fields) is not pinned down by the text, so it is recorded and
  // only bounds-checked, never used to locate the payloads.
  uint16_t metadata_size = 0;
  std::vector<uint8_t> static_metadata;
  std::vector<uint8_t> dynamic_metadata;

  bool extension_present = false;  // enhance_type == 1
  ExtendInfoExtension extension;
};


enum class ParseStatus
{
  parsed,
  malformed
};


struct UWAInfoParseResult
{
  ParseStatus status = ParseStatus::malformed;
  std::optional<UWAInfo> info;
  Error error;
  size_t bytes_consumed = 0;
};


// Parse a UWAInfo structure (HEIF 'it35' item data, or the payload of a JPEG
// UWA Info APP8 after its 6-byte 'ITUT35' tag). Big-endian, bounded reads.
UWAInfoParseResult parse_uwa_info(std::span<const uint8_t> data);

Result<std::vector<uint8_t>> serialize_uwa_info(const UWAInfo& info);


struct ExtensionParseResult
{
  ParseStatus status = ParseStatus::malformed;
  std::optional<ExtendInfoExtension> extension;
  Error error;
  size_t bytes_consumed = 0;
};

// Parse extend_info_extension (table 15) standalone.
ExtensionParseResult parse_extend_info_extension(std::span<const uint8_t> data);

Result<std::vector<uint8_t>> serialize_extend_info_extension(
    const ExtendInfoExtension& extension);

}  // namespace gbt48347

#endif  // LIBHEIF_GBT48347_H