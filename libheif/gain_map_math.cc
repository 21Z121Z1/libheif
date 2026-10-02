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

#include "gain_map_math.h"

#include <algorithm>
#include <cmath>

namespace {
Error numeric_error()
{
  return {heif_error_Invalid_input, heif_suberror_Unspecified,
          "Non-finite ISO 21496-1 reconstruction value"};
}
}  // namespace

double gain_map_full_weight(const GainMapMetadata& metadata)
{
  // Compare exact rational products. UINT32_MAX squared still fits uint64_t.
  const uint64_t alternate = uint64_t(metadata.alternate_hdr_headroom.numerator) *
                             metadata.base_hdr_headroom.denominator;
  const uint64_t baseline = uint64_t(metadata.base_hdr_headroom.numerator) *
                            metadata.alternate_hdr_headroom.denominator;
  return alternate > baseline ? 1.0 : -1.0;
}

Result<double> gain_map_target_weight(const GainMapMetadata& metadata, double target_headroom)
{
  if (auto error = validate_gain_map_metadata(metadata)) {
    return error;
  }
  if (!std::isfinite(target_headroom)) {
    return numeric_error();
  }
  const auto& b = metadata.base_hdr_headroom;
  const auto& a = metadata.alternate_hdr_headroom;
  const uint64_t ap = uint64_t(a.numerator) * b.denominator;
  const uint64_t bp = uint64_t(b.numerator) * a.denominator;
  const double direction = gain_map_full_weight(metadata);
  const double difference = direction * static_cast<double>(ap > bp ? ap - bp : bp - ap);
  // Form (target * base_denominator - base_numerator) with one rounding.
  // Subtracting a rounded base quotient can lose the entire interval between
  // nearby rational endpoints even though the integer cross-products differ.
  const double distance = std::fma(target_headroom, static_cast<double>(b.denominator),
                                    -static_cast<double>(b.numerator));
  return direction * std::clamp(distance * a.denominator / difference, 0.0, 1.0);
}

Result<double> gain_map_unnormalize(double normalized, const GainMapChannel& channel)
{
  if (!std::isfinite(normalized)) {
    return numeric_error();
  }
  const double low = static_cast<double>(channel.gain_map_min.numerator) / channel.gain_map_min.denominator;
  const double high = static_cast<double>(channel.gain_map_max.numerator) / channel.gain_map_max.denominator;
  const double inverse_gamma = static_cast<double>(channel.gamma.denominator) / channel.gamma.numerator;
  const double result = (high - low) * std::pow(std::clamp(normalized, 0.0, 1.0), inverse_gamma) + low;
  if (!std::isfinite(result)) {
    return numeric_error();
  }
  return result;
}

Result<double> gain_map_apply(double baseline, double log2_gain,
                              const GainMapChannel& channel, double weight)
{
  if (!std::isfinite(baseline) || !std::isfinite(log2_gain) ||
      !std::isfinite(weight) || weight < -1.0 || weight > 1.0) {
    return numeric_error();
  }
  const double base_offset = static_cast<double>(channel.base_offset.numerator) / channel.base_offset.denominator;
  const double alternate_offset = static_cast<double>(channel.alternate_offset.numerator) / channel.alternate_offset.denominator;
  const double result = (baseline + base_offset) * std::exp2(weight * log2_gain) - alternate_offset;
  if (!std::isfinite(result)) {
    return numeric_error();
  }
  return result;
}
