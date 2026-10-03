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
#include <utility>
#include <vector>

#if HAVE_LCMS2
#include <lcms2.h>
#endif

namespace {
Error unsupported_colour()
{
  return {heif_error_Unsupported_feature, heif_suberror_Unsupported_color_conversion,
          "Unsupported ISO tone-map colour description"};
}

Error unsupported_icc()
{
  return {heif_error_Unsupported_feature, heif_suberror_Unsupported_color_conversion,
          "Unsupported ISO tone-map ICC colour transform"};
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
constexpr double hlg_scale = 1000.0 / 203.0;
constexpr double hlg_gamma = 1.2;
constexpr double hlg_a = 0.17883277;
constexpr double bt_alpha = 1.0992968268094429403;
constexpr double bt_beta = 0.0180539685108078073;
constexpr double smpte240_alpha = 1.1115721959217312197;
constexpr double smpte240_beta = 0.0228215855294450222;
constexpr double cinema_scale = 52.37 / 203.0;
const double hlg_b = 1.0 - 4.0 * hlg_a;
const double hlg_c = 0.5 - hlg_a * std::log(4.0 * hlg_a);

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

GainMapMatrix inverse(const GainMapMatrix& m);
GainMapMatrix multiply(const GainMapMatrix& a, const GainMapMatrix& b);

GainMapRGB adopted_white(uint16_t primaries)
{
  double x = 0.3127, y = 0.3290; // D65
  switch (primaries) {
    case 4:
    case 8: x = 0.310; y = 0.316; break; // Illuminant C
    case 10: return {1, 1, 1}; // Equal-energy XYZ centre white
    case 11: x = 0.314; y = 0.351; break; // DCI white
    default: break;
  }
  return {x / y, 1, (1 - x - y) / y};
}

GainMapMatrix adapt_white(const GainMapRGB& source_white, const GainMapRGB& target_white)
{
  if (source_white == target_white) {
    return GainMapMatrix{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
  }
  const GainMapMatrix bradford{{{0.8951, 0.2664, -0.1614},
                               {-0.7502, 1.7135, 0.0367},
                               {0.0389, -0.0685, 1.0296}}};
  const auto source = gain_map_transform(bradford, source_white);
  const auto target = gain_map_transform(bradford, target_white);
  GainMapMatrix scale{};
  for (size_t c = 0; c < 3; ++c) { scale[c][c] = target[c] / source[c]; }
  return multiply(inverse(bradford), multiply(scale, bradford));
}

Result<GainMapMatrix> to_xyz(uint16_t primaries)
{
  // Native-white matrices from H.273 Table 2. Keep the established D65
  // matrices at double precision; other code points use the same derivation.
  std::array<std::array<double, 2>, 3> xy{};
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
    case 4: xy = {{{0.67, 0.33}, {0.21, 0.71}, {0.14, 0.08}}}; break;
    case 5: xy = {{{0.64, 0.33}, {0.29, 0.60}, {0.15, 0.06}}}; break;
    case 6:
    case 7: xy = {{{0.630, 0.340}, {0.310, 0.595}, {0.155, 0.070}}}; break;
    case 8: xy = {{{0.681, 0.319}, {0.243, 0.692}, {0.145, 0.049}}}; break;
    case 10: return GainMapMatrix{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    case 11: xy = {{{0.680, 0.320}, {0.265, 0.690}, {0.150, 0.060}}}; break;
    case 22: xy = {{{0.630, 0.340}, {0.295, 0.605}, {0.155, 0.077}}}; break;
    default:
      return unsupported_colour();
  }
  GainMapMatrix matrix{};
  for (size_t c = 0; c < 3; ++c) {
    matrix[0][c] = xy[c][0] / xy[c][1];
    matrix[1][c] = 1;
    matrix[2][c] = (1 - xy[c][0] - xy[c][1]) / xy[c][1];
  }
  const auto scale = gain_map_transform(inverse(matrix), adopted_white(primaries));
  for (size_t row = 0; row < 3; ++row) {
    for (size_t c = 0; c < 3; ++c) { matrix[row][c] *= scale[c]; }
  }
  return matrix;
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

Result<double> evaluate_icc_curve(const IccView& view, const IccTag& tag, double x,
                                 bool allow_sampled = false)
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
      if (!allow_sampled) { return unsupported_icc(); }
      if (count > (tag.size - 12) / 2) { return malformed_icc(); }
      const double position = std::clamp(x, 0.0, 1.0) * (count - 1);
      const uint32_t low = static_cast<uint32_t>(position);
      const uint32_t high = std::min(low + 1, count - 1);
      uint16_t a = 0, b = 0;
      if (!read_u16(data, tag.offset + 12 + size_t(low) * 2, a) ||
          !read_u16(data, tag.offset + 12 + size_t(high) * 2, b)) {
        return malformed_icc();
      }
      result = (a + (static_cast<double>(b) - a) * (position - low)) / 65535.0;
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
  return transfer == 1 || (transfer >= 4 && transfer <= 18);
}

namespace {
double decode_bt(double signal)
{
  return signal < 4.5 * bt_beta ? signal / 4.5 : std::pow((signal + bt_alpha - 1) / bt_alpha, 1.0 / 0.45);
}

double encode_bt(double linear)
{
  return linear < bt_beta ? 4.5 * linear : bt_alpha * std::pow(linear, 0.45) - (bt_alpha - 1);
}
}  // namespace

Result<double> gain_map_decode_transfer(double value, uint16_t transfer)
{
  if (!std::isfinite(value)) {
    return invalid_value();
  }
  const double v = std::max(value, 0.0);
  switch (transfer) {
    case 4: return std::pow(v, 2.2);
    case 5: return std::pow(v, 2.8);
    case 7: return v < 4 * smpte240_beta ? v / 4 :
                    std::pow((v + smpte240_alpha - 1) / smpte240_alpha, 1.0 / 0.45);
    case 8: return v;
    case 9: return v == 0 ? 0.0 : std::pow(10.0, 2 * (v - 1));
    case 10: return v == 0 ? 0.0 : std::pow(10.0, 2.5 * (v - 1));
    case 11: return std::copysign(decode_bt(std::abs(value)), value);
    case 12: return value < 0 ? -decode_bt(-4 * value) / 4 : decode_bt(v);
    case 13: return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    case 1:
    case 6:
    case 14:
    case 15: return decode_bt(v);
    case 16: {
      const double p = std::pow(std::min(v, 1.0), 1.0 / pq_m2);
      return pq_scale * std::pow(std::max(p - pq_c1, 0.0) / (pq_c2 - pq_c3 * p), 1.0 / pq_m1);
    }
    case 17: return cinema_scale * std::pow(v, 2.6);
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
    case 4: return std::pow(v, 1.0 / 2.2);
    case 5: return std::pow(v, 1.0 / 2.8);
    case 7: return v < smpte240_beta ? 4 * v :
                    smpte240_alpha * std::pow(v, 0.45) - (smpte240_alpha - 1);
    case 8: return v;
    case 9: return v < 0.01 ? 0.0 : 1 + std::log10(v) / 2;
    case 10: return v < std::sqrt(10.0) / 1000 ? 0.0 : 1 + std::log10(v) / 2.5;
    case 11: return std::copysign(encode_bt(std::abs(value)), value);
    case 12: return value < 0 ? -encode_bt(-4 * value) / 4 : encode_bt(v);
    case 13: return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
    case 1:
    case 6:
    case 14:
    case 15: return encode_bt(v);
    case 16: {
      const double p = std::pow(std::min(v / pq_scale, 1.0), pq_m1);
      return std::pow((pq_c1 + pq_c2 * p) / (1 + pq_c3 * p), pq_m2);
    }
    case 17: return std::pow(v / cinema_scale, 1.0 / 2.6);
    default: return unsupported_colour();
  }
}

Result<double> gain_map_decode_matrix_signal(double value, uint16_t transfer)
{
  if (!std::isfinite(value)) { return invalid_value(); }
  if (transfer == 18) {
    const double v = std::max(value, 0.0);
    return v <= 0.5 ? v * v / 3.0 : (std::exp((v - hlg_c) / hlg_a) + hlg_b) / 12.0;
  }
  auto linear = gain_map_decode_transfer(transfer == 13 ? std::abs(value) : value, transfer);
  if (!linear) { return linear.error(); }
  if (transfer == 13) { return std::copysign(*linear, value); }
  return *linear / (transfer == 16 ? pq_scale : transfer == 17 ? cinema_scale : 1);
}

Result<double> gain_map_encode_matrix_signal(double value, uint16_t transfer)
{
  if (!std::isfinite(value)) { return invalid_value(); }
  if (transfer == 18) {
    const double v = std::max(value, 0.0);
    return v <= 1.0 / 12 ? std::sqrt(3 * v) : hlg_a * std::log(12.0 * v - hlg_b) + hlg_c;
  }
  auto signal = gain_map_encode_transfer(
      transfer == 13 ? std::abs(value) : value * (transfer == 16 ? pq_scale : transfer == 17 ? cinema_scale : 1),
      transfer);
  if (!signal) { return signal.error(); }
  return transfer == 13 ? std::copysign(*signal, value) : *signal;
}

Result<GainMapRGB> gain_map_decode_lms_matrix(const GainMapRGB& value, uint16_t matrix, uint16_t transfer)
{
  if (matrix != 14 && matrix != 15) { return unsupported_colour(); }
  // H.273 (2024) Eq.14-19 and Eq.79-87. All specified matrix numerators
  // divide by 4096; their inverses are computed once in double precision.
  static const GainMapMatrix lms_to_rgb = inverse({{{1688.0 / 4096, 2146.0 / 4096, 262.0 / 4096},
                                                   {683.0 / 4096, 2951.0 / 4096, 462.0 / 4096},
                                                   {99.0 / 4096, 309.0 / 4096, 3688.0 / 4096}}});
  static const GainMapMatrix ipt_lms_to_rgb = inverse({{{1747.0 / 4096, 2169.0 / 4096, 180.0 / 4096},
                                                       {673.0 / 4096, 3029.0 / 4096, 394.0 / 4096},
                                                       {50.0 / 4096, 207.0 / 4096, 3839.0 / 4096}}});
  static const GainMapMatrix pq_to_lms = inverse({{{0.5, 0.5, 0},
                                                  {6610.0 / 4096, -13613.0 / 4096, 7003.0 / 4096},
                                                  {17933.0 / 4096, -17390.0 / 4096, -543.0 / 4096}}});
  static const GainMapMatrix hlg_to_lms = inverse({{{0.5, 0.5, 0},
                                                   {3625.0 / 4096, -7465.0 / 4096, 3840.0 / 4096},
                                                   {9500.0 / 4096, -9212.0 / 4096, -288.0 / 4096}}});
  static const GainMapMatrix ipt_to_lms = inverse({{{1638.0 / 4096, 1638.0 / 4096, 820.0 / 4096},
                                                   {18248.0 / 4096, -19870.0 / 4096, 1622.0 / 4096},
                                                   {3300.0 / 4096, 1463.0 / 4096, -4763.0 / 4096}}});
  auto lms = gain_map_transform(matrix == 15 ? ipt_to_lms : transfer == 18 ? hlg_to_lms : pq_to_lms, value);
  for (auto& component : lms) {
    auto linear = gain_map_decode_matrix_signal(component, transfer);
    if (!linear) { return linear.error(); }
    component = *linear;
  }
  auto rgb = gain_map_transform(matrix == 15 ? ipt_lms_to_rgb : lms_to_rgb, lms);
  for (auto& component : rgb) {
    auto encoded = gain_map_encode_matrix_signal(component, transfer);
    if (!encoded) { return encoded.error(); }
    if (!std::isfinite(*encoded)) { return invalid_value(); }
    component = *encoded;
  }
  return rgb;
}

namespace {
Result<GainMapRGB> hlg_rgb(const GainMapRGB& value, const nclx_profile& profile, bool decode)
{
  auto xyz = to_xyz(profile.m_colour_primaries);
  if (!xyz) { return xyz.error(); }
  GainMapRGB linear{};
  for (size_t c = 0; c < 3; ++c) {
    if (!std::isfinite(value[c])) { return invalid_value(); }
    if (decode) {
      const double v = std::clamp(value[c], 0.0, 1.0);
      linear[c] = *gain_map_decode_matrix_signal(v, 18);
    }
    else {
      linear[c] = std::max(value[c], 0.0) / hlg_scale;
    }
  }

  // BT.2100 Table 5: the OOTF depends on luminance, not independent
  // per-channel powers. Obtain luminance from the declared RGB primaries.
  const double luminance = (*xyz)[1][0] * linear[0] +
                           (*xyz)[1][1] * linear[1] + (*xyz)[1][2] * linear[2];
  if (!std::isfinite(luminance)) { return invalid_value(); }
  if (luminance <= 0) { return GainMapRGB{0, 0, 0}; }
  const double scale = decode ? hlg_scale * std::pow(luminance, hlg_gamma - 1.0) :
                                std::pow(luminance, 1.0 / hlg_gamma - 1.0);
  GainMapRGB result{};
  for (size_t c = 0; c < 3; ++c) {
    const double v = linear[c] * scale;
    result[c] = decode ? v :
                (v <= 1.0 / 12.0 ? std::sqrt(3.0 * v) :
                                  hlg_a * std::log(12.0 * v - hlg_b) + hlg_c);
    if (!std::isfinite(result[c])) { return invalid_value(); }
  }
  return result;
}
}  // namespace

Result<GainMapRGB> gain_map_decode_rgb(const GainMapRGB& value, const nclx_profile& profile)
{
  if (profile.m_transfer_characteristics == 18) {
    return hlg_rgb(value, profile, true);
  }
  GainMapRGB result{};
  for (size_t c = 0; c < 3; ++c) {
    // H.273 TC 13 with a non-identity matrix denotes signed sYCC signals.
    auto component = profile.m_transfer_characteristics == 13 && profile.m_matrix_coefficients != 0 ?
                     gain_map_decode_matrix_signal(value[c], 13) :
                     gain_map_decode_transfer(value[c], profile.m_transfer_characteristics);
    if (!component) { return component.error(); }
    result[c] = *component;
  }
  return result;
}

Result<GainMapRGB> gain_map_encode_rgb(const GainMapRGB& value, const nclx_profile& profile)
{
  if (profile.m_transfer_characteristics == 18) {
    return hlg_rgb(value, profile, false);
  }
  GainMapRGB result{};
  for (size_t c = 0; c < 3; ++c) {
    auto component = profile.m_transfer_characteristics == 13 && profile.m_matrix_coefficients != 0 ?
                     gain_map_encode_matrix_signal(value[c], 13) :
                     gain_map_encode_transfer(value[c], profile.m_transfer_characteristics);
    if (!component) { return component.error(); }
    result[c] = *component;
  }
  return result;
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
  return multiply(inverse(*dst), multiply(adapt_white(adopted_white(source), adopted_white(target)), *src));
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

struct GainMapIccColour
{
  IccView view;
  std::array<IccTag, 3> curves;
  GainMapMatrix to_pcs;
#if HAVE_LCMS2
  cmsContext context = nullptr;
  cmsHPROFILE lut_profile = nullptr;
  cmsHPROFILE xyz_profile = nullptr;
  cmsHTRANSFORM decode_lut = nullptr;
  cmsHTRANSFORM encode_lut = nullptr;
  GainMapMatrix from_pcs;

  ~GainMapIccColour()
  {
    if (decode_lut) { cmsDeleteTransform(decode_lut); }
    if (encode_lut) { cmsDeleteTransform(encode_lut); }
    if (lut_profile) { cmsCloseProfile(lut_profile); }
    if (xyz_profile) { cmsCloseProfile(xyz_profile); }
    if (context) { cmsDeleteContext(context); }
  }
#endif
};

namespace {
Error validate_rgb_curve(const IccView& view, const IccTag& tag)
{
  const auto& bytes = *view.bytes;
  auto first = evaluate_icc_curve(view, tag, 0, true);
  auto last = evaluate_icc_curve(view, tag, 1, true);
  if (!first) { return first.error(); }
  if (!last) { return last.error(); }
  if (*last <= *first) { return unsupported_icc(); }
  uint32_t type = 0;
  read_u32(bytes, tag.offset, type);
  if (type == icc_sig('c', 'u', 'r', 'v')) {
    uint32_t count = 0;
    read_u32(bytes, tag.offset + 8, count);
    if (count > 1) {
      uint16_t previous = 0;
      for (uint32_t i = 0; i < count; ++i) {
        uint16_t value = 0;
        if (!read_u16(bytes, tag.offset + 12 + size_t(i) * 2, value)) { return malformed_icc(); }
        if (i && value < previous) { return unsupported_icc(); }
        previous = value;
      }
    }
  }
  else {
    uint16_t function = 0;
    read_u16(bytes, tag.offset + 8, function);
    if (function > 0) {
      auto a = read_s15_fixed16(bytes, tag.offset + 16);
      if (!a || *a <= 0) { return unsupported_icc(); }
    }
    if (function >= 3) {
      auto a = read_s15_fixed16(bytes, tag.offset + 16);
      auto b = read_s15_fixed16(bytes, tag.offset + 20);
      auto c = read_s15_fixed16(bytes, tag.offset + 24);
      auto d = read_s15_fixed16(bytes, tag.offset + 28);
      if (!a || !b || !c || !d || *c < 0 || (*d <= 1 && *a * std::max(*d, 0.0) + *b < 0)) {
        return unsupported_icc();
      }
      if (*d > 0 && *d <= 1) {
        auto left = evaluate_icc_curve(view, tag, std::nextafter(*d, 0.0), true);
        auto right = evaluate_icc_curve(view, tag, *d, true);
        if (!left || !right || *left > *right) { return unsupported_icc(); }
      }
    }
  }
  return Error::Ok;
}

Result<double> inverse_rgb_curve(const IccView& view, const IccTag& tag, double value)
{
  if (!std::isfinite(value)) { return invalid_value(); }
  auto first = evaluate_icc_curve(view, tag, 0, true);
  auto last = evaluate_icc_curve(view, tag, 1, true);
  if (!first) { return first.error(); }
  if (!last) { return last.error(); }
  const double target = std::clamp(value, *first, *last);
  uint32_t type = 0;
  read_u32(*view.bytes, tag.offset, type);
  if (type == icc_sig('c', 'u', 'r', 'v')) {
    uint32_t count = 0;
    read_u32(*view.bytes, tag.offset + 8, count);
    if (count == 0) { return target; }
    if (count == 1) {
      uint16_t gamma = 0;
      read_u16(*view.bytes, tag.offset + 12, gamma);
      return std::pow(target, 256.0 / gamma);
    }
    // Find the table interval in O(log n), retaining exact plateau endpoints.
    const auto entry = [&view, &tag](uint32_t i) {
      uint16_t sample = 0;
      read_u16(*view.bytes, tag.offset + 12 + size_t(i) * 2, sample);
      return sample / 65535.0;
    };
    uint32_t low = 0, high = count - 1;
    if (target == *last) {
      while (low < high) {
        const uint32_t middle = low + (high - low) / 2;
        if (entry(middle) < target) { low = middle + 1; }
        else { high = middle; }
      }
      return static_cast<double>(low) / (count - 1);
    }
    while (low < high) {
      const uint32_t middle = low + (high - low + 1) / 2;
      if (entry(middle) <= target) { low = middle; }
      else { high = middle - 1; }
    }
    const double a = entry(low), b = entry(low + 1);
    return (low + (target - a) / (b - a)) / (count - 1);
  }
  else {
    uint16_t function = 0;
    read_u16(*view.bytes, tag.offset + 8, function);
    constexpr std::array<size_t, 5> parameter_counts{1, 3, 4, 5, 7};
    std::array<double, 7> p{};
    for (size_t i = 0; i < parameter_counts[function]; ++i) {
      auto parameter = read_s15_fixed16(*view.bytes, tag.offset + 12 + i * 4);
      if (!parameter) { return parameter.error(); }
      p[i] = *parameter;
    }
    if (function == 0) { return std::pow(target, 1.0 / p[0]); }
    const double high_offset = function == 2 ? p[3] : (function == 4 ? p[5] : 0);
    const double breakpoint = function <= 2 ? -p[2] / p[1] : p[4];
    const double high_value = (std::pow(std::max(target - high_offset, 0.0), 1.0 / p[0]) - p[2]) / p[1];
    if (function <= 2) { return std::clamp(high_value, 0.0, 1.0); }
    const double low_offset = function == 4 ? p[6] : 0;
    const double low_end = std::clamp(p[3] * breakpoint + low_offset, 0.0, 1.0);
    if (breakpoint > 0 && (breakpoint > 1 || target < low_end || (target == *last && target <= low_end))) {
      const double low_value = p[3] > 0 ? (target - low_offset) / p[3] : breakpoint;
      return std::clamp(low_value, 0.0, std::min(breakpoint, 1.0));
    }
    return std::clamp(high_value, std::clamp(breakpoint, 0.0, 1.0), 1.0);
  }
}

Result<GainMapMatrix> nclx_to_pcs(const nclx_profile& profile)
{
  auto xyz = to_xyz(profile.m_colour_primaries);
  if (!xyz) { return xyz.error(); }
  // Bradford adaptation from the declared white to ICC's D50 PCS white.
  // Compute in double precision instead of reusing an s15Fixed16 'chad' tag:
  // its rounding is amplified by inverse TRCs near dark channel values.
  const GainMapMatrix adaptation = adapt_white(adopted_white(profile.m_colour_primaries), {0.9642, 1, 0.8249});
  return multiply(adaptation, *xyz);
}
}  // namespace

Result<GainMapColour> GainMapColour::from_icc(const std::shared_ptr<const color_profile_raw>& profile,
                                           uint32_t diffuse_white)
{
  if (!profile) { return unsupported_icc(); }
  auto view = parse_icc(*profile);
  if (!view) { return view.error(); }
  uint32_t magic = 0;
  if (!read_u32(profile->get_data(), 36, magic) || magic != icc_sig('a', 'c', 's', 'p')) {
    return malformed_icc();
  }
  if (const auto* cicp = view->find(icc_sig('c', 'i', 'c', 'p'))) {
    auto nclx = nclx_from_cicp(*view, *cicp);
    if (!nclx) { return nclx.error(); }
    GainMapColour result(*nclx, diffuse_white);
    result.m_profile = profile;
    return result;
  }
  if (view->data_space != icc_sig('R', 'G', 'B', ' ') ||
      (view->pcs != icc_sig('X', 'Y', 'Z', ' ') && view->pcs != icc_sig('L', 'a', 'b', ' ')) ||
      (view->profile_class != icc_sig('m', 'n', 't', 'r') &&
       view->profile_class != icc_sig('s', 'c', 'n', 'r') &&
       view->profile_class != icc_sig('p', 'r', 't', 'r'))) {
    return unsupported_icc();
  }
  // Respect ICC LUT precedence even when a matrix/TRC model is also present.
  bool lut = false;
  for (char i : {'0', '1', '2', '3'}) {
    for (uint32_t tag : {icc_sig('A', '2', 'B', i), icc_sig('B', '2', 'A', i),
                         icc_sig('D', '2', 'B', i), icc_sig('B', '2', 'D', i)}) {
      lut |= view->find(tag) != nullptr;
    }
  }
  auto transform = std::make_shared<GainMapIccColour>();
  transform->view = *view;
  for (size_t c = 0; c < 3; ++c) {
    const char name = "rgb"[c];
    const auto* xyz = view->find(icc_sig(name, 'X', 'Y', 'Z'));
    const auto* curve = view->find(icc_sig(name, 'T', 'R', 'C'));
    // ISO 21496-1 Annex B needs actual RGB application primaries. PCS XYZ/Lab
    // alone is not that space; do not substitute sRGB for absent colourants.
    if (!xyz || (!lut && !curve)) { return unsupported_icc(); }
    auto column = parse_xyz_tag(*view, *xyz);
    if (!column) { return column.error(); }
    if (!lut) {
      if (auto error = validate_rgb_curve(*view, *curve)) { return error; }
      transform->curves[c] = *curve;
    }
    for (size_t row = 0; row < 3; ++row) { transform->to_pcs[row][c] = (*column)[row]; }
  }
  const double det = determinant(transform->to_pcs);
  if (!std::isfinite(det) || std::abs(det) < 1e-12) { return unsupported_icc(); }
  if (lut) {
#if HAVE_LCMS2
    transform->context = cmsCreateContext(nullptr, nullptr);
    if (!transform->context) { return unsupported_icc(); }
    // Per-decode context and uncached transforms avoid shared CMM state during
    // parallel image decoding. Little CMS validates the bounded tag payloads.
    cmsSetLogErrorHandlerTHR(transform->context, [](cmsContext, cmsUInt32Number, const char*) {});
    transform->lut_profile = cmsOpenProfileFromMemTHR(transform->context, view->bytes->data(),
                                                     static_cast<cmsUInt32Number>(view->profile_size));
    transform->xyz_profile = cmsCreateXYZProfileTHR(transform->context);
    if (!transform->lut_profile || !transform->xyz_profile) { return malformed_icc(); }
    constexpr auto flags = cmsFLAGS_NOOPTIMIZE | cmsFLAGS_NOCACHE;
    transform->decode_lut = cmsCreateTransformTHR(transform->context, transform->lut_profile, TYPE_RGB_DBL,
        transform->xyz_profile, TYPE_XYZ_DBL, INTENT_RELATIVE_COLORIMETRIC, flags);
    transform->encode_lut = cmsCreateTransformTHR(transform->context, transform->xyz_profile, TYPE_XYZ_DBL,
        transform->lut_profile, TYPE_RGB_DBL, INTENT_RELATIVE_COLORIMETRIC, flags);
    if (!transform->decode_lut || !transform->encode_lut) { return unsupported_icc(); }
    transform->from_pcs = inverse(transform->to_pcs);
#else
    return unsupported_icc();
#endif
  }
  else if (view->pcs != icc_sig('X', 'Y', 'Z', ' ')) { return unsupported_icc(); }
  nclx_profile raster = nclx_profile::undefined();
  raster.set_matrix_coefficients(0);
  raster.set_full_range_flag(true);
  GainMapColour result(raster, diffuse_white);
  result.m_profile = profile;
  result.m_matrix_trc = std::move(transform);
  return result;
}

Result<GainMapRGB> GainMapColour::decode(const GainMapRGB& signal) const
{
  if (!m_matrix_trc) {
    auto result = gain_map_decode_rgb(signal, m_nclx);
    if (!result) { return result.error(); }
    const auto transfer = m_nclx.m_transfer_characteristics;
    if (m_diffuse_white && (transfer == 16 || transfer == 17 || transfer == 18)) {
      for (auto& value : *result) { value *= 2030000.0 / m_diffuse_white; }
    }
    return result;
  }
  GainMapRGB result{};
#if HAVE_LCMS2
  if (m_matrix_trc->decode_lut) {
    for (double value : signal) { if (!std::isfinite(value)) { return invalid_value(); } }
    cmsDoTransform(m_matrix_trc->decode_lut, signal.data(), result.data(), 1);
    result = gain_map_transform(m_matrix_trc->from_pcs, result);
    for (double value : result) { if (!std::isfinite(value)) { return invalid_value(); } }
    return result;
  }
#endif
  for (size_t c = 0; c < 3; ++c) {
    if (!std::isfinite(signal[c])) { return invalid_value(); }
    auto value = evaluate_icc_curve(m_matrix_trc->view, m_matrix_trc->curves[c],
                                    std::clamp(signal[c], 0.0, 1.0), true);
    if (!value) { return value.error(); }
    result[c] = *value;
  }
  return result;
}

Result<GainMapRGB> GainMapColour::encode(const GainMapRGB& linear) const
{
  if (!m_matrix_trc) {
    auto normalized = linear;
    const auto transfer = m_nclx.m_transfer_characteristics;
    if (m_diffuse_white && (transfer == 16 || transfer == 17 || transfer == 18)) {
      for (auto& value : normalized) { value *= m_diffuse_white / 2030000.0; }
    }
    return gain_map_encode_rgb(normalized, m_nclx);
  }
  GainMapRGB result{};
#if HAVE_LCMS2
  if (m_matrix_trc->encode_lut) {
    const auto pcs = gain_map_transform(m_matrix_trc->to_pcs, linear);
    for (double value : pcs) { if (!std::isfinite(value)) { return invalid_value(); } }
    cmsDoTransform(m_matrix_trc->encode_lut, pcs.data(), result.data(), 1);
    for (double value : result) { if (!std::isfinite(value)) { return invalid_value(); } }
    return result;
  }
#endif
  for (size_t c = 0; c < 3; ++c) {
    auto value = inverse_rgb_curve(m_matrix_trc->view, m_matrix_trc->curves[c], linear[c]);
    if (!value) { return value.error(); }
    result[c] = *value;
  }
  return result;
}

Result<GainMapMatrix> GainMapColour::matrix_to(const GainMapColour& target) const
{
  if (!m_matrix_trc && !target.m_matrix_trc) {
    return gain_map_primaries_matrix(m_nclx.m_colour_primaries, target.m_nclx.m_colour_primaries);
  }
  if (m_matrix_trc && m_matrix_trc == target.m_matrix_trc) {
    return GainMapMatrix{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
  }
  auto source_pcs = m_matrix_trc ? Result<GainMapMatrix>(m_matrix_trc->to_pcs) : nclx_to_pcs(m_nclx);
  auto target_pcs = target.m_matrix_trc ? Result<GainMapMatrix>(target.m_matrix_trc->to_pcs) : nclx_to_pcs(target.m_nclx);
  if (!source_pcs) { return source_pcs.error(); }
  if (!target_pcs) { return target_pcs.error(); }
  return multiply(inverse(*target_pcs), *source_pcs);
}
