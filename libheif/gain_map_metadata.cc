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

#include <cstddef>
#include <cstdint>
#include <vector>


namespace
{

class GainMapByteReader
{
public:
  explicit GainMapByteReader(std::span<const uint8_t> data)
      : m_data(data)
  {
  }

  bool read_u8(uint8_t* value)
  {
    if (!can_read(1)) {
      return false;
    }

    *value = m_data[m_position];
    ++m_position;
    return true;
  }

  bool read_u16(uint16_t* value)
  {
    if (!can_read(2)) {
      return false;
    }

    *value = static_cast<uint16_t>(
        (static_cast<uint16_t>(m_data[m_position]) << 8) |
        static_cast<uint16_t>(m_data[m_position + 1]));
    m_position += 2;
    return true;
  }

  bool read_u32(uint32_t* value)
  {
    if (!can_read(4)) {
      return false;
    }

    *value =
        (static_cast<uint32_t>(m_data[m_position]) << 24) |
        (static_cast<uint32_t>(m_data[m_position + 1]) << 16) |
        (static_cast<uint32_t>(m_data[m_position + 2]) << 8) |
        static_cast<uint32_t>(m_data[m_position + 3]);
    m_position += 4;
    return true;
  }

  bool read_i32(int32_t* value)
  {
    uint32_t wire_value;
    if (!read_u32(&wire_value)) {
      return false;
    }

    if ((wire_value & 0x80000000U) != 0) {
      *value =
          -static_cast<int32_t>((~wire_value) & 0x7FFFFFFFU) - 1;
    }
    else {
      *value = static_cast<int32_t>(wire_value);
    }

    return true;
  }

  size_t position() const
  {
    return m_position;
  }

private:
  bool can_read(size_t size) const
  {
    return m_position <= m_data.size() &&
           size <= m_data.size() - m_position;
  }

  std::span<const uint8_t> m_data;
  size_t m_position = 0;
};


class GainMapByteWriter
{
public:
  void write_u8(uint8_t value)
  {
    m_data.push_back(value);
  }

  void write_u16(uint16_t value)
  {
    m_data.push_back(static_cast<uint8_t>(value >> 8));
    m_data.push_back(static_cast<uint8_t>(value));
  }

  void write_u32(uint32_t value)
  {
    m_data.push_back(static_cast<uint8_t>(value >> 24));
    m_data.push_back(static_cast<uint8_t>(value >> 16));
    m_data.push_back(static_cast<uint8_t>(value >> 8));
    m_data.push_back(static_cast<uint8_t>(value));
  }

  void write_i32(int32_t value)
  {
    write_u32(static_cast<uint32_t>(value));
  }

  const std::vector<uint8_t>& data() const
  {
    return m_data;
  }

private:
  std::vector<uint8_t> m_data;
};


Error truncated_metadata_error()
{
  return {
      heif_error_Invalid_input,
      heif_suberror_End_of_data,
      "Truncated ISO 21496-1 gain map metadata"
  };
}


Error invalid_metadata_error(const char* message)
{
  return {
      heif_error_Invalid_input,
      heif_suberror_Unspecified,
      message
  };
}


Error invalid_rational_error(const char* message)
{
  return {
      heif_error_Invalid_input,
      heif_suberror_Invalid_fractional_number,
      message
  };
}


Error unsupported_version_error()
{
  return {
      heif_error_Unsupported_feature,
      heif_suberror_Unsupported_data_version,
      "Unsupported ISO 21496-1 gain map metadata minimum version"
  };
}


bool read_signed_rational(GainMapByteReader* reader,
                          GainMapSignedRational32* value)
{
  return reader->read_i32(&value->numerator) &&
         reader->read_u32(&value->denominator);
}


bool read_unsigned_rational(GainMapByteReader* reader,
                            GainMapUnsignedRational32* value)
{
  return reader->read_u32(&value->numerator) &&
         reader->read_u32(&value->denominator);
}


void write_signed_rational(GainMapByteWriter* writer,
                           const GainMapSignedRational32& value)
{
  writer->write_i32(value.numerator);
  writer->write_u32(value.denominator);
}


void write_unsigned_rational(GainMapByteWriter* writer,
                             const GainMapUnsignedRational32& value)
{
  writer->write_u32(value.numerator);
  writer->write_u32(value.denominator);
}


bool signed_rational_less(const GainMapSignedRational32& lhs,
                          const GainMapSignedRational32& rhs)
{
  const int64_t left =
      static_cast<int64_t>(lhs.numerator) * rhs.denominator;
  const int64_t right =
      static_cast<int64_t>(rhs.numerator) * lhs.denominator;
  return left < right;
}


bool unsigned_rational_equal(const GainMapUnsignedRational32& lhs,
                             const GainMapUnsignedRational32& rhs)
{
  const uint64_t left =
      static_cast<uint64_t>(lhs.numerator) * rhs.denominator;
  const uint64_t right =
      static_cast<uint64_t>(rhs.numerator) * lhs.denominator;
  return left == right;
}


GainMapMetadataParseResult malformed_result(
    const GainMapVersion& version,
    size_t bytes_consumed,
    const Error& error)
{
  GainMapMetadataParseResult result;
  result.status = GainMapMetadataParseStatus::malformed;
  result.version = version;
  result.error = error;
  result.bytes_consumed = bytes_consumed;
  return result;
}

}  // namespace


Error validate_gain_map_metadata(const GainMapMetadata& metadata)
{
  if (metadata.version.writer_version <
      metadata.version.minimum_version) {
    return invalid_metadata_error(
        "Gain map writer_version is smaller than minimum_version");
  }

  if (metadata.version.minimum_version != 0) {
    return unsupported_version_error();
  }

  if (metadata.channel_count != 1 && metadata.channel_count != 3) {
    return invalid_metadata_error(
        "Gain map metadata channel count must be 1 or 3");
  }

  if (metadata.base_hdr_headroom.denominator == 0 ||
      metadata.alternate_hdr_headroom.denominator == 0) {
    return invalid_rational_error(
        "Gain map HDR headroom denominator must not be zero");
  }

  if (unsigned_rational_equal(metadata.base_hdr_headroom,
                              metadata.alternate_hdr_headroom)) {
    return invalid_metadata_error(
        "Baseline and alternate HDR headroom must differ");
  }

  for (uint8_t i = 0; i < metadata.channel_count; ++i) {
    const GainMapChannel& channel = metadata.channels[i];

    if (channel.gain_map_min.denominator == 0 ||
        channel.gain_map_max.denominator == 0 ||
        channel.gamma.denominator == 0 ||
        channel.base_offset.denominator == 0 ||
        channel.alternate_offset.denominator == 0) {
      return invalid_rational_error(
          "Gain map rational denominator must not be zero");
    }

    if (channel.gamma.numerator == 0) {
      return invalid_rational_error(
          "Gain map gamma numerator must not be zero");
    }

    if (signed_rational_less(channel.gain_map_max,
                             channel.gain_map_min)) {
      return invalid_metadata_error(
          "Gain map maximum value is smaller than minimum value");
    }
  }

  return Error::Ok;
}


GainMapMetadataParseResult parse_gain_map_metadata(
    std::span<const uint8_t> data)
{
  GainMapByteReader reader(data);
  GainMapVersion version;

  if (!reader.read_u16(&version.minimum_version) ||
      !reader.read_u16(&version.writer_version)) {
    return malformed_result(
        version, reader.position(), truncated_metadata_error());
  }

  if (version.writer_version < version.minimum_version) {
    return malformed_result(
        version,
        reader.position(),
        invalid_metadata_error(
            "Gain map writer_version is smaller than minimum_version"));
  }

  if (version.minimum_version != 0) {
    GainMapMetadataParseResult result;
    result.status =
        GainMapMetadataParseStatus::unsupported_minimum_version;
    result.version = version;
    result.bytes_consumed = reader.position();
    return result;
  }

  GainMapMetadata metadata;
  metadata.version = version;

  uint8_t flags;
  if (!reader.read_u8(&flags)) {
    return malformed_result(
        version, reader.position(), truncated_metadata_error());
  }

  metadata.channel_count = (flags & 0x80U) != 0 ? 3 : 1;
  metadata.use_base_colour_space = (flags & 0x40U) != 0;

  if (!read_unsigned_rational(&reader,
                              &metadata.base_hdr_headroom) ||
      !read_unsigned_rational(&reader,
                              &metadata.alternate_hdr_headroom)) {
    return malformed_result(
        version, reader.position(), truncated_metadata_error());
  }

  for (uint8_t i = 0; i < metadata.channel_count; ++i) {
    GainMapChannel& channel = metadata.channels[i];

    if (!read_signed_rational(&reader, &channel.gain_map_min) ||
        !read_signed_rational(&reader, &channel.gain_map_max) ||
        !read_unsigned_rational(&reader, &channel.gamma) ||
        !read_signed_rational(&reader, &channel.base_offset) ||
        !read_signed_rational(&reader, &channel.alternate_offset)) {
      return malformed_result(
          version, reader.position(), truncated_metadata_error());
    }
  }

  Error validation_error = validate_gain_map_metadata(metadata);
  if (validation_error) {
    return malformed_result(
        version, reader.position(), validation_error);
  }

  GainMapMetadataParseResult result;
  result.status = GainMapMetadataParseStatus::parsed;
  result.version = version;
  result.metadata = metadata;
  result.bytes_consumed = reader.position();
  return result;
}


Result<std::vector<uint8_t>> serialize_gain_map_metadata(
    const GainMapMetadata& metadata)
{
  Error validation_error = validate_gain_map_metadata(metadata);
  if (validation_error) {
    return validation_error;
  }

  GainMapByteWriter writer;
  writer.write_u16(metadata.version.minimum_version);
  writer.write_u16(metadata.version.writer_version);

  uint8_t flags = 0;
  if (metadata.channel_count == 3) {
    flags |= 0x80U;
  }
  if (metadata.use_base_colour_space) {
    flags |= 0x40U;
  }
  writer.write_u8(flags);

  write_unsigned_rational(&writer, metadata.base_hdr_headroom);
  write_unsigned_rational(&writer, metadata.alternate_hdr_headroom);

  for (uint8_t i = 0; i < metadata.channel_count; ++i) {
    const GainMapChannel& channel = metadata.channels[i];

    write_signed_rational(&writer, channel.gain_map_min);
    write_signed_rational(&writer, channel.gain_map_max);
    write_unsigned_rational(&writer, channel.gamma);
    write_signed_rational(&writer, channel.base_offset);
    write_signed_rational(&writer, channel.alternate_offset);
  }

  return writer.data();
}


ToneMapImageParseResult parse_tone_map_image(
    std::span<const uint8_t> data)
{
  ToneMapImageParseResult result;

  if (data.empty()) {
    result.error = truncated_metadata_error();
    return result;
  }

  result.version = data[0];
  result.bytes_consumed = 1;

  if (result.version != 0) {
    result.status =
        ToneMapImageParseStatus::unsupported_tone_map_version;
    return result;
  }

  GainMapMetadataParseResult gain_map =
      parse_gain_map_metadata(data.subspan(1));
  result.bytes_consumed += gain_map.bytes_consumed;

  switch (gain_map.status) {
    case GainMapMetadataParseStatus::parsed: {
      ToneMapImage tone_map_image;
      tone_map_image.version = 0;
      tone_map_image.gain_map_metadata = *gain_map.metadata;
      result.status = ToneMapImageParseStatus::parsed;
      result.tone_map_image = tone_map_image;
      return result;
    }

    case GainMapMetadataParseStatus::unsupported_minimum_version:
      result.status =
          ToneMapImageParseStatus::unsupported_minimum_version;
      return result;

    case GainMapMetadataParseStatus::malformed:
      result.status = ToneMapImageParseStatus::malformed;
      result.error = gain_map.error;
      return result;
  }

  result.error = invalid_metadata_error(
      "Invalid ISO 21496-1 gain map metadata parse status");
  return result;
}


Result<std::vector<uint8_t>> serialize_tone_map_image(
    const ToneMapImage& tone_map_image)
{
  if (tone_map_image.version != 0) {
    return Error{
        heif_error_Unsupported_feature,
        heif_suberror_Unsupported_data_version,
        "Unsupported ToneMapImage version"
    };
  }

  auto gain_map_data =
      serialize_gain_map_metadata(tone_map_image.gain_map_metadata);
  if (!gain_map_data) {
    return gain_map_data.error();
  }

  std::vector<uint8_t> data;
  data.reserve(1 + gain_map_data->size());
  data.push_back(0);
  data.insert(data.end(), gain_map_data->begin(), gain_map_data->end());
  return data;
}
