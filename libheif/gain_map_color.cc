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
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace {
Error unsupported_colour()
{
  return {heif_error_Unsupported_feature, heif_suberror_Unsupported_color_conversion,
          "ISO tone-map reconstruction supports CICP BT.709/P3-D65/BT.2020 and sRGB/BT.709/linear/PQ only"};
}

Error unsupported_icc()
{
  return {heif_error_Unsupported_feature, heif_suberror_Unsupported_color_conversion,
          "ICC profile cannot be mapped to the supported ISO tone-map CICP subset"};
}

Error malformed_icc()
{
  return {heif_error_Invalid_input, heif_suberror_Unspecified,
          "Malformed ICC colour profile"};
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

constexpr uint32_t icc_sig(char a, char b, char c, char d)
{
  return (static_cast<uint32_t>(static_cast<uint8_t>(a)) << 24) |
         (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 8) |
         static_cast<uint32_t>(static_cast<uint8_t>(d));
}

bool read_u16(const std::vector<uint8_t>& data, size_t offset, uint16_t& value)
{
  if (offset > data.size() || data.size() - offset < 2) {
    return false;
  }
  value = static_cast<uint16_t>((static_cast<uint16_t>(data[offset]) << 8) |
                                static_cast<uint16_t>(data[offset + 1]));
  return true;
}

bool read_u32(const std::vector<uint8_t>& data, size_t offset, uint32_t& value)
{
  if (offset > data.size() || data.size() - offset < 4) {
    return false;
  }
  value = (static_cast<uint32_t>(data[offset]) << 24) |
          (static_cast<uint32_t>(data[offset + 1]) << 16) |
          (static_cast<uint32_t>(data[offset + 2]) << 8) |
          static_cast<uint32_t>(data[offset + 3]);
  return true;
}

Result<double> read_s15_fixed16(const std::vector<uint8_t>& data, size_t offset)
{
  uint32_t raw = 0;
  if (!read_u32(data, offset, raw)) {
    return malformed_icc();
  }
  const int64_t signed_value = (raw & 0x80000000U) ?
                               static_cast<int64_t>(raw) - 0x100000000LL :
                               static_cast<int64_t>(raw);
  return static_cast<double>(signed_value) / 65536.0;
}

struct IccTag
{
  uint32_t signature = 0;
  size_t offset = 0;
  size_t size = 0;
};

struct IccView
{
  const std::vector<uint8_t>* bytes = nullptr;
  size_t profile_size = 0;
  uint32_t profile_class = 0;
  uint32_t data_space = 0;
  uint32_t pcs = 0;
  std::vector<IccTag> tags;

  const IccTag* find(uint32_t signature) const
  {
    for (const auto& tag : tags) {
      if (tag.signature == signature) {
        return &tag;
      }
    }
    return nullptr;
  }
};

Result<IccView> parse_icc(const color_profile_raw& profile)
{
  const auto& data = profile.get_data();
  if (data.size() < 132) {
    return malformed_icc();
  }

  uint32_t declared_size = 0;
  uint32_t tag_count = 0;
  uint32_t profile_class = 0;
  uint32_t data_space = 0;
  uint32_t pcs = 0;
  if (!read_u32(data, 0, declared_size) ||
      !read_u32(data, 12, profile_class) ||
      !read_u32(data, 16, data_space) ||
      !read_u32(data, 20, pcs) ||
      !read_u32(data, 128, tag_count)) {
    return malformed_icc();
  }
  if (declared_size < 132 || declared_size > data.size()) {
    return malformed_icc();
  }
  if (tag_count > (declared_size - 132) / 12) {
    return malformed_icc();
  }

  IccView view;
  view.bytes = &data;
  view.profile_size = declared_size;
  view.profile_class = profile_class;
  view.data_space = data_space;
  view.pcs = pcs;
  view.tags.reserve(tag_count);

  for (uint32_t i = 0; i < tag_count; ++i) {
    const size_t table_offset = 132 + static_cast<size_t>(i) * 12;
    uint32_t signature = 0;
    uint32_t offset = 0;
    uint32_t size = 0;
    if (!read_u32(data, table_offset, signature) ||
        !read_u32(data, table_offset + 4, offset) ||
        !read_u32(data, table_offset + 8, size)) {
      return malformed_icc();
    }
    if (offset > declared_size || size > declared_size - offset) {
      return malformed_icc();
    }
    view.tags.push_back({signature, offset, size});
  }

  return view;
}

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

double determinant(const GainMapMatrix& m)
{
  return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
         m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
         m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

GainMapMatrix inverse(const GainMapMatrix& m)
{
  const double det = determinant(m);
  GainMapMatrix result{};
  for (size_t y = 0; y < 3; ++y) {
    for (size_t x = 0; x < 3; ++x) {
      result[x][y] = (m[(y + 1) % 3][(x + 1) % 3] * m[(y + 2) % 3][(x + 2) % 3] -
                      m[(y + 1) % 3][(x + 2) % 3] * m[(y + 2) % 3][(x + 1) % 3]) / det;
    }
  }
  return result;
}

GainMapMatrix multiply(const GainMapMatrix& a, const GainMapMatrix& b)
{
  GainMapMatrix result{};
  for (size_t y = 0; y < 3; ++y) {
    for (size_t x = 0; x < 3; ++x) {
      for (size_t k = 0; k < 3; ++k) {
        result[y][x] += a[y][k] * b[k][x];
      }
    }
  }
  return result;
}

double matrix_difference(const GainMapMatrix& a, const GainMapMatrix& b)
{
  double difference = 0;
  for (size_t y = 0; y < 3; ++y) {
    for (size_t x = 0; x < 3; ++x) {
      difference = std::max(difference, std::abs(a[y][x] - b[y][x]));
    }
  }
  return difference;
}

Result<GainMapRGB> parse_xyz_tag(const IccView& view, const IccTag& tag)
{
  if (!view.bytes) {
    return malformed_icc();
  }
  const auto& data = *view.bytes;
  uint32_t type = 0;
  if (tag.size < 20 || !read_u32(data, tag.offset, type) ||
      type != icc_sig('X', 'Y', 'Z', ' ')) {
    return malformed_icc();
  }

  GainMapRGB xyz{};
  for (size_t i = 0; i < 3; ++i) {
    auto value = read_s15_fixed16(data, tag.offset + 8 + i * 4);
    if (!value) {
      return value.error();
    }
    xyz[i] = *value;
  }
  return xyz;
}

Result<GainMapMatrix> parse_chad_tag(const IccView& view, const IccTag& tag)
{
  if (!view.bytes) {
    return malformed_icc();
  }
  const auto& data = *view.bytes;
  uint32_t type = 0;
  if (tag.size < 44 || !read_u32(data, tag.offset, type) ||
      type != icc_sig('s', 'f', '3', '2')) {
    return malformed_icc();
  }

  GainMapMatrix matrix{};
  for (size_t y = 0; y < 3; ++y) {
    for (size_t x = 0; x < 3; ++x) {
      auto value = read_s15_fixed16(data, tag.offset + 8 + (y * 3 + x) * 4);
      if (!value) {
        return value.error();
      }
      matrix[y][x] = *value;
    }
  }
  return matrix;
}

Result<double> evaluate_icc_curve(const IccView& view, const IccTag& tag, double x)
{
  if (!view.bytes) {
    return malformed_icc();
  }
  const auto& data = *view.bytes;
  uint32_t type = 0;
  if (tag.size < 12 || !read_u32(data, tag.offset, type)) {
    return malformed_icc();
  }

  double result = 0;
  if (type == icc_sig('p', 'a', 'r', 'a')) {
    uint16_t function_type = 0;
    if (!read_u16(data, tag.offset + 8, function_type) || function_type > 4) {
      return unsupported_icc();
    }
    constexpr std::array<size_t, 5> parameter_counts{1, 3, 4, 5, 7};
    const size_t count = parameter_counts[function_type];
    if (tag.size < 12 + count * 4) {
      return malformed_icc();
    }

    std::array<double, 7> p{};
    for (size_t i = 0; i < count; ++i) {
      auto value = read_s15_fixed16(data, tag.offset + 12 + i * 4);
      if (!value) {
        return value.error();
      }
      p[i] = *value;
    }
    if (!(p[0] > 0) || !std::isfinite(p[0])) {
      return unsupported_icc();
    }

    switch (function_type) {
      case 0:
        result = std::pow(x, p[0]);
        break;
      case 1:
        if (p[1] == 0) {
          return unsupported_icc();
        }
        result = x >= -p[2] / p[1] ? std::pow(p[1] * x + p[2], p[0]) : 0;
        break;
      case 2:
        if (p[1] == 0) {
          return unsupported_icc();
        }
        result = x >= -p[2] / p[1] ?
                 std::pow(p[1] * x + p[2], p[0]) + p[3] : p[3];
        break;
      case 3:
        result = x >= p[4] ? std::pow(p[1] * x + p[2], p[0]) : p[3] * x;
        break;
      case 4:
        result = x >= p[4] ?
                 std::pow(p[1] * x + p[2], p[0]) + p[5] : p[3] * x + p[6];
        break;
    }
  }
  else if (type == icc_sig('c', 'u', 'r', 'v')) {
    uint32_t count = 0;
    if (!read_u32(data, tag.offset + 8, count)) {
      return malformed_icc();
    }
    if (count == 0) {
      result = x;
    }
    else if (count == 1) {
      uint16_t gamma_raw = 0;
      if (tag.size < 14 || !read_u16(data, tag.offset + 12, gamma_raw)) {
        return malformed_icc();
      }
      const double gamma = gamma_raw / 256.0;
      if (!(gamma > 0)) {
        return unsupported_icc();
      }
      result = std::pow(x, gamma);
    }
    else {
      // A sampled curve may represent an arbitrary device transform. Do not
      // infer a standard transfer function from a sparse coincidence.
      return unsupported_icc();
    }
  }
  else {
    return unsupported_icc();
  }

  if (!std::isfinite(result)) {
    return unsupported_icc();
  }
  return std::clamp(result, 0.0, 1.0);
}

Result<uint16_t> classify_matrix_trc_transfer(const IccView& view,
                                             const std::array<const IccTag*, 3>& trcs)
{
  constexpr std::array<double, 10> samples{
      0.0, 0.001, 0.0031308, 0.01, 0.04, 0.1, 0.25, 0.5, 0.75, 1.0};
  constexpr std::array<uint16_t, 3> candidates{13, 1, 8};

  double best_error = std::numeric_limits<double>::infinity();
  uint16_t best_transfer = 0;
  for (uint16_t transfer : candidates) {
    double max_error = 0;
    for (const IccTag* trc : trcs) {
      for (double sample : samples) {
        auto actual = evaluate_icc_curve(view, *trc, sample);
        auto reference = gain_map_decode_transfer(sample, transfer);
        if (!actual) {
          return actual.error();
        }
        if (!reference) {
          return reference.error();
        }
        max_error = std::max(max_error, std::abs(*actual - *reference));
      }
    }
    if (max_error < best_error) {
      best_error = max_error;
      best_transfer = transfer;
    }
  }

  // s15Fixed16 matrix/TRC profiles quantize their parameters. The supplied
  // Display-P3 profile differs from the exact sRGB curve by about 4e-6;
  // 5e-4 leaves ample encoding tolerance while staying far from BT.709.
  if (best_error > 5e-4) {
    return unsupported_icc();
  }
  return best_transfer;
}

Result<nclx_profile> nclx_from_cicp(const IccView& view, const IccTag& tag)
{
  if (!view.bytes) {
    return malformed_icc();
  }
  const auto& data = *view.bytes;
  uint32_t type = 0;
  if (tag.size < 12 || !read_u32(data, tag.offset, type) ||
      type != icc_sig('c', 'i', 'c', 'p')) {
    return malformed_icc();
  }

  const uint16_t primaries = data[tag.offset + 8];
  const uint16_t transfer = data[tag.offset + 9];
  const uint16_t matrix = data[tag.offset + 10];
  const uint8_t full_range = data[tag.offset + 11];
  if (full_range > 1) {
    return malformed_icc();
  }

  // ICC.1:2022 requires MatrixCoefficients=0 for RGB/XYZ CICP profiles.
  if ((view.data_space == icc_sig('R', 'G', 'B', ' ') ||
       view.data_space == icc_sig('X', 'Y', 'Z', ' ')) &&
      matrix != 0) {
    return malformed_icc();
  }
  if (view.data_space != icc_sig('R', 'G', 'B', ' ')) {
    return unsupported_icc();
  }

  auto supported_primaries = to_xyz(primaries);
  if (!supported_primaries || !gain_map_supports_transfer(transfer)) {
    return unsupported_icc();
  }

  nclx_profile result;
  result.set_colour_primaries(primaries);
  result.set_transfer_characteristics(transfer);
  result.set_matrix_coefficients(matrix);
  result.set_full_range_flag(full_range != 0);
  return result;
}

Result<nclx_profile> nclx_from_matrix_trc(const IccView& view)
{
  if (view.data_space != icc_sig('R', 'G', 'B', ' ') ||
      view.pcs != icc_sig('X', 'Y', 'Z', ' ')) {
    return unsupported_icc();
  }

  const IccTag* r_xyz = view.find(icc_sig('r', 'X', 'Y', 'Z'));
  const IccTag* g_xyz = view.find(icc_sig('g', 'X', 'Y', 'Z'));
  const IccTag* b_xyz = view.find(icc_sig('b', 'X', 'Y', 'Z'));
  const IccTag* chad = view.find(icc_sig('c', 'h', 'a', 'd'));
  const IccTag* r_trc = view.find(icc_sig('r', 'T', 'R', 'C'));
  const IccTag* g_trc = view.find(icc_sig('g', 'T', 'R', 'C'));
  const IccTag* b_trc = view.find(icc_sig('b', 'T', 'R', 'C'));
  if (!r_xyz || !g_xyz || !b_xyz || !chad || !r_trc || !g_trc || !b_trc) {
    return unsupported_icc();
  }

  auto red = parse_xyz_tag(view, *r_xyz);
  auto green = parse_xyz_tag(view, *g_xyz);
  auto blue = parse_xyz_tag(view, *b_xyz);
  auto adaptation = parse_chad_tag(view, *chad);
  if (!red) { return red.error(); }
  if (!green) { return green.error(); }
  if (!blue) { return blue.error(); }
  if (!adaptation) { return adaptation.error(); }

  const double det = determinant(*adaptation);
  if (!std::isfinite(det) || std::abs(det) < 1e-12) {
    return unsupported_icc();
  }

  GainMapMatrix device_to_d50{{
      {(*red)[0], (*green)[0], (*blue)[0]},
      {(*red)[1], (*green)[1], (*blue)[1]},
      {(*red)[2], (*green)[2], (*blue)[2]}}};
  const GainMapMatrix device_to_adopted =
      multiply(inverse(*adaptation), device_to_d50);

  uint16_t best_primaries = 0;
  double best_difference = std::numeric_limits<double>::infinity();
  for (uint16_t primaries : {uint16_t{1}, uint16_t{9}, uint16_t{12}}) {
    auto reference = to_xyz(primaries);
    if (!reference) {
      continue;
    }
    const double difference = matrix_difference(device_to_adopted, *reference);
    if (difference < best_difference) {
      best_difference = difference;
      best_primaries = primaries;
    }
  }
  if (best_primaries == 0 || best_difference > 0.002) {
    return unsupported_icc();
  }

  auto transfer = classify_matrix_trc_transfer(view, {r_trc, g_trc, b_trc});
  if (!transfer) {
    return transfer.error();
  }

  nclx_profile result;
  result.set_colour_primaries(best_primaries);
  result.set_transfer_characteristics(*transfer);
  result.set_matrix_coefficients(0);
  result.set_full_range_flag(true);
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

Result<nclx_profile> gain_map_nclx_from_icc(const color_profile_raw& profile)
{
  auto view = parse_icc(profile);
  if (!view) {
    return view.error();
  }

  if (const IccTag* cicp = view->find(icc_sig('c', 'i', 'c', 'p'))) {
    return nclx_from_cicp(*view, *cicp);
  }
  return nclx_from_matrix_trc(*view);
}
