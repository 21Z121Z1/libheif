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
 * along with libheif. If not, see <http://www.gnu.org/licenses/>.
 */

#include "gbt48347.h"

#include "bitstream.h"

#include <cmath>
#include <cstring>
#include <utility>


namespace
{

class ByteReader
{
public:
  explicit ByteReader(std::span<const uint8_t> data) : m_data(data) {}

  bool read_u8(uint8_t* v)
  {
    if (!can_read(1)) {
      return false;
    }
    *v = m_data[m_pos++];
    return true;
  }

  bool read_u16(uint16_t* v)
  {
    if (!can_read(2)) {
      return false;
    }
    *v = static_cast<uint16_t>((uint16_t(m_data[m_pos]) << 8) | m_data[m_pos + 1]);
    m_pos += 2;
    return true;
  }

  bool read_u32(uint32_t* v)
  {
    if (!can_read(4)) {
      return false;
    }
    *v = (uint32_t(m_data[m_pos]) << 24) | (uint32_t(m_data[m_pos + 1]) << 16) |
         (uint32_t(m_data[m_pos + 2]) << 8) | uint32_t(m_data[m_pos + 3]);
    m_pos += 4;
    return true;
  }

  bool read_bytes(size_t n, std::vector<uint8_t>* out)
  {
    if (!can_read(n)) {
      return false;
    }
    out->assign(m_data.begin() + m_pos, m_data.begin() + m_pos + n);
    m_pos += n;
    return true;
  }

  bool read_f16(float* v)
  {
    uint16_t h;
    if (!read_u16(&h)) {
      return false;
    }
    *v = half_to_float(h);
    return true;
  }

  bool read_f32(float* v)
  {
    uint32_t bits;
    if (!read_u32(&bits)) {
      return false;
    }
    std::memcpy(v, &bits, sizeof(bits));
    return true;
  }

  size_t position() const { return m_pos; }
  size_t remaining() const { return m_data.size() - m_pos; }

  static float half_to_float(uint16_t h)
  {
    const uint32_t sign = uint32_t(h & 0x8000) << 16;
    const uint32_t exponent = (h >> 10) & 0x1F;
    const uint32_t mantissa = h & 0x03FF;
    uint32_t bits;
    if (exponent == 0) {
      if (mantissa == 0) {
        bits = sign;  // +/- zero
      }
      else {
        // Subnormal half: normalize into a float32 normal.
        uint32_t e = 127 - 15 + 1;
        uint32_t m = mantissa;
        while ((m & 0x0400) == 0) {
          m <<= 1;
          --e;
        }
        m &= 0x03FF;
        bits = sign | (e << 23) | (m << 13);
      }
    }
    else if (exponent == 0x1F) {
      bits = sign | 0x7F800000 | (mantissa << 13);  // Inf / NaN
    }
    else {
      bits = sign | ((exponent - 15 + 127) << 23) | (mantissa << 13);
    }
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
  }

  static uint16_t float_to_half(float f)
  {
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    const uint16_t sign = uint16_t((bits >> 16) & 0x8000);
    int32_t exponent = int32_t((bits >> 23) & 0xFF) - 127 + 15;
    uint32_t mantissa = bits & 0x007FFFFF;
    if (((bits >> 23) & 0xFF) == 0xFF) {
      return uint16_t(sign | 0x7C00 | (mantissa ? 0x0200 : 0));  // Inf / NaN
    }
    if (exponent >= 0x1F) {
      return uint16_t(sign | 0x7C00);  // overflow -> Inf
    }
    if (exponent <= 0) {
      if (exponent < -10) {
        return sign;  // underflow -> zero
      }
      mantissa |= 0x00800000;
      const uint32_t shift = uint32_t(14 - exponent);
      uint32_t half_m = mantissa >> shift;
      // Round to nearest even on the discarded bits.
      const uint32_t round_bit = uint32_t(1) << (shift - 1);
      if ((mantissa & round_bit) &&
          ((mantissa & (round_bit - 1)) || (half_m & 1))) {
        ++half_m;
      }
      return uint16_t(sign | half_m);
    }
    uint16_t half_m = uint16_t(mantissa >> 13);
    const uint32_t round_bit = 0x00001000;
    if ((mantissa & round_bit) &&
        ((mantissa & (round_bit - 1)) || (half_m & 1))) {
      ++half_m;
      if (half_m == 0x0400) {
        half_m = 0;
        ++exponent;
        if (exponent >= 0x1F) {
          return uint16_t(sign | 0x7C00);
        }
      }
    }
    return uint16_t(sign | (uint16_t(exponent) << 10) | half_m);
  }

private:
  bool can_read(size_t n) const
  {
    return m_pos <= m_data.size() && n <= m_data.size() - m_pos;
  }

  std::span<const uint8_t> m_data;
  size_t m_pos = 0;
};


Error malformed(const char* message)
{
  return {heif_error_Invalid_input, heif_suberror_Unspecified, message};
}

Error truncated()
{
  return {heif_error_Invalid_input, heif_suberror_End_of_data,
          "Truncated GB/T 48347 metadata"};
}


bool read_mapping_element(ByteReader* reader, uint8_t flag, float* v)
{
  return flag == 16 ? reader->read_f16(v) : reader->read_f32(v);
}


// Parse one base/combined mapping block (tables 16/17). The block length is
// fixed by the enclosing size prefix; the LUT order N is derived from the
// remaining bytes and must be an exact cube (see header note).
bool parse_mapping_block(std::span<const uint8_t> block, gbt48347::MappingInfo* out)
{
  ByteReader reader(block);
  if (!reader.read_u8(&out->flag)) {
    return false;
  }
  if (out->flag == 0) {
    return reader.remaining() == 0;
  }
  if (out->flag != 16 && out->flag != 32) {
    return false;
  }
  out->matrix.resize(9);
  for (int i = 0; i < 9; ++i) {
    if (!read_mapping_element(&reader, out->flag, &out->matrix[i])) {
      return false;
    }
  }
  const size_t element_bytes = out->flag / 8;
  const size_t remaining = reader.remaining();
  if (remaining == 0) {
    out->lut_order = 0;
    return true;
  }
  if (remaining % element_bytes != 0) {
    return false;
  }
  const size_t count = remaining / element_bytes;
  const uint32_t n = static_cast<uint32_t>(std::cbrt(static_cast<double>(count)));
  bool exact = false;
  for (uint32_t candidate = (n > 0 ? n - 1 : 0); candidate <= n + 1; ++candidate) {
    if (uint64_t(candidate) * candidate * candidate == count) {
      out->lut_order = candidate;
      exact = true;
      break;
    }
  }
  if (!exact || out->lut_order == 0) {
    return false;
  }
  out->lut.resize(count);
  for (size_t i = 0; i < count; ++i) {
    if (!read_mapping_element(&reader, out->flag, &out->lut[i])) {
      return false;
    }
  }
  return reader.remaining() == 0;
}


void write_mapping_element(StreamWriter* writer, uint8_t flag, float v)
{
  if (flag == 16) {
    writer->write16(ByteReader::float_to_half(v));
  }
  else {
    uint32_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    writer->write32(bits);
  }
}


Result<std::vector<uint8_t>> serialize_mapping_block(const gbt48347::MappingInfo& info)
{
  // GB/T 48347-2026 tables 16/17: flag is 0, 16 or 32. A non-zero flag transmits
  // exactly nine matrix elements plus an N^3 LUT; flag 0 transmits nothing else.
  if (info.flag != 0 && info.flag != 16 && info.flag != 32) {
    return malformed("GB/T 48347 mapping flag must be 0, 16 or 32");
  }
  if (info.flag == 0) {
    if (!info.matrix.empty() || !info.lut.empty() || info.lut_order != 0) {
      return malformed("GB/T 48347 mapping flag 0 must carry no matrix or LUT");
    }
  }
  else {
    if (info.matrix.size() != 9) {
      return malformed("GB/T 48347 mapping matrix must have exactly 9 elements");
    }
    if (info.lut_order == 0) {
      if (!info.lut.empty()) {
        return malformed("GB/T 48347 mapping lut_order 0 requires an empty LUT");
      }
    }
    else {
      const uint64_t n = info.lut_order;
      if (static_cast<uint64_t>(info.lut.size()) != n * n * n) {
        return malformed("GB/T 48347 mapping LUT size must equal lut_order cubed");
      }
    }
  }

  StreamWriter writer;
  writer.write8(info.flag);
  if (info.flag != 0) {
    for (float v : info.matrix) {
      write_mapping_element(&writer, info.flag, v);
    }
    for (float v : info.lut) {
      write_mapping_element(&writer, info.flag, v);
    }
  }
  return writer.get_data();
}

}  // namespace


namespace gbt48347
{

ExtensionParseResult parse_extend_info_extension(std::span<const uint8_t> data)
{
  ExtensionParseResult result;
  ByteReader reader(data);

  auto read_block = [&](MappingInfo* out, bool* present) -> bool {
    uint16_t size;
    if (!reader.read_u16(&size)) {
      return false;
    }
    if (size == 0) {
      *present = false;
      return true;
    }
    std::vector<uint8_t> block;
    if (!reader.read_bytes(size, &block)) {
      return false;
    }
    *present = true;
    return parse_mapping_block(block, out);
  };

  ExtendInfoExtension extension;
  if (!read_block(&extension.base, &extension.base_present) ||
      !read_block(&extension.combined, &extension.combined_present)) {
    result.error = reader.remaining() == 0 && data.size() < 4
                       ? truncated()
                       : malformed("Invalid GB/T 48347 extend_info_extension");
    result.bytes_consumed = reader.position();
    return result;
  }

  // Table 15 defines exactly two size-prefixed blocks; unexplained trailing
  // bytes within the extension are non-conformant.
  if (reader.remaining() != 0) {
    result.error = malformed("GB/T 48347 extend_info_extension has trailing bytes");
    result.bytes_consumed = reader.position();
    return result;
  }

  result.status = ParseStatus::parsed;
  result.extension = std::move(extension);
  result.bytes_consumed = reader.position();
  return result;
}


Result<std::vector<uint8_t>> serialize_extend_info_extension(
    const ExtendInfoExtension& extension)
{
  StreamWriter writer;
  auto write_block = [&](const MappingInfo& info, bool present) -> Error {
    if (!present) {
      writer.write16(0);
      return Error::Ok;
    }
    auto block = serialize_mapping_block(info);
    if (!block) {
      return block.error();
    }
    // The size prefix is u(16); validate before the narrowing cast so a large
    // LUT cannot produce a truncated, inconsistent length prefix.
    if (block->size() > 0xFFFF) {
      return malformed("GB/T 48347 mapping block exceeds the 16-bit size prefix");
    }
    writer.write16(static_cast<uint16_t>(block->size()));
    for (uint8_t b : *block) {
      writer.write8(b);
    }
    return Error::Ok;
  };
  if (Error error = write_block(extension.base, extension.base_present)) {
    return error;
  }
  if (Error error = write_block(extension.combined, extension.combined_present)) {
    return error;
  }
  return writer.get_data();
}


UWAInfoParseResult parse_uwa_info(std::span<const uint8_t> data)
{
  UWAInfoParseResult result;
  ByteReader reader(data);
  UWAInfo info;

  if (!reader.read_u8(&info.country_code) ||
      !reader.read_u16(&info.terminal_provide_code) ||
      !reader.read_u16(&info.terminal_provide_oriented_code) ||
      !reader.read_u8(&info.extended_frame_number) ||
      !reader.read_u32(&info.extended_type)) {
    result.error = truncated();
    result.bytes_consumed = reader.position();
    return result;
  }

  if (info.country_code != kCountryCode ||
      info.terminal_provide_code != kTerminalProvideCode ||
      info.terminal_provide_oriented_code != kTerminalProvideOrientedCode) {
    result.error = malformed("GB/T 48347 UWAInfo ITU-T T.35 identifiers do not match");
    result.bytes_consumed = reader.position();
    return result;
  }

  if (extended_meta_type(info.extended_type) == 1) {
    info.metadata_present = true;
    uint16_t static_size;
    if (!reader.read_u16(&info.metadata_size) || !reader.read_u16(&static_size)) {
      result.error = truncated();
      result.bytes_consumed = reader.position();
      return result;
    }
    if (!reader.read_bytes(static_size, &info.static_metadata)) {
      result.error = truncated();
      result.bytes_consumed = reader.position();
      return result;
    }
    uint16_t dynamic_size;
    if (!reader.read_u16(&dynamic_size)) {
      result.error = truncated();
      result.bytes_consumed = reader.position();
      return result;
    }
    if (!reader.read_bytes(dynamic_size, &info.dynamic_metadata)) {
      result.error = truncated();
      result.bytes_consumed = reader.position();
      return result;
    }
    // GB/T 48347-2026 9.2.3a: metadata_size is "the total length of dynamic and
    // static metadata" = static_metadata_size + dynamic_metadata_size (payloads
    // only, excluding the two 16-bit size fields). It is a single discrete value.
    const size_t payloads = size_t(static_size) + dynamic_size;
    if (info.metadata_size != payloads) {
      result.error = malformed("GB/T 48347 UWAInfo metadata_size does not match payloads");
      result.bytes_consumed = reader.position();
      return result;
    }
  }

  if (extended_enhance_type(info.extended_type) == 1) {
    info.extension_present = true;
    uint16_t extension_size;
    if (!reader.read_u16(&extension_size)) {
      result.error = truncated();
      result.bytes_consumed = reader.position();
      return result;
    }
    std::vector<uint8_t> block;
    if (!reader.read_bytes(extension_size, &block)) {
      result.error = truncated();
      result.bytes_consumed = reader.position();
      return result;
    }
    auto extension = parse_extend_info_extension(block);
    if (extension.status != ParseStatus::parsed) {
      result.error = extension.error;
      result.bytes_consumed = reader.position();
      return result;
    }
    info.extension = *extension.extension;
  }

  result.status = ParseStatus::parsed;
  result.info = std::move(info);
  result.bytes_consumed = reader.position();
  return result;
}


Result<std::vector<uint8_t>> serialize_uwa_info(const UWAInfo& info)
{
  if (info.country_code != kCountryCode ||
      info.terminal_provide_code != kTerminalProvideCode ||
      info.terminal_provide_oriented_code != kTerminalProvideOrientedCode) {
    return malformed("GB/T 48347 UWAInfo ITU-T T.35 identifiers do not match");
  }
  if (info.metadata_present != (extended_meta_type(info.extended_type) == 1) ||
      info.extension_present != (extended_enhance_type(info.extended_type) == 1)) {
    return malformed("GB/T 48347 UWAInfo extended_type flags disagree with payloads");
  }

  StreamWriter writer;
  writer.write8(info.country_code);
  writer.write16(info.terminal_provide_code);
  writer.write16(info.terminal_provide_oriented_code);
  writer.write8(info.extended_frame_number);
  writer.write32(info.extended_type);

  if (info.metadata_present) {
    // metadata_size = static + dynamic payload lengths (9.2.3a), excluding the
    // two 16-bit size fields.
    const size_t metadata_size =
        info.static_metadata.size() + info.dynamic_metadata.size();
    if (metadata_size > 0xFFFF || info.static_metadata.size() > 0xFFFF ||
        info.dynamic_metadata.size() > 0xFFFF) {
      return malformed("GB/T 48347 UWAInfo metadata block too large");
    }
    writer.write16(static_cast<uint16_t>(metadata_size));
    writer.write16(static_cast<uint16_t>(info.static_metadata.size()));
    for (uint8_t b : info.static_metadata) {
      writer.write8(b);
    }
    writer.write16(static_cast<uint16_t>(info.dynamic_metadata.size()));
    for (uint8_t b : info.dynamic_metadata) {
      writer.write8(b);
    }
  }

  if (info.extension_present) {
    auto block = serialize_extend_info_extension(info.extension);
    if (!block) {
      return block.error();
    }
    if (block->size() > 0xFFFF) {
      return malformed("GB/T 48347 UWAInfo extension block too large");
    }
    writer.write16(static_cast<uint16_t>(block->size()));
    for (uint8_t b : *block) {
      writer.write8(b);
    }
  }

  return writer.get_data();
}

}  // namespace gbt48347