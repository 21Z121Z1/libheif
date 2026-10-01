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

#include "gain_map_color.h"

#include <algorithm>
#include <cmath>

namespace {
Error unsupported_colour()
{
  return {heif_error_Unsupported_feature, heif_suberror_Unsupported_color_conversion,
          "ISO tone-map reconstruction supports NCLX BT.709/P3-D65/BT.2020 and sRGB/BT.709/linear/PQ only"};
}

Error invalid_value()
{
  return {heif_error_Invalid_input, heif_suberror_Unspecified,
          "Non-finite tone-map colour value"};
}

constexpr double pq_m1 = 2610.0 / 16384.0;
constexpr double pq_m2 = 2523.0 / 32.0;
constexpr double pq_c1 = 3424.0 / 4096.0;
constexpr double pq_c2 = 2413.0 / 128.0;
constexpr double pq_c3 = 2392.0 / 128.0;
constexpr double pq_scale = 10000.0 / 203.0;

Result<GainMapMatrix> to_xyz(uint16_t primaries)
{
  // D65 matrices derived from the CICP chromaticities. No chromatic adaptation.
  switch (primaries) {
    case 1:
      return GainMapMatrix{{{0.4123907993, 0.3575843394, 0.1804807884},
                            {0.2126390059, 0.7151686788, 0.0721923154},
                            {0.0193308187, 0.1191947798, 0.9505321522}}};
    case 9:
      return GainMapMatrix{{{0.6369580483, 0.1446169036, 0.1688809752},
                            {0.2627002120, 0.6779980715, 0.0593017165},
                            {0.0, 0.0280726930, 1.0609850577}}};
    case 12:
      return GainMapMatrix{{{0.4865709486, 0.2656676932, 0.1982172852},
                            {0.2289745641, 0.6917385218, 0.0792869141},
                            {0.0, 0.0451133819, 1.0439443689}}};
    default:
      return unsupported_colour();
  }
}

GainMapMatrix inverse(const GainMapMatrix& m)
{
  const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                     m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                     m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
  GainMapMatrix result{};
  for (size_t y = 0; y < 3; ++y) {
    for (size_t x = 0; x < 3; ++x) {
      result[x][y] = (m[(y + 1) % 3][(x + 1) % 3] * m[(y + 2) % 3][(x + 2) % 3] -
                      m[(y + 1) % 3][(x + 2) % 3] * m[(y + 2) % 3][(x + 1) % 3]) / det;
    }
  }
  return result;
}
}  // namespace

bool gain_map_supports_transfer(uint16_t transfer)
{
  return transfer == 1 || transfer == 6 || transfer == 8 || transfer == 13 ||
         transfer == 16;
}

Result<double> gain_map_decode_transfer(double value, uint16_t transfer)
{
  if (!std::isfinite(value)) {
    return invalid_value();
  }
  const double v = std::max(value, 0.0);
  switch (transfer) {
    case 8: return v;
    case 13: return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    case 1:
    case 6: return v < 0.081 ? v / 4.5 : std::pow((v + 0.099) / 1.099, 1.0 / 0.45);
    case 16: {
      const double p = std::pow(std::min(v, 1.0), 1.0 / pq_m2);
      return pq_scale * std::pow(std::max(p - pq_c1, 0.0) / (pq_c2 - pq_c3 * p), 1.0 / pq_m1);
    }
    default: return unsupported_colour();
  }
}

Result<double> gain_map_encode_transfer(double value, uint16_t transfer)
{
  if (!std::isfinite(value)) {
    return invalid_value();
  }
  const double v = std::max(value, 0.0);
  switch (transfer) {
    case 8: return v;
    case 13: return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
    case 1:
    case 6: return v < 0.018 ? 4.5 * v : 1.099 * std::pow(v, 0.45) - 0.099;
    case 16: {
      const double p = std::pow(std::min(v / pq_scale, 1.0), pq_m1);
      return std::pow((pq_c1 + pq_c2 * p) / (1 + pq_c3 * p), pq_m2);
    }
    default: return unsupported_colour();
  }
}

GainMapRGB gain_map_transform(const GainMapMatrix& matrix, const GainMapRGB& value)
{
  GainMapRGB result{};
  for (size_t y = 0; y < 3; ++y) {
    for (size_t x = 0; x < 3; ++x) {
      result[y] += matrix[y][x] * value[x];
    }
  }
  return result;
}

Result<GainMapMatrix> gain_map_primaries_matrix(uint16_t source, uint16_t target)
{
  auto src = to_xyz(source);
  auto dst = to_xyz(target);
  if (!src) { return src.error(); }
  if (!dst) { return dst.error(); }
  if (source == target) {
    return GainMapMatrix{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
  }
  const GainMapMatrix inv = inverse(*dst);
  GainMapMatrix result{};
  for (size_t y = 0; y < 3; ++y) {
    for (size_t x = 0; x < 3; ++x) {
      for (size_t k = 0; k < 3; ++k) {
        result[y][x] += inv[y][k] * (*src)[k][x];
      }
    }
  }
  return result;
}
