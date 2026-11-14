/*
  libheif GB/T 48347-2026 dual-layer metadata tests

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

// Golden vectors below are hand-assembled from the GB/T 48347-2026 syntax
// tables (big-endian), not produced by the parser under test. IEEE-754
// binary16 encodings are checked against the standard's fixed values.

#include "catch_amalgamated.hpp"
#include "gbt48347.h"

#include <cstdint>
#include <cstring>
#include <vector>

using gbt48347::UWAInfo;
using gbt48347::ParseStatus;

namespace
{
std::vector<uint8_t> bytes(std::initializer_list<unsigned> list)
{
  std::vector<uint8_t> out;
  out.reserve(list.size());
  for (unsigned b : list) {
    out.push_back(static_cast<uint8_t>(b));
  }
  return out;
}

// UWAInfo header, big-endian: country=0x26, provider=0x0004, oriented=0x0007,
// frame_number, extended_type.
std::vector<uint8_t> header(uint8_t frame, uint32_t extended_type)
{
  return bytes({0x26, 0x00, 0x04, 0x00, 0x07, frame,
                (extended_type >> 24) & 0xFF, (extended_type >> 16) & 0xFF,
                (extended_type >> 8) & 0xFF, extended_type & 0xFF});
}

void append(std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
  a.insert(a.end(), b.begin(), b.end());
}
}  // namespace


TEST_CASE("GB/T UWAInfo minimal golden vector parses and round-trips")
{
  // file_type=2, meta_type=0, enhance_type=0 -> no optional blocks.
  const auto golden = header(2, 0x02000000);
  REQUIRE(golden.size() == 10);

  auto result = gbt48347::parse_uwa_info(golden);
  REQUIRE(result.status == ParseStatus::parsed);
  REQUIRE(result.bytes_consumed == golden.size());
  const auto& info = *result.info;
  CHECK(info.country_code == 0x26);
  CHECK(info.terminal_provide_code == 0x0004);
  CHECK(info.terminal_provide_oriented_code == 0x0007);
  CHECK(info.extended_frame_number == 2);
  CHECK(info.extended_type == 0x02000000);
  CHECK_FALSE(info.metadata_present);
  CHECK_FALSE(info.extension_present);

  auto serialized = gbt48347::serialize_uwa_info(info);
  REQUIRE(serialized);
  CHECK(*serialized == golden);
}


TEST_CASE("GB/T UWAInfo extended_type bit fields")
{
  CHECK(gbt48347::extended_file_type(0x01000000) == 1);
  CHECK(gbt48347::extended_meta_type(0x01000000) == 0);
  CHECK(gbt48347::extended_enhance_type(0x01000000) == 0);
  CHECK(gbt48347::extended_file_type(0x02010100) == 2);
  CHECK(gbt48347::extended_meta_type(0x02010100) == 1);
  CHECK(gbt48347::extended_enhance_type(0x02010100) == 1);
  CHECK(gbt48347::extended_enhance_type(0x02010000) == 0);
}


TEST_CASE("GB/T UWAInfo static and dynamic metadata golden vector")
{
  // meta_type=1, enhance_type=0. static={AA,BB}, dynamic={CC}.
  auto golden = header(2, 0x02010000);
  append(golden, bytes({0x00, 0x07,        // metadata_size = 2+2+2+1
                        0x00, 0x02, 0xAA, 0xBB,
                        0x00, 0x01, 0xCC}));

  auto result = gbt48347::parse_uwa_info(golden);
  REQUIRE(result.status == ParseStatus::parsed);
  const auto& info = *result.info;
  REQUIRE(info.metadata_present);
  CHECK(info.static_metadata == bytes({0xAA, 0xBB}));
  CHECK(info.dynamic_metadata == bytes({0xCC}));
  CHECK_FALSE(info.extension_present);

  auto serialized = gbt48347::serialize_uwa_info(info);
  REQUIRE(serialized);
  CHECK(*serialized == golden);
}


TEST_CASE("GB/T UWAInfo rejects wrong T.35 identifiers")
{
  auto bad_country = header(2, 0x02000000);
  bad_country[0] = 0x27;
  CHECK(gbt48347::parse_uwa_info(bad_country).status == ParseStatus::malformed);

  auto bad_provider = header(2, 0x02000000);
  bad_provider[2] = 0x05;
  CHECK(gbt48347::parse_uwa_info(bad_provider).status == ParseStatus::malformed);

  auto bad_oriented = header(2, 0x02000000);
  bad_oriented[4] = 0x08;
  CHECK(gbt48347::parse_uwa_info(bad_oriented).status == ParseStatus::malformed);
}


TEST_CASE("GB/T UWAInfo every truncated header prefix is rejected")
{
  const auto full = header(2, 0x02000000);
  for (size_t n = 0; n < full.size(); ++n) {
    const std::vector<uint8_t> prefix(full.begin(), full.begin() + n);
    auto result = gbt48347::parse_uwa_info(prefix);
    CHECK(result.status == ParseStatus::malformed);
  }
}


TEST_CASE("GB/T UWAInfo metadata_size out of range is rejected")
{
  // payloads = 2 + 1 = 3; accepted metadata_size in [3,7]. 8 is out of range.
  auto golden = header(2, 0x02010000);
  append(golden, bytes({0x00, 0x08, 0x00, 0x02, 0xAA, 0xBB, 0x00, 0x01, 0xCC}));
  CHECK(gbt48347::parse_uwa_info(golden).status == ParseStatus::malformed);

  // The alternate reading (payloads only = 3) is accepted.
  auto alt = header(2, 0x02010000);
  append(alt, bytes({0x00, 0x03, 0x00, 0x02, 0xAA, 0xBB, 0x00, 0x01, 0xCC}));
  CHECK(gbt48347::parse_uwa_info(alt).status == ParseStatus::parsed);
}


TEST_CASE("GB/T UWAInfo truncated metadata payload is rejected")
{
  // static_size claims 4 bytes but only 2 follow.
  auto golden = header(2, 0x02010000);
  append(golden, bytes({0x00, 0x07, 0x00, 0x04, 0xAA, 0xBB}));
  CHECK(gbt48347::parse_uwa_info(golden).status == ParseStatus::malformed);
}


TEST_CASE("GB/T extend_info_extension empty blocks")
{
  // base size=0, combined size=0.
  const auto golden = bytes({0x00, 0x00, 0x00, 0x00});
  auto result = gbt48347::parse_extend_info_extension(golden);
  REQUIRE(result.status == ParseStatus::parsed);
  CHECK_FALSE(result.extension->base_present);
  CHECK_FALSE(result.extension->combined_present);
  auto serialized = gbt48347::serialize_extend_info_extension(*result.extension);
  REQUIRE(serialized);
  CHECK(*serialized == golden);
}


TEST_CASE("GB/T mapping flag 0 is a one-byte absent block")
{
  const auto golden = bytes({0x00, 0x01, 0x00, 0x00, 0x00});
  auto result = gbt48347::parse_extend_info_extension(golden);
  REQUIRE(result.status == ParseStatus::parsed);
  REQUIRE(result.extension->base_present);
  CHECK(result.extension->base.flag == 0);
  CHECK(result.extension->base.matrix.empty());
  CHECK_FALSE(result.extension->combined_present);
}


TEST_CASE("GB/T fp32 identity matrix golden vector")
{
  // combined block: flag=32, 3x3 identity, no LUT -> 1 + 9*4 = 37 bytes.
  std::vector<uint8_t> block = {0x20};
  const float identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  for (float v : identity) {
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    block.push_back((bits >> 24) & 0xFF);
    block.push_back((bits >> 16) & 0xFF);
    block.push_back((bits >> 8) & 0xFF);
    block.push_back(bits & 0xFF);
  }
  std::vector<uint8_t> golden = {0x00, 0x00,  // base size 0
                                 0x00, 0x25};  // combined size 37
  append(golden, block);

  auto result = gbt48347::parse_extend_info_extension(golden);
  REQUIRE(result.status == ParseStatus::parsed);
  const auto& combined = result.extension->combined;
  REQUIRE(combined.flag == 32);
  REQUIRE(combined.matrix.size() == 9);
  for (int i = 0; i < 9; ++i) {
    CHECK(combined.matrix[i] == identity[i]);
  }
  CHECK(combined.lut_order == 0);
  CHECK(combined.lut.empty());

  auto serialized = gbt48347::serialize_extend_info_extension(*result.extension);
  REQUIRE(serialized);
  CHECK(*serialized == golden);
}


TEST_CASE("GB/T fp16 matrix uses IEEE binary16 encodings")
{
  // flag=16, matrix = [1.0, 0.5, -2.0, 0,0,0, 0,0,0].
  // Standard binary16: 1.0=0x3C00, 0.5=0x3800, -2.0=0xC000, 0.0=0x0000.
  std::vector<uint8_t> block = {0x10,
                                0x3C, 0x00, 0x38, 0x00, 0xC0, 0x00,
                                0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  std::vector<uint8_t> golden = {0x00, 0x13};  // base size 19
  append(golden, block);
  append(golden, bytes({0x00, 0x00}));  // combined size 0

  auto result = gbt48347::parse_extend_info_extension(golden);
  REQUIRE(result.status == ParseStatus::parsed);
  const auto& base = result.extension->base;
  REQUIRE(base.flag == 16);
  REQUIRE(base.matrix.size() == 9);
  CHECK(base.matrix[0] == 1.0f);
  CHECK(base.matrix[1] == 0.5f);
  CHECK(base.matrix[2] == -2.0f);

  auto serialized = gbt48347::serialize_extend_info_extension(*result.extension);
  REQUIRE(serialized);
  CHECK(*serialized == golden);
}


TEST_CASE("GB/T 3D LUT order derived from block length")
{
  // flag=32, identity matrix (36 bytes), LUT N=2 -> 8 floats (32 bytes).
  std::vector<uint8_t> block = {0x20};
  const float identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  auto push_f32 = [&](float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    block.push_back((bits >> 24) & 0xFF);
    block.push_back((bits >> 16) & 0xFF);
    block.push_back((bits >> 8) & 0xFF);
    block.push_back(bits & 0xFF);
  };
  for (float v : identity) {
    push_f32(v);
  }
  for (int i = 0; i < 8; ++i) {
    push_f32(static_cast<float>(i) / 8.0f);
  }
  REQUIRE(block.size() == 1 + 36 + 32);
  std::vector<uint8_t> golden = {0x00, 0x00, 0x00, static_cast<uint8_t>(block.size())};
  append(golden, block);

  auto result = gbt48347::parse_extend_info_extension(golden);
  REQUIRE(result.status == ParseStatus::parsed);
  const auto& combined = result.extension->combined;
  CHECK(combined.lut_order == 2);
  REQUIRE(combined.lut.size() == 8);
  CHECK(combined.lut[7] == 7.0f / 8.0f);
}


TEST_CASE("GB/T mapping rejects invalid flag and non-cube LUT")
{
  // flag=8 is not a defined fp width.
  auto bad_flag = bytes({0x00, 0x01, 0x08, 0x00, 0x00});
  CHECK(gbt48347::parse_extend_info_extension(bad_flag).status == ParseStatus::malformed);

  // flag=32, matrix present, then 3 leftover floats (not a perfect cube).
  std::vector<uint8_t> block = {0x20};
  for (int i = 0; i < 9 + 3; ++i) {
    block.insert(block.end(), {0x00, 0x00, 0x00, 0x00});
  }
  std::vector<uint8_t> golden = {0x00, 0x00, 0x00, static_cast<uint8_t>(block.size())};
  append(golden, block);
  CHECK(gbt48347::parse_extend_info_extension(golden).status == ParseStatus::malformed);
}


TEST_CASE("GB/T UWAInfo carries extend_info_extension")
{
  // enhance_type=1, meta_type=0. Extension = two empty blocks (4 bytes).
  auto golden = header(2, 0x02000100);
  append(golden, bytes({0x00, 0x04, 0x00, 0x00, 0x00, 0x00}));

  auto result = gbt48347::parse_uwa_info(golden);
  REQUIRE(result.status == ParseStatus::parsed);
  REQUIRE(result.info->extension_present);
  CHECK_FALSE(result.info->extension.base_present);
  CHECK_FALSE(result.info->extension.combined_present);

  auto serialized = gbt48347::serialize_uwa_info(*result.info);
  REQUIRE(serialized);
  CHECK(*serialized == golden);
}


TEST_CASE("GB/T UWAInfo serializer rejects flag/payload disagreement")
{
  UWAInfo info;
  info.extended_type = 0x02000000;  // meta=0, enhance=0
  info.metadata_present = true;     // disagrees with extended_type
  auto result = gbt48347::serialize_uwa_info(info);
  CHECK_FALSE(result);
}