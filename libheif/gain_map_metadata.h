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
 * along with libheif.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef LIBHEIF_GAIN_MAP_METADATA_H
#define LIBHEIF_GAIN_MAP_METADATA_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "error.h"


struct GainMapVersion
{
  uint16_t minimum_version = 0;
  uint16_t writer_version = 0;
};


struct GainMapSignedRational32
{
  int32_t numerator = 0;
  uint32_t denominator = 1;
};


struct GainMapUnsignedRational32
{
  uint32_t numerator = 0;
  uint32_t denominator = 1;
};


struct GainMapChannel
{
  GainMapSignedRational32 gain_map_min{0, 1};
  GainMapSignedRational32 gain_map_max{0, 1};
  GainMapUnsignedRational32 gamma{1, 1};
  GainMapSignedRational32 base_offset{0, 1};
  GainMapSignedRational32 alternate_offset{0, 1};
};


struct GainMapMetadata
{
  GainMapVersion version;
  uint8_t channel_count = 1;
  bool use_base_colour_space = false;

  GainMapUnsignedRational32 base_hdr_headroom{0, 1};
  GainMapUnsignedRational32 alternate_hdr_headroom{1, 1};

  std::array<GainMapChannel, 3> channels;
};


enum class GainMapMetadataParseStatus
{
  parsed,
  unsupported_minimum_version,
  malformed
};


struct GainMapMetadataParseResult
{
  GainMapMetadataParseStatus status =
      GainMapMetadataParseStatus::malformed;
  GainMapVersion version;
  std::optional<GainMapMetadata> metadata;
  Error error;
  size_t bytes_consumed = 0;
};


GainMapMetadataParseResult parse_gain_map_metadata(
    std::span<const uint8_t> data);

Error validate_gain_map_metadata(const GainMapMetadata& metadata);

Result<std::vector<uint8_t>> serialize_gain_map_metadata(
    const GainMapMetadata& metadata);

#endif
