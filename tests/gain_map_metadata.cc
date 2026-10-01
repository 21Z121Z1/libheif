/*
  libheif ISO 21496-1 gain map metadata tests

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
#include "gain_map_metadata.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>


namespace
{

const std::vector<uint8_t> kMonoGolden = {
    0x00, 0x00, 0x00, 0x00, 0x40,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x02,
    0x00, 0x00, 0x00, 0x01,
    0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0x00, 0x00, 0x02,
    0x00, 0x00, 0x00, 0x03,
    0x00, 0x00, 0x00, 0x02,
    0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x01,
    0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x08
};


GainMapMetadata make_valid_metadata()
{
  GainMapMetadata metadata;
  metadata.version.minimum_version = 0;
  metadata.version.writer_version = 0;
  metadata.channel_count = 1;
  metadata.use_base_colour_space = true;
  metadata.base_hdr_headroom = {0, 1};
  metadata.alternate_hdr_headroom = {2, 1};

  metadata.channels[0].gain_map_min = {-1, 2};
  metadata.channels[0].gain_map_max = {3, 2};
  metadata.channels[0].gamma = {1, 1};
  metadata.channels[0].base_offset = {-1, 4};
  metadata.channels[0].alternate_offset = {1, 8};
  return metadata;
}


void write_u32(std::vector<uint8_t>* data,
               size_t offset,
               uint32_t value)
{
  REQUIRE(offset + 4 <= data->size());
  (*data)[offset] = static_cast<uint8_t>(value >> 24);
  (*data)[offset + 1] = static_cast<uint8_t>(value >> 16);
  (*data)[offset + 2] = static_cast<uint8_t>(value >> 8);
  (*data)[offset + 3] = static_cast<uint8_t>(value);
}

}  // namespace


TEST_CASE("gain map metadata mono v0 golden vector")
{
  REQUIRE(kMonoGolden.size() == 61);

  GainMapMetadataParseResult parsed =
      parse_gain_map_metadata(kMonoGolden);

  REQUIRE(parsed.status == GainMapMetadataParseStatus::parsed);
  REQUIRE(parsed.metadata.has_value());
  REQUIRE(parsed.bytes_consumed == 61);

  const GainMapMetadata& metadata = *parsed.metadata;
  REQUIRE(metadata.version.minimum_version == 0);
  REQUIRE(metadata.version.writer_version == 0);
  REQUIRE(metadata.channel_count == 1);
  REQUIRE(metadata.use_base_colour_space);
  REQUIRE(metadata.base_hdr_headroom.numerator == 0);
  REQUIRE(metadata.base_hdr_headroom.denominator == 1);
  REQUIRE(metadata.alternate_hdr_headroom.numerator == 2);
  REQUIRE(metadata.alternate_hdr_headroom.denominator == 1);

  REQUIRE(metadata.channels[0].gain_map_min.numerator == -1);
  REQUIRE(metadata.channels[0].gain_map_min.denominator == 2);
  REQUIRE(metadata.channels[0].gain_map_max.numerator == 3);
  REQUIRE(metadata.channels[0].gain_map_max.denominator == 2);
  REQUIRE(metadata.channels[0].gamma.numerator == 1);
  REQUIRE(metadata.channels[0].gamma.denominator == 1);
  REQUIRE(metadata.channels[0].base_offset.numerator == -1);
  REQUIRE(metadata.channels[0].base_offset.denominator == 4);
  REQUIRE(metadata.channels[0].alternate_offset.numerator == 1);
  REQUIRE(metadata.channels[0].alternate_offset.denominator == 8);

  auto encoded = serialize_gain_map_metadata(metadata);
  REQUIRE(encoded);
  REQUIRE(*encoded == kMonoGolden);
}


TEST_CASE("gain map metadata RGB channel order")
{
  GainMapMetadata metadata = make_valid_metadata();
  metadata.channel_count = 3;
  metadata.use_base_colour_space = false;

  metadata.channels[0].gain_map_min = {-1, 1};
  metadata.channels[0].gain_map_max = {1, 1};
  metadata.channels[1].gain_map_min = {-2, 3};
  metadata.channels[1].gain_map_max = {2, 3};
  metadata.channels[2].gain_map_min = {-3, 5};
  metadata.channels[2].gain_map_max = {3, 5};

  auto encoded = serialize_gain_map_metadata(metadata);
  REQUIRE(encoded);
  REQUIRE(encoded->size() == 141);
  REQUIRE((*encoded)[4] == 0x80);

  GainMapMetadataParseResult parsed =
      parse_gain_map_metadata(*encoded);
  REQUIRE(parsed.status == GainMapMetadataParseStatus::parsed);
  REQUIRE(parsed.metadata.has_value());
  REQUIRE(parsed.metadata->channel_count == 3);

  REQUIRE(parsed.metadata->channels[0].gain_map_min.numerator == -1);
  REQUIRE(parsed.metadata->channels[0].gain_map_min.denominator == 1);
  REQUIRE(parsed.metadata->channels[1].gain_map_min.numerator == -2);
  REQUIRE(parsed.metadata->channels[1].gain_map_min.denominator == 3);
  REQUIRE(parsed.metadata->channels[2].gain_map_min.numerator == -3);
  REQUIRE(parsed.metadata->channels[2].gain_map_min.denominator == 5);
}


TEST_CASE("gain map metadata accepts future writer and trailing data")
{
  std::vector<uint8_t> data = kMonoGolden;
  data[2] = 0x12;
  data[3] = 0x34;
  data.insert(data.end(), {0xAA, 0xBB, 0xCC, 0xDD});

  GainMapMetadataParseResult parsed =
      parse_gain_map_metadata(data);

  REQUIRE(parsed.status == GainMapMetadataParseStatus::parsed);
  REQUIRE(parsed.metadata.has_value());
  REQUIRE(parsed.metadata->version.minimum_version == 0);
  REQUIRE(parsed.metadata->version.writer_version == 0x1234);
  REQUIRE(parsed.bytes_consumed == 61);
}


TEST_CASE("gain map metadata unknown minimum version is not parsed")
{
  const std::vector<uint8_t> data = {
      0x00, 0x01, 0x00, 0x01, 0xDE, 0xAD, 0xBE, 0xEF
  };

  GainMapMetadataParseResult parsed =
      parse_gain_map_metadata(data);

  REQUIRE(parsed.status ==
          GainMapMetadataParseStatus::unsupported_minimum_version);
  REQUIRE(!parsed.metadata.has_value());
  REQUIRE(!parsed.error);
  REQUIRE(parsed.version.minimum_version == 1);
  REQUIRE(parsed.version.writer_version == 1);
  REQUIRE(parsed.bytes_consumed == 4);
}


TEST_CASE("gain map writer version below minimum is malformed")
{
  const std::vector<uint8_t> data = {
      0x00, 0x01, 0x00, 0x00
  };

  GainMapMetadataParseResult parsed =
      parse_gain_map_metadata(data);

  REQUIRE(parsed.status == GainMapMetadataParseStatus::malformed);
  REQUIRE(parsed.error.error_code == heif_error_Invalid_input);
}


TEST_CASE("gain map metadata rejects every zero denominator")
{
  constexpr std::array<size_t, 7> denominator_offsets = {
      9, 17, 25, 33, 41, 49, 57
  };

  for (size_t offset : denominator_offsets) {
    std::vector<uint8_t> data = kMonoGolden;
    write_u32(&data, offset, 0);

    GainMapMetadataParseResult parsed =
        parse_gain_map_metadata(data);
    INFO("denominator byte offset: " << offset);
    REQUIRE(parsed.status == GainMapMetadataParseStatus::malformed);
    REQUIRE(parsed.error.error_code == heif_error_Invalid_input);
    REQUIRE(parsed.error.sub_error_code ==
            heif_suberror_Invalid_fractional_number);
  }
}


TEST_CASE("gain map metadata rejects zero gamma numerator")
{
  std::vector<uint8_t> data = kMonoGolden;
  write_u32(&data, 37, 0);

  GainMapMetadataParseResult parsed =
      parse_gain_map_metadata(data);

  REQUIRE(parsed.status == GainMapMetadataParseStatus::malformed);
  REQUIRE(parsed.error.sub_error_code ==
          heif_suberror_Invalid_fractional_number);
}


TEST_CASE("gain map metadata rejects maximum below minimum")
{
  std::vector<uint8_t> data = kMonoGolden;
  write_u32(&data, 21, 4);
  write_u32(&data, 25, 1);

  GainMapMetadataParseResult parsed =
      parse_gain_map_metadata(data);

  REQUIRE(parsed.status == GainMapMetadataParseStatus::malformed);
  REQUIRE(parsed.error.error_code == heif_error_Invalid_input);
}


TEST_CASE("gain map metadata rejects equal HDR headrooms")
{
  std::vector<uint8_t> data = kMonoGolden;
  write_u32(&data, 13, 0);

  GainMapMetadataParseResult parsed =
      parse_gain_map_metadata(data);

  REQUIRE(parsed.status == GainMapMetadataParseStatus::malformed);
  REQUIRE(parsed.error.error_code == heif_error_Invalid_input);
}


TEST_CASE("gain map metadata ignores reserved flag bits")
{
  std::vector<uint8_t> data = kMonoGolden;
  data[4] = 0x7F;

  GainMapMetadataParseResult parsed =
      parse_gain_map_metadata(data);

  REQUIRE(parsed.status == GainMapMetadataParseStatus::parsed);
  REQUIRE(parsed.metadata.has_value());
  REQUIRE(parsed.metadata->channel_count == 1);
  REQUIRE(parsed.metadata->use_base_colour_space);

  auto encoded = serialize_gain_map_metadata(*parsed.metadata);
  REQUIRE(encoded);
  REQUIRE((*encoded)[4] == 0x40);
}


TEST_CASE("gain map metadata handles INT32_MIN safely")
{
  GainMapMetadata metadata = make_valid_metadata();
  metadata.channels[0].gain_map_min = {
      std::numeric_limits<int32_t>::min(),
      std::numeric_limits<uint32_t>::max()
  };
  metadata.channels[0].gain_map_max = {
      std::numeric_limits<int32_t>::max(),
      std::numeric_limits<uint32_t>::max()
  };
  metadata.channels[0].base_offset = {
      std::numeric_limits<int32_t>::min(),
      std::numeric_limits<uint32_t>::max()
  };

  auto encoded = serialize_gain_map_metadata(metadata);
  REQUIRE(encoded);

  GainMapMetadataParseResult parsed =
      parse_gain_map_metadata(*encoded);
  REQUIRE(parsed.status == GainMapMetadataParseStatus::parsed);
  REQUIRE(parsed.metadata.has_value());
  REQUIRE(parsed.metadata->channels[0].gain_map_min.numerator ==
          std::numeric_limits<int32_t>::min());
  REQUIRE(parsed.metadata->channels[0].base_offset.numerator ==
          std::numeric_limits<int32_t>::min());
}


TEST_CASE("gain map metadata rejects every truncated prefix")
{
  for (size_t size = 0; size < kMonoGolden.size(); ++size) {
    std::span<const uint8_t> prefix(kMonoGolden.data(), size);
    GainMapMetadataParseResult parsed =
        parse_gain_map_metadata(prefix);

    INFO("prefix size: " << size);
    REQUIRE(parsed.status == GainMapMetadataParseStatus::malformed);
    REQUIRE(parsed.error.error_code == heif_error_Invalid_input);
    REQUIRE(parsed.error.sub_error_code == heif_suberror_End_of_data);
  }
}


TEST_CASE("gain map metadata serializer rejects invalid metadata")
{
  GainMapMetadata metadata = make_valid_metadata();
  metadata.channel_count = 2;
  auto invalid_channels = serialize_gain_map_metadata(metadata);
  REQUIRE(!invalid_channels);
  REQUIRE(invalid_channels.error().error_code ==
          heif_error_Invalid_input);

  metadata = make_valid_metadata();
  metadata.channels[0].gamma.denominator = 0;
  auto invalid_gamma = serialize_gain_map_metadata(metadata);
  REQUIRE(!invalid_gamma);
  REQUIRE(invalid_gamma.error().sub_error_code ==
          heif_suberror_Invalid_fractional_number);

  metadata = make_valid_metadata();
  metadata.version.minimum_version = 1;
  metadata.version.writer_version = 1;
  auto unsupported = serialize_gain_map_metadata(metadata);
  REQUIRE(!unsupported);
  REQUIRE(unsupported.error().error_code ==
          heif_error_Unsupported_feature);
  REQUIRE(unsupported.error().sub_error_code ==
          heif_suberror_Unsupported_data_version);
}
