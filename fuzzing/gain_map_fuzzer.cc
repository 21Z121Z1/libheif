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

#include "gain_map_metadata.h"
#include "gain_map_math.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>

namespace {
void exercise_metadata(const GainMapMetadata& metadata, double sample)
{
  auto encoded = serialize_gain_map_metadata(metadata);
  assert(encoded);
  auto decoded = parse_gain_map_metadata(*encoded);
  assert(decoded.status == GainMapMetadataParseStatus::parsed);
  auto reencoded = serialize_gain_map_metadata(*decoded.metadata);
  assert(reencoded && *encoded == *reencoded);
  auto weight = gain_map_target_weight(metadata, sample * 32.0);
  for (uint8_t c = 0; c < metadata.channel_count; ++c) {
    const auto& channel = metadata.channels[c];
    auto gain = gain_map_unnormalize(sample, channel);
    if (gain) {
      (void) gain_map_apply(sample, *gain, channel, gain_map_full_weight(metadata));
      if (weight) { (void) gain_map_apply(sample, *gain, channel, *weight); }
    }
  }
}
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  const std::span<const uint8_t> bytes(data, size);
  const double sample = size ? data[size - 1] / 255.0 : 0.0;
  auto metadata = parse_gain_map_metadata(bytes);
  if (metadata.metadata) { exercise_metadata(*metadata.metadata, sample); }
  auto tmap = parse_tone_map_image(bytes);
  if (tmap.tone_map_image) {
    exercise_metadata(tmap.tone_map_image->gain_map_metadata, sample);
    auto encoded = serialize_tone_map_image(*tmap.tone_map_image);
    assert(encoded);
    auto decoded = parse_tone_map_image(*encoded);
    assert(decoded.status == ToneMapImageParseStatus::parsed);
  }
  return 0;
}
