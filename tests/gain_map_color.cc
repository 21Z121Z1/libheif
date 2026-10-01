/*
  libheif ICC colour bridge tests for ISO 21496-1 tone-map reconstruction

  MIT License

  Copyright (c) 2026 libheif contributors

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
*/

#include "catch_amalgamated.hpp"
#include "gain_map_color.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

uint32_t signature(const char* text)
{
  return (static_cast<uint32_t>(static_cast<uint8_t>(text[0])) << 24) |
         (static_cast<uint32_t>(static_cast<uint8_t>(text[1])) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(text[2])) << 8) |
         static_cast<uint32_t>(static_cast<uint8_t>(text[3]));
}

void put_u16(std::vector<uint8_t>& data, uint16_t value)
{
  data.push_back(static_cast<uint8_t>(value >> 8));
  data.push_back(static_cast<uint8_t>(value));
}

void put_u32(std::vector<uint8_t>& data, uint32_t value)
{
  data.push_back(static_cast<uint8_t>(value >> 24));
  data.push_back(static_cast<uint8_t>(value >> 16));
  data.push_back(static_cast<uint8_t>(value >> 8));
  data.push_back(static_cast<uint8_t>(value));
}

void set_u32(std::vector<uint8_t>& data, size_t offset, uint32_t value)
{
  REQUIRE(offset + 4 <= data.size());
  data[offset] = static_cast<uint8_t>(value >> 24);
  data[offset + 1] = static_cast<uint8_t>(value >> 16);
  data[offset + 2] = static_cast<uint8_t>(value >> 8);
  data[offset + 3] = static_cast<uint8_t>(value);
}

void put_fixed(std::vector<uint8_t>& data, double value)
{
  const int64_t fixed = std::llround(value * 65536.0);
  put_u32(data, static_cast<uint32_t>(fixed));
}

std::vector<uint8_t> xyz_tag(const std::array<double, 3>& xyz)
{
  std::vector<uint8_t> data;
  put_u32(data, signature("XYZ "));
  put_u32(data, 0);
  for (double value : xyz) {
    put_fixed(data, value);
  }
  return data;
}

std::vector<uint8_t> chad_tag(const std::array<std::array<double, 3>, 3>& matrix)
{
  std::vector<uint8_t> data;
  put_u32(data, signature("sf32"));
  put_u32(data, 0);
  for (const auto& row : matrix) {
    for (double value : row) {
      put_fixed(data, value);
    }
  }
  return data;
}

std::vector<uint8_t> srgb_trc_tag()
{
  std::vector<uint8_t> data;
  put_u32(data, signature("para"));
  put_u32(data, 0);
  put_u16(data, 3);
  put_u16(data, 0);
  for (double value : {2.4, 1.0 / 1.055, 0.055 / 1.055,
                       1.0 / 12.92, 0.04045}) {
    put_fixed(data, value);
  }
  return data;
}

std::vector<uint8_t> cicp_tag(uint8_t primaries, uint8_t transfer,
                              uint8_t matrix, uint8_t full_range)
{
  std::vector<uint8_t> data;
  put_u32(data, signature("cicp"));
  put_u32(data, 0);
  data.push_back(primaries);
  data.push_back(transfer);
  data.push_back(matrix);
  data.push_back(full_range);
  return data;
}

struct TestTag
{
  uint32_t signature;
  std::vector<uint8_t> payload;
};

std::vector<uint8_t> make_icc(const std::vector<TestTag>& tags)
{
  std::vector<uint8_t> data(132 + tags.size() * 12, 0);
  set_u32(data, 12, signature("mntr"));
  set_u32(data, 16, signature("RGB "));
  set_u32(data, 20, signature("XYZ "));
  set_u32(data, 36, signature("acsp"));
  set_u32(data, 128, static_cast<uint32_t>(tags.size()));

  for (size_t i = 0; i < tags.size(); ++i) {
    while ((data.size() & 3U) != 0) {
      data.push_back(0);
    }
    const uint32_t offset = static_cast<uint32_t>(data.size());
    data.insert(data.end(), tags[i].payload.begin(), tags[i].payload.end());
    const size_t entry = 132 + i * 12;
    set_u32(data, entry, tags[i].signature);
    set_u32(data, entry + 4, offset);
    set_u32(data, entry + 8, static_cast<uint32_t>(tags[i].payload.size()));
  }
  set_u32(data, 0, static_cast<uint32_t>(data.size()));
  return data;
}

std::array<std::array<double, 3>, 3> multiply(
    const std::array<std::array<double, 3>, 3>& a,
    const std::array<std::array<double, 3>, 3>& b)
{
  std::array<std::array<double, 3>, 3> result{};
  for (size_t y = 0; y < 3; ++y) {
    for (size_t x = 0; x < 3; ++x) {
      for (size_t k = 0; k < 3; ++k) {
        result[y][x] += a[y][k] * b[k][x];
      }
    }
  }
  return result;
}

}  // namespace

TEST_CASE("ICC CICP tag maps Display-P3 PQ directly")
{
  const auto bytes = make_icc({
      {signature("cicp"), cicp_tag(12, 16, 0, 1)}
  });
  const color_profile_raw profile(signature("prof"), bytes);
  auto result = gain_map_nclx_from_icc(profile);
  REQUIRE(result);
  REQUIRE(result->m_colour_primaries == 12);
  REQUIRE(result->m_transfer_characteristics == 16);
  REQUIRE(result->m_matrix_coefficients == 0);
  REQUIRE(result->m_full_range_flag);
}

TEST_CASE("ICC matrix-shaper Display-P3 with sRGB TRC maps to CICP")
{
  const std::array<std::array<double, 3>, 3> p3{{
      {0.4865709486, 0.2656676932, 0.1982172852},
      {0.2289745641, 0.6917385218, 0.0792869141},
      {0.0, 0.0451133819, 1.0439443689}}};
  const std::array<std::array<double, 3>, 3> d65_to_d50{{
      {1.04788208, 0.02291870, -0.05020142},
      {0.02958679, 0.99047852, -0.01705933},
      {-0.00923157, 0.01507568, 0.75167847}}};
  const auto adapted = multiply(d65_to_d50, p3);
  const auto trc = srgb_trc_tag();

  const auto bytes = make_icc({
      {signature("rXYZ"), xyz_tag({adapted[0][0], adapted[1][0], adapted[2][0]})},
      {signature("gXYZ"), xyz_tag({adapted[0][1], adapted[1][1], adapted[2][1]})},
      {signature("bXYZ"), xyz_tag({adapted[0][2], adapted[1][2], adapted[2][2]})},
      {signature("chad"), chad_tag(d65_to_d50)},
      {signature("rTRC"), trc},
      {signature("gTRC"), trc},
      {signature("bTRC"), trc}
  });

  const color_profile_raw profile(signature("prof"), bytes);
  auto result = gain_map_nclx_from_icc(profile);
  REQUIRE(result);
  REQUIRE(result->m_colour_primaries == 12);
  REQUIRE(result->m_transfer_characteristics == 13);
  REQUIRE(result->m_matrix_coefficients == 0);
  REQUIRE(result->m_full_range_flag);
}

TEST_CASE("ICC tag bounds are checked before colour interpretation")
{
  auto bytes = make_icc({
      {signature("cicp"), cicp_tag(12, 16, 0, 1)}
  });
  set_u32(bytes, 136, 0xFFFFFF00U);
  const color_profile_raw profile(signature("prof"), bytes);
  auto result = gain_map_nclx_from_icc(profile);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().error_code == heif_error_Invalid_input);
}

TEST_CASE("Arbitrary ICC LUT profile is not guessed as CICP")
{
  std::vector<uint8_t> lut(32, 0);
  set_u32(lut, 0, signature("mAB "));
  const auto bytes = make_icc({
      {signature("A2B0"), lut}
  });
  const color_profile_raw profile(signature("prof"), bytes);
  auto result = gain_map_nclx_from_icc(profile);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().error_code == heif_error_Unsupported_feature);
  REQUIRE(result.error().sub_error_code ==
          heif_suberror_Unsupported_color_conversion);
}
