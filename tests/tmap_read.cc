/*
  libheif ISO 21496-1 HEIF tmap read tests

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
#include "libheif/heif.h"
#include "libheif/heif_experimental.h"
#include "test_utils.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <limits>
#include <vector>


namespace
{

std::vector<uint8_t> make_nclx(
    uint16_t primaries,
    uint16_t transfer,
    uint16_t matrix,
    bool full_range)
{
  std::vector<uint8_t> payload;
  append_fourcc(payload, "nclx");
  put_u16_be(payload, primaries);
  put_u16_be(payload, transfer);
  put_u16_be(payload, matrix);
  payload.push_back(full_range ? 0x80 : 0x00);
  return make_box("colr", payload);
}


std::vector<uint8_t> make_tone_map_payload(
    uint8_t outer_version = 0,
    uint16_t minimum_version = 0,
    const GainMapMetadata* override_metadata = nullptr)
{
  GainMapMetadata metadata;
  metadata.version.minimum_version = 0;
  metadata.version.writer_version = 0;
  metadata.channel_count = 1;
  metadata.use_base_colour_space = true;
  metadata.base_hdr_headroom = {0, 1};
  metadata.alternate_hdr_headroom = {2, 1};
  metadata.channels[0].gain_map_min = {-1, 1};
  metadata.channels[0].gain_map_max = {1, 1};
  metadata.channels[0].gamma = {1, 1};
  metadata.channels[0].base_offset = {0, 1};
  metadata.channels[0].alternate_offset = {0, 1};
  if (override_metadata) {
    metadata = *override_metadata;
  }

  auto annex_c = serialize_gain_map_metadata(metadata);
  REQUIRE(annex_c);

  std::vector<uint8_t> payload;
  payload.push_back(outer_version);
  payload.insert(payload.end(), annex_c->begin(), annex_c->end());

  payload[1] = static_cast<uint8_t>(minimum_version >> 8);
  payload[2] = static_cast<uint8_t>(minimum_version);
  if (minimum_version != 0) {
    payload[3] = static_cast<uint8_t>(minimum_version >> 8);
    payload[4] = static_cast<uint8_t>(minimum_version);
  }

  return payload;
}


std::vector<uint8_t> build_tmap_file(
    int dimg_count = 2,
    uint8_t outer_version = 0,
    uint16_t minimum_version = 0,
    uint16_t gain_primaries = 2,
    uint16_t gain_transfer = 2,
    uint16_t primary_item = 1,
    bool rotate_base = false,
    bool duplicate_dimg_entry = false,
    const GainMapMetadata* override_metadata = nullptr,
    bool premultiplied_base = false)
{
  constexpr uint32_t width = 2;
  constexpr uint32_t height = 2;

  std::vector<uint8_t> tmap_payload =
      make_tone_map_payload(outer_version, minimum_version, override_metadata);

  std::vector<uint8_t> ftyp_payload;
  append_fourcc(ftyp_payload, "mif1");
  put_u32_be(ftyp_payload, 0);
  append_fourcc(ftyp_payload, "mif1");
  append_fourcc(ftyp_payload, "tmap");
  auto ftyp = make_box("ftyp", ftyp_payload);

  std::vector<uint8_t> hdlr_payload;
  put_u32_be(hdlr_payload, 0);
  append_fourcc(hdlr_payload, "pict");
  put_u32_be(hdlr_payload, 0);
  put_u32_be(hdlr_payload, 0);
  put_u32_be(hdlr_payload, 0);
  hdlr_payload.push_back(0);
  auto hdlr = make_box("hdlr", hdlr_payload, true);

  std::vector<uint8_t> pitm_payload;
  put_u16_be(pitm_payload, primary_item);
  auto pitm = make_box("pitm", pitm_payload, true);

  std::vector<uint8_t> iinf_payload;
  const uint16_t item_count = premultiplied_base ? 4 : 3;
  put_u16_be(iinf_payload, item_count);
  for (uint16_t id = 1; id <= item_count; ++id) {
    std::vector<uint8_t> infe;
    put_u16_be(infe, id);
    put_u16_be(infe, 0);
    append_fourcc(infe, id == 3 ? "tmap" : "mski");
    infe.push_back(0);
    append(iinf_payload,
           make_box("infe", infe, true, 2, id == 2 || id == 4 ? 1 : 0));
  }
  auto iinf = make_box("iinf", iinf_payload, true);

  std::vector<uint8_t> ispe_payload;
  put_u32_be(ispe_payload, width);
  put_u32_be(ispe_payload, height);
  auto ispe = make_box("ispe", ispe_payload, true);

  auto mskC = make_box("mskC", std::vector<uint8_t>{8}, true);
  auto base_colr = make_nclx(1, 13, 1, true);
  auto gain_colr =
      make_nclx(gain_primaries, gain_transfer, 2, true);
  auto tmap_colr = make_nclx(9, 16, 9, true);

  std::vector<uint8_t> ipco_payload;
  append(ipco_payload, ispe);
  append(ipco_payload, mskC);
  append(ipco_payload, base_colr);
  append(ipco_payload, gain_colr);
  append(ipco_payload, tmap_colr);
  if (rotate_base) {
    append(ipco_payload, make_box("irot", std::vector<uint8_t>{1}));
  }
  if (premultiplied_base) {
    const char auxiliary_type[] = "urn:mpeg:mpegB:cicp:systems:auxiliary:alpha";
    const std::vector<uint8_t> auxiliary_payload(auxiliary_type, auxiliary_type + sizeof(auxiliary_type));
    append(ipco_payload, make_box("auxC", auxiliary_payload, true));
  }
  auto ipco = make_box("ipco", ipco_payload);

  std::vector<uint8_t> ipma_payload;
  put_u32_be(ipma_payload, item_count);

  put_u16_be(ipma_payload, 1);
  ipma_payload.push_back(rotate_base ? 4 : 3);
  ipma_payload.push_back(0x80 | 1);
  ipma_payload.push_back(0x80 | 2);
  ipma_payload.push_back(3);
  if (rotate_base) {
    ipma_payload.push_back(0x80 | 6);
  }

  put_u16_be(ipma_payload, 2);
  ipma_payload.push_back(3);
  ipma_payload.push_back(0x80 | 1);
  ipma_payload.push_back(0x80 | 2);
  ipma_payload.push_back(4);

  put_u16_be(ipma_payload, 3);
  ipma_payload.push_back(2);
  ipma_payload.push_back(0x80 | 1);
  ipma_payload.push_back(5);
  if (premultiplied_base) {
    put_u16_be(ipma_payload, 4);
    ipma_payload.push_back(3);
    ipma_payload.push_back(0x80 | 1);
    ipma_payload.push_back(0x80 | 2);
    ipma_payload.push_back(0x80 | (rotate_base ? 7 : 6));
  }

  auto ipma = make_box("ipma", ipma_payload, true);

  std::vector<uint8_t> iprp_payload;
  append(iprp_payload, ipco);
  append(iprp_payload, ipma);
  auto iprp = make_box("iprp", iprp_payload);

  std::vector<uint8_t> idat_payload(width * height * 2, 0x7F);
  if (rotate_base) {
    idat_payload[0] = 0;
    idat_payload[1] = 63;
    idat_payload[2] = 127;
    idat_payload[3] = 255;
  }
  idat_payload.insert(
      idat_payload.end(), tmap_payload.begin(), tmap_payload.end());
  if (premultiplied_base) {
    std::fill_n(idat_payload.begin(), width * height, uint8_t{51});
    append(idat_payload, std::vector<uint8_t>{0, 85, 85, 255});
  }
  auto idat = make_box("idat", idat_payload);

  std::vector<uint8_t> iloc_payload;
  iloc_payload.push_back((4 << 4) | 4);
  iloc_payload.push_back(0);
  put_u16_be(iloc_payload, item_count);

  const uint32_t item_lengths[4] = {
      width * height,
      width * height,
      static_cast<uint32_t>(tmap_payload.size()),
      width * height
  };
  uint32_t offset = 0;
  for (uint16_t id = 1; id <= item_count; ++id) {
    put_u16_be(iloc_payload, id);
    put_u16_be(iloc_payload, 0x0001);
    put_u16_be(iloc_payload, 0);
    put_u16_be(iloc_payload, 1);
    put_u32_be(iloc_payload, offset);
    put_u32_be(iloc_payload, item_lengths[id - 1]);
    offset += item_lengths[id - 1];
  }
  auto iloc = make_box("iloc", iloc_payload, true, 1);

  std::vector<uint8_t> dimg_payload;
  put_u16_be(dimg_payload, 3);
  put_u16_be(dimg_payload, static_cast<uint16_t>(dimg_count));
  if (dimg_count >= 1) {
    put_u16_be(dimg_payload, 1);
  }
  if (dimg_count >= 2) {
    put_u16_be(dimg_payload, 2);
  }
  for (int i = 2; i < dimg_count; ++i) {
    put_u16_be(dimg_payload, 1);
  }
  std::vector<uint8_t> iref_payload;
  append(iref_payload, make_box("dimg", dimg_payload));
  if (duplicate_dimg_entry) {
    std::vector<uint8_t> duplicate_payload;
    put_u16_be(duplicate_payload, 3);
    put_u16_be(duplicate_payload, 1);
    put_u16_be(duplicate_payload, 1);
    append(iref_payload, make_box("dimg", duplicate_payload));
  }
  if (premultiplied_base) {
    std::vector<uint8_t> auxl;
    put_u16_be(auxl, 4);
    put_u16_be(auxl, 1);
    put_u16_be(auxl, 1);
    append(iref_payload, make_box("auxl", auxl));
    std::vector<uint8_t> prem;
    put_u16_be(prem, 1);
    put_u16_be(prem, 1);
    put_u16_be(prem, 4);
    append(iref_payload, make_box("prem", prem));
  }
  auto iref = make_box("iref", iref_payload, true);

  std::vector<uint8_t> meta_payload;
  append(meta_payload, hdlr);
  append(meta_payload, pitm);
  append(meta_payload, iinf);
  append(meta_payload, iprp);
  append(meta_payload, iloc);
  append(meta_payload, iref);
  append(meta_payload, idat);
  auto meta = make_box("meta", meta_payload, true);

  std::vector<uint8_t> file;
  append(file, ftyp);
  append(file, meta);
  return file;
}


heif_image_handle* open_tmap(
    heif_context** out_context,
    const std::vector<uint8_t>& file)
{
  heif_context* context = heif_context_alloc();
  REQUIRE(context != nullptr);
  REQUIRE(heif_context_read_from_memory(
              context, file.data(), file.size(), nullptr).code ==
          heif_error_Ok);

  heif_image_handle* tmap = nullptr;
  REQUIRE(heif_context_get_image_handle(
              context, 3, &tmap).code == heif_error_Ok);
  REQUIRE(tmap != nullptr);

  *out_context = context;
  return tmap;
}



std::vector<uint8_t> build_two_tmap_file(
    uint16_t second_base_id, uint16_t transfer = 16)
{
  constexpr uint32_t width = 2;
  constexpr uint32_t height = 2;

  const std::vector<uint8_t> tmap_payload =
      make_tone_map_payload();

  std::vector<uint8_t> ftyp_payload;
  append_fourcc(ftyp_payload, "mif1");
  put_u32_be(ftyp_payload, 0);
  append_fourcc(ftyp_payload, "mif1");
  append_fourcc(ftyp_payload, "tmap");
  auto ftyp = make_box("ftyp", ftyp_payload);

  std::vector<uint8_t> hdlr_payload;
  put_u32_be(hdlr_payload, 0);
  append_fourcc(hdlr_payload, "pict");
  put_u32_be(hdlr_payload, 0);
  put_u32_be(hdlr_payload, 0);
  put_u32_be(hdlr_payload, 0);
  hdlr_payload.push_back(0);
  auto hdlr = make_box("hdlr", hdlr_payload, true);

  std::vector<uint8_t> pitm_payload;
  put_u16_be(pitm_payload, 1);
  auto pitm = make_box("pitm", pitm_payload, true);

  std::vector<uint8_t> iinf_payload;
  put_u16_be(iinf_payload, 5);
  for (uint16_t id = 1; id <= 5; ++id) {
    std::vector<uint8_t> infe;
    put_u16_be(infe, id);
    put_u16_be(infe, 0);
    const bool is_tmap = id == 3 || id == 5;
    append_fourcc(infe, is_tmap ? "tmap" : "mski");
    infe.push_back(0);
    const bool hidden_gain = id == 2 || id == 4;
    append(iinf_payload,
           make_box("infe", infe, true, 2,
                    hidden_gain ? 1 : 0));
  }
  auto iinf = make_box("iinf", iinf_payload, true);

  std::vector<uint8_t> ispe_payload;
  put_u32_be(ispe_payload, width);
  put_u32_be(ispe_payload, height);
  auto ispe = make_box("ispe", ispe_payload, true);
  auto mskC = make_box(
      "mskC", std::vector<uint8_t>{8}, true);
  auto base_colr = make_nclx(1, 13, 1, true);
  auto gain_colr = make_nclx(2, 2, 0, true);
  auto tmap_colr = make_nclx(9, transfer, 9, true);

  std::vector<uint8_t> ipco_payload;
  append(ipco_payload, ispe);      // 1
  append(ipco_payload, mskC);      // 2
  append(ipco_payload, base_colr); // 3
  append(ipco_payload, gain_colr); // 4
  append(ipco_payload, tmap_colr); // 5
  auto ipco = make_box("ipco", ipco_payload);

  std::vector<uint8_t> ipma_payload;
  put_u32_be(ipma_payload, 5);
  for (uint16_t id = 1; id <= 5; ++id) {
    put_u16_be(ipma_payload, id);
    if (id == 1) {
      ipma_payload.push_back(3);
      ipma_payload.push_back(0x80 | 1);
      ipma_payload.push_back(0x80 | 2);
      ipma_payload.push_back(3);
    }
    else if (id == 2 || id == 4) {
      ipma_payload.push_back(3);
      ipma_payload.push_back(0x80 | 1);
      ipma_payload.push_back(0x80 | 2);
      ipma_payload.push_back(4);
    }
    else {
      ipma_payload.push_back(2);
      ipma_payload.push_back(0x80 | 1);
      ipma_payload.push_back(5);
    }
  }
  auto ipma = make_box("ipma", ipma_payload, true);

  std::vector<uint8_t> iprp_payload;
  append(iprp_payload, ipco);
  append(iprp_payload, ipma);
  auto iprp = make_box("iprp", iprp_payload);

  std::vector<uint8_t> idat_payload;
  std::vector<uint32_t> item_offsets;
  std::vector<uint32_t> item_lengths;
  for (uint16_t id = 1; id <= 5; ++id) {
    item_offsets.push_back(
        static_cast<uint32_t>(idat_payload.size()));
    if (id == 3 || id == 5) {
      item_lengths.push_back(
          static_cast<uint32_t>(tmap_payload.size()));
      idat_payload.insert(
          idat_payload.end(),
          tmap_payload.begin(), tmap_payload.end());
    }
    else {
      item_lengths.push_back(width * height);
      idat_payload.insert(
          idat_payload.end(), width * height, 0x7F);
    }
  }
  auto idat = make_box("idat", idat_payload);

  std::vector<uint8_t> iloc_payload;
  iloc_payload.push_back((4 << 4) | 4);
  iloc_payload.push_back(0);
  put_u16_be(iloc_payload, 5);
  for (uint16_t id = 1; id <= 5; ++id) {
    put_u16_be(iloc_payload, id);
    put_u16_be(iloc_payload, 0x0001);
    put_u16_be(iloc_payload, 0);
    put_u16_be(iloc_payload, 1);
    put_u32_be(iloc_payload, item_offsets[id - 1]);
    put_u32_be(iloc_payload, item_lengths[id - 1]);
  }
  auto iloc = make_box("iloc", iloc_payload, true, 1);

  std::vector<uint8_t> dimg_first;
  put_u16_be(dimg_first, 3);
  put_u16_be(dimg_first, 2);
  put_u16_be(dimg_first, 1);
  put_u16_be(dimg_first, 2);

  std::vector<uint8_t> dimg_second;
  put_u16_be(dimg_second, 5);
  put_u16_be(dimg_second, 2);
  put_u16_be(dimg_second, second_base_id);
  put_u16_be(dimg_second, 4);

  std::vector<uint8_t> iref_payload;
  append(iref_payload, make_box("dimg", dimg_first));
  append(iref_payload, make_box("dimg", dimg_second));
  auto iref = make_box("iref", iref_payload, true);

  std::vector<uint8_t> meta_payload;
  append(meta_payload, hdlr);
  append(meta_payload, pitm);
  append(meta_payload, iinf);
  append(meta_payload, iprp);
  append(meta_payload, iloc);
  append(meta_payload, iref);
  append(meta_payload, idat);
  auto meta = make_box("meta", meta_payload, true);

  std::vector<uint8_t> file;
  append(file, ftyp);
  append(file, meta);
  return file;
}

}  // namespace


TEST_CASE("tmap is a first-class image item with ordered inputs")
{
  std::vector<uint8_t> file = build_tmap_file();

  heif_context* context = nullptr;
  heif_image_handle* tmap = open_tmap(&context, file);

  REQUIRE(heif_image_handle_is_tone_map_derived_image(tmap) == 1);
  REQUIRE(heif_image_handle_get_item_id(tmap) == 3);
  REQUIRE(heif_image_handle_get_gain_map_metadata_status(tmap) ==
          heif_gain_map_metadata_status_parsed);

  heif_image_handle* base = nullptr;
  REQUIRE(heif_image_handle_get_tone_map_base_image_handle(
              tmap, &base).code == heif_error_Ok);
  REQUIRE(heif_image_handle_get_item_id(base) == 1);

  heif_image_handle* gain = nullptr;
  REQUIRE(heif_image_handle_get_tone_map_gain_map_image_handle(
              tmap, &gain).code == heif_error_Ok);
  REQUIRE(heif_image_handle_get_item_id(gain) == 2);

  heif_gain_map_metadata metadata{};
  REQUIRE(heif_image_handle_get_gain_map_metadata(
              tmap, &metadata).code == heif_error_Ok);
  REQUIRE(metadata.struct_version == 1);
  REQUIRE(metadata.minimum_version == 0);
  REQUIRE(metadata.writer_version == 0);
  REQUIRE(metadata.channel_count == 1);
  REQUIRE(metadata.use_base_colour_space == 1);
  REQUIRE(metadata.channels[0].gain_map_min.numerator == -1);
  REQUIRE(metadata.channels[0].gain_map_min.denominator == 1);
  REQUIRE(metadata.channels[0].gain_map_max.numerator == 1);

  heif_image_handle_release(gain);
  heif_image_handle_release(base);
  heif_image_handle_release(tmap);
  heif_context_free(context);
}


TEST_CASE("tmap base need not be the primary image")
{
  std::vector<uint8_t> file = build_tmap_file(
      2, 0, 0, 2, 2, 3);

  heif_context* context = nullptr;
  heif_image_handle* tmap = open_tmap(&context, file);

  heif_item_id primary_id = 0;
  REQUIRE(heif_context_get_primary_image_ID(
              context, &primary_id).code == heif_error_Ok);
  REQUIRE(primary_id == 3);

  heif_image_handle* base = nullptr;
  REQUIRE(heif_image_handle_get_tone_map_base_image_handle(
              tmap, &base).code == heif_error_Ok);
  REQUIRE(heif_image_handle_get_item_id(base) == 1);

  heif_image_handle_release(base);
  heif_image_handle_release(tmap);
  heif_context_free(context);
}


TEST_CASE("malformed tmap does not prevent base access")
{
  std::vector<uint8_t> file = build_tmap_file(1);

  heif_context* context = nullptr;
  heif_image_handle* tmap = open_tmap(&context, file);

  heif_image_handle* base = nullptr;
  REQUIRE(heif_context_get_image_handle(
              context, 1, &base).code == heif_error_Ok);

  heif_image_handle* input = nullptr;
  heif_error error =
      heif_image_handle_get_tone_map_base_image_handle(
          tmap, &input);
  REQUIRE(error.code == heif_error_Invalid_input);
  REQUIRE(input == nullptr);

  heif_image_handle_release(base);
  heif_image_handle_release(tmap);
  heif_context_free(context);
}


TEST_CASE("tmap rejects multiple dimg reference entries")
{
  std::vector<uint8_t> file = build_tmap_file(
      2, 0, 0, 2, 2, 1, false, true);

  heif_context* context = nullptr;
  heif_image_handle* tmap = open_tmap(&context, file);

  heif_image_handle* base = nullptr;
  heif_error error =
      heif_image_handle_get_tone_map_base_image_handle(
          tmap, &base);
  REQUIRE(error.code == heif_error_Invalid_input);
  REQUIRE(base == nullptr);

  heif_gain_map_metadata metadata{};
  error = heif_image_handle_get_gain_map_metadata(
      tmap, &metadata);
  REQUIRE(error.code == heif_error_Invalid_input);

  heif_image_handle_release(tmap);
  heif_context_free(context);
}


TEST_CASE("tmap reports unsupported outer and inner versions")
{
  {
    std::vector<uint8_t> file = build_tmap_file(
        2, 1, 0);
    heif_context* context = nullptr;
    heif_image_handle* tmap = open_tmap(&context, file);

    REQUIRE(heif_image_handle_get_gain_map_metadata_status(tmap) ==
            heif_gain_map_metadata_status_unsupported_tone_map_version);

    heif_gain_map_metadata metadata{};
    heif_error error = heif_image_handle_get_gain_map_metadata(
        tmap, &metadata);
    REQUIRE(error.code == heif_error_Unsupported_feature);
    REQUIRE(error.subcode == heif_suberror_Unsupported_data_version);

    heif_image_handle_release(tmap);
    heif_context_free(context);
  }

  {
    std::vector<uint8_t> file = build_tmap_file(
        2, 0, 1);
    heif_context* context = nullptr;
    heif_image_handle* tmap = open_tmap(&context, file);

    REQUIRE(heif_image_handle_get_gain_map_metadata_status(tmap) ==
            heif_gain_map_metadata_status_unsupported_minimum_version);

    heif_image_handle_release(tmap);
    heif_context_free(context);
  }
}


TEST_CASE("tmap validates gain-map NCLX role")
{
  std::vector<uint8_t> file = build_tmap_file(
      2, 0, 0, 1, 13);

  heif_context* context = nullptr;
  heif_image_handle* tmap = open_tmap(&context, file);

  REQUIRE(heif_image_handle_get_gain_map_metadata_status(tmap) ==
          heif_gain_map_metadata_status_parsed);

  heif_gain_map_metadata metadata{};
  heif_error error = heif_image_handle_get_gain_map_metadata(
      tmap, &metadata);
  REQUIRE(error.code == heif_error_Invalid_input);

  heif_image_handle_release(tmap);
  heif_context_free(context);
}


TEST_CASE("tmap reconstructs synthetic uncompressed inputs")
{
  std::vector<uint8_t> file = build_tmap_file();

  heif_context* context = nullptr;
  heif_image_handle* tmap = open_tmap(&context, file);

  heif_image* image = nullptr;
  heif_error error = heif_decode_image(
      tmap, &image,
      heif_colorspace_undefined,
      heif_chroma_undefined,
      nullptr);

  INFO(error.message);
  REQUIRE(error.code == heif_error_Ok);
  REQUIRE(image != nullptr);
  REQUIRE(heif_image_get_colorspace(image) == heif_colorspace_RGB);
  heif_image_release(image);

  heif_image_handle_release(tmap);
  heif_context_free(context);
}

TEST_CASE("Requested tmap output converts PQ samples to linear RGB")
{
  const bool fallback = GENERATE(false, true);
  GainMapMetadata metadata;
  metadata.channels[0].gain_map_min = {1, 1};
  metadata.channels[0].gain_map_max = {1, 1};
  const auto file = build_tmap_file(2, 0, fallback ? 1 : 0, 2, 2, 1, false, false, &metadata);
  heif_context* context = nullptr;
  auto* tmap = open_tmap(&context, file);
  auto* options = heif_decoding_options_alloc();
  REQUIRE(options);
  options->output_image_nclx_profile = heif_nclx_color_profile_alloc();
  REQUIRE(options->output_image_nclx_profile);
  auto* requested = options->output_image_nclx_profile;
  requested->color_primaries = heif_color_primaries_ITU_R_BT_709_5;
  requested->transfer_characteristics = heif_transfer_characteristic_linear;
  requested->matrix_coefficients = heif_matrix_coefficients_RGB_GBR;
  requested->full_range_flag = 1;
  heif_image* image = nullptr;
  const auto error = heif_decode_image(tmap, &image, heif_colorspace_RGB, heif_chroma_444, options);
  INFO(error.message);
  REQUIRE(error.code == heif_error_Ok);
  REQUIRE(image);
  const double linear = std::pow((127.0 / 255 + 0.055) / 1.055, 2.4) * (fallback ? 1 : 2);
  for (auto channel : {heif_channel_R, heif_channel_G, heif_channel_B}) {
    size_t stride = 0;
    const auto* pixels = reinterpret_cast<const uint16_t*>(heif_image_get_plane_readonly2(image, channel, &stride));
    REQUIRE(pixels);
    REQUIRE(pixels[0] == Catch::Approx(std::round(linear * 65535)).margin(4));
  }
  heif_color_profile_nclx* profile = nullptr;
  REQUIRE(heif_image_get_nclx_color_profile(image, &profile).code == heif_error_Ok);
  REQUIRE(profile->color_primaries == requested->color_primaries);
  REQUIRE(profile->transfer_characteristics == requested->transfer_characteristics);
  heif_nclx_color_profile_free(profile);
  heif_image_release(image);

  requested->transfer_characteristics = heif_transfer_characteristic_ITU_R_BT_470_6_System_M;
  image = nullptr;
  REQUIRE(heif_decode_image(tmap, &image, heif_colorspace_RGB, heif_chroma_444, options).code ==
          heif_error_Unsupported_feature);
  REQUIRE(image == nullptr);
  heif_nclx_color_profile_free(requested);
  options->output_image_nclx_profile = nullptr;
  heif_decoding_options_free(options);
  heif_image_handle_release(tmap);
  heif_context_free(context);
}

TEST_CASE("HEIF prem alpha is reconstructed and retained through requested root output")
{
  GainMapMetadata metadata;
  metadata.channels[0].gain_map_min = {1, 1};
  metadata.channels[0].gain_map_max = {1, 1};
  metadata.channels[0].base_offset = {1, 8};
  metadata.channels[0].alternate_offset = {1, 16};
  const auto file = build_tmap_file(2, 0, 0, 2, 2, 1, false, false, &metadata, true);
  heif_context* context = nullptr;
  auto* tmap = open_tmap(&context, file);
  auto* options = heif_decoding_options_alloc();
  REQUIRE(options);
  auto* requested = heif_nclx_color_profile_alloc();
  REQUIRE(requested);
  requested->color_primaries = heif_color_primaries_ITU_R_BT_709_5;
  requested->transfer_characteristics = heif_transfer_characteristic_linear;
  requested->matrix_coefficients = heif_matrix_coefficients_RGB_GBR;
  requested->full_range_flag = 1;
  options->output_image_nclx_profile = requested;
  heif_image* image = nullptr;
  const auto error = heif_decode_image(tmap, &image, heif_colorspace_RGB, heif_chroma_444, options);
  INFO(error.message);
  REQUIRE(error.code == heif_error_Ok);
  REQUIRE(image);
  REQUIRE(heif_image_is_premultiplied_alpha(image));
  size_t stride = 0;
  const auto* colour = heif_image_get_plane_readonly2(image, heif_channel_R, &stride);
  REQUIRE(colour);
  REQUIRE(reinterpret_cast<const uint16_t*>(colour)[0] == 0);
  const double linear = (std::pow((0.6 + 0.055) / 1.055, 2.4) + 0.125) * 2 - 0.0625;
  REQUIRE(reinterpret_cast<const uint16_t*>(colour)[1] == Catch::Approx(linear / 3 * 65535).margin(5));
  size_t alpha_stride = 0;
  const auto* alpha = heif_image_get_plane_readonly2(image, heif_channel_Alpha, &alpha_stride);
  REQUIRE(alpha);
  REQUIRE(reinterpret_cast<const uint16_t*>(alpha)[0] == 0);
  REQUIRE(reinterpret_cast<const uint16_t*>(alpha)[1] == 21845);
  heif_image_release(image);
  heif_nclx_color_profile_free(requested);
  options->output_image_nclx_profile = nullptr;
  heif_decoding_options_free(options);
  heif_image_handle_release(tmap);
  heif_context_free(context);
}


TEST_CASE("Target headroom decode matches independent ISO and PQ values in both directions")
{
  const bool reverse = GENERATE(false, true);
  GainMapMetadata metadata;
  metadata.base_hdr_headroom = {reverse ? 2u : 0u, 1};
  metadata.alternate_hdr_headroom = {reverse ? 0u : 2u, 1};
  metadata.channels[0].gain_map_min = {2, 1};
  metadata.channels[0].gain_map_max = {2, 1};
  // Nonzero unequal offsets ensure the weighted path retains Formula (2).
  metadata.channels[0].base_offset = {1, 64};
  metadata.channels[0].alternate_offset = {1, 128};
  const auto file = build_tmap_file(2, 0, 0, 2, 2, 1, false, false, &metadata);
  heif_context* ctx = nullptr;
  auto* tmap = open_tmap(&ctx, file);
  const double signal = 127.0 / 255.0;
  const double baseline = std::pow((signal + 0.055) / 1.055, 2.4);
  // ST 2084 constants; no production gain-map or colour helper is used.
  const auto pq = [](double linear) {
    const double power = std::pow(linear * 203.0 / 10000.0, 2610.0 / 16384.0);
    return std::pow((3424.0 / 4096.0 + 2413.0 / 128.0 * power) /
                    (1.0 + 2392.0 / 128.0 * power), 2523.0 / 32.0);
  };
  for (const double target : {0.0, 1.0, 2.0, 3.0}) {
    heif_image* image = nullptr;
    const auto error = heif_decode_tone_map_image(tmap, &image, heif_colorspace_RGB,
                                                 heif_chroma_444, nullptr, target);
    INFO(error.message);
    REQUIRE(error.code == heif_error_Ok);
    REQUIRE(image);
    const double weight = reverse ? (target >= 2 ? 0 : (target - 2) / 2) :
                                   (target >= 2 ? 1 : target / 2);
    const double linear = (baseline + 1.0 / 64) * std::exp2(weight * 2) - 1.0 / 128;
    for (auto channel : {heif_channel_R, heif_channel_G, heif_channel_B}) {
      size_t stride = 0;
      const auto* pixels = reinterpret_cast<const uint16_t*>(
          heif_image_get_plane_readonly2(image, channel, &stride));
      REQUIRE(pixels);
      REQUIRE(pixels[0] == Catch::Approx(pq(linear) * 65535).margin(3));
    }
    heif_color_profile_nclx* profile = nullptr;
    REQUIRE(heif_image_get_nclx_color_profile(image, &profile).code == heif_error_Ok);
    REQUIRE(profile->transfer_characteristics == heif_transfer_characteristic_ITU_R_BT_2100_0_PQ);
    heif_nclx_color_profile_free(profile);
    heif_image_release(image);
  }
  heif_image* canonical = nullptr;
  heif_image* endpoint = nullptr;
  REQUIRE(heif_decode_image(tmap, &canonical, heif_colorspace_RGB, heif_chroma_444, nullptr).code == heif_error_Ok);
  REQUIRE(heif_decode_tone_map_image(tmap, &endpoint, heif_colorspace_RGB, heif_chroma_444,
                                    nullptr, reverse ? 0 : 2).code == heif_error_Ok);
  size_t stride = 0;
  REQUIRE(reinterpret_cast<const uint16_t*>(heif_image_get_plane_readonly2(canonical, heif_channel_R, &stride))[0] ==
          reinterpret_cast<const uint16_t*>(heif_image_get_plane_readonly2(endpoint, heif_channel_R, &stride))[0]);
  heif_image_release(canonical);
  heif_image_release(endpoint);
  heif_image_handle_release(tmap);
  heif_context_free(ctx);
}

TEST_CASE("Target headroom API rejects invalid arguments and preserves version fallback")
{
  const auto file = build_tmap_file();
  heif_context* ctx = nullptr;
  auto* tmap = open_tmap(&ctx, file);
  heif_image* image = nullptr;
  for (double target : {-1.0, std::numeric_limits<double>::infinity(),
                         std::numeric_limits<double>::quiet_NaN()}) {
    REQUIRE(heif_decode_tone_map_image(tmap, &image, heif_colorspace_undefined,
                                      heif_chroma_undefined, nullptr, target).code == heif_error_Usage_error);
    REQUIRE(image == nullptr);
  }
  heif_image_handle* base = nullptr;
  REQUIRE(heif_image_handle_get_tone_map_base_image_handle(tmap, &base).code == heif_error_Ok);
  REQUIRE(heif_decode_tone_map_image(base, &image, heif_colorspace_undefined,
                                    heif_chroma_undefined, nullptr, 1).code == heif_error_Usage_error);
  REQUIRE(image == nullptr);
  REQUIRE(heif_decode_tone_map_image(nullptr, &image, heif_colorspace_undefined,
                                    heif_chroma_undefined, nullptr, 1).code == heif_error_Usage_error);
  REQUIRE(heif_decode_tone_map_image(tmap, nullptr, heif_colorspace_undefined,
                                    heif_chroma_undefined, nullptr, 1).code == heif_error_Usage_error);
  heif_image_handle_release(base);
  heif_image_handle_release(tmap);
  heif_context_free(ctx);

  const auto unknown = build_tmap_file(2, 0, 1);
  tmap = open_tmap(&ctx, unknown);
  REQUIRE(heif_decode_tone_map_image(tmap, &image, heif_colorspace_undefined,
                                    heif_chroma_undefined, nullptr, 1).code == heif_error_Ok);
  REQUIRE(heif_image_get_colorspace(image) == heif_colorspace_monochrome);
  size_t stride = 0;
  REQUIRE(heif_image_get_plane_readonly2(image, heif_channel_Y, &stride)[0] == 127);
  heif_image_release(image);
  heif_image_handle_release(tmap);
  heif_context_free(ctx);
}

TEST_CASE("Requested root output leaves nested PQ and HLG tmap colour operations canonical")
{
  const uint16_t transfer = GENERATE(uint16_t{16}, uint16_t{18});
  const auto file = build_two_tmap_file(3, transfer);
  auto* context = heif_context_alloc();
  REQUIRE(heif_context_read_from_memory_without_copy(context, file.data(), file.size(), nullptr).code == heif_error_Ok);
  heif_image_handle* outer = nullptr;
  REQUIRE(heif_context_get_image_handle(context, 5, &outer).code == heif_error_Ok);
  auto* options = heif_decoding_options_alloc();
  REQUIRE(options);
  auto* requested = heif_nclx_color_profile_alloc();
  REQUIRE(requested);
  requested->color_primaries = heif_color_primaries_ITU_R_BT_709_5;
  requested->transfer_characteristics = heif_transfer_characteristic_linear;
  requested->matrix_coefficients = heif_matrix_coefficients_RGB_GBR;
  requested->full_range_flag = 1;
  options->output_image_nclx_profile = requested;
  const double baseline = std::pow((127.0 / 255 + 0.055) / 1.055, 2.4);
  const double log_gain = -1 + 2 * 127.0 / 255;
  for (double target : {1.0, 2.0}) {
    heif_image* image = nullptr;
    const auto error = heif_decode_tone_map_image(outer, &image, heif_colorspace_RGB,
                                                  heif_chroma_444, options, target);
    INFO(error.message);
    REQUIRE(error.code == heif_error_Ok);
    size_t stride = 0;
    const auto* pixels = reinterpret_cast<const uint16_t*>(heif_image_get_plane_readonly2(image, heif_channel_R, &stride));
    REQUIRE(pixels);
    // Inner always applies fully; only the root gets the target weight.
    const double expected = baseline * std::exp2((1 + target / 2) * log_gain);
    REQUIRE(pixels[0] == Catch::Approx(std::round(expected * 65535)).margin(5));
    heif_image_release(image);
  }
  heif_nclx_color_profile_free(requested);
  options->output_image_nclx_profile = nullptr;
  heif_decoding_options_free(options);
  heif_image_handle_release(outer);
  heif_context_free(context);
}

TEST_CASE("multiple tmap items can share the same base")
{
  std::vector<uint8_t> file = build_two_tmap_file(1);

  heif_context* context = heif_context_alloc();
  REQUIRE(context != nullptr);
  REQUIRE(heif_context_read_from_memory(
              context, file.data(), file.size(), nullptr).code ==
          heif_error_Ok);

  heif_image_handle* first = nullptr;
  heif_image_handle* second = nullptr;
  REQUIRE(heif_context_get_image_handle(
              context, 3, &first).code == heif_error_Ok);
  REQUIRE(heif_context_get_image_handle(
              context, 5, &second).code == heif_error_Ok);

  heif_image_handle* first_base = nullptr;
  heif_image_handle* second_base = nullptr;
  REQUIRE(heif_image_handle_get_tone_map_base_image_handle(
              first, &first_base).code == heif_error_Ok);
  REQUIRE(heif_image_handle_get_tone_map_base_image_handle(
              second, &second_base).code == heif_error_Ok);

  REQUIRE(heif_image_handle_get_item_id(first_base) == 1);
  REQUIRE(heif_image_handle_get_item_id(second_base) == 1);
  REQUIRE(heif_image_handle_get_gain_map_metadata_status(first) ==
          heif_gain_map_metadata_status_parsed);
  REQUIRE(heif_image_handle_get_gain_map_metadata_status(second) ==
          heif_gain_map_metadata_status_parsed);

  heif_image_handle_release(second_base);
  heif_image_handle_release(first_base);
  heif_image_handle_release(second);
  heif_image_handle_release(first);
  heif_context_free(context);
}


TEST_CASE("tmap input may itself be a derived tmap image")
{
  std::vector<uint8_t> file = build_two_tmap_file(3);

  heif_context* context = heif_context_alloc();
  REQUIRE(context != nullptr);
  REQUIRE(heif_context_read_from_memory(
              context, file.data(), file.size(), nullptr).code ==
          heif_error_Ok);

  heif_image_handle* outer = nullptr;
  REQUIRE(heif_context_get_image_handle(
              context, 5, &outer).code == heif_error_Ok);

  heif_image_handle* derived_base = nullptr;
  REQUIRE(heif_image_handle_get_tone_map_base_image_handle(
              outer, &derived_base).code == heif_error_Ok);
  REQUIRE(heif_image_handle_get_item_id(derived_base) == 3);
  REQUIRE(heif_image_handle_is_tone_map_derived_image(
              derived_base) == 1);

  heif_gain_map_metadata metadata{};
  REQUIRE(heif_image_handle_get_gain_map_metadata(
              outer, &metadata).code == heif_error_Ok);

  heif_image_handle_release(derived_base);
  heif_image_handle_release(outer);
  heif_context_free(context);
}

TEST_CASE("Unknown minimum metadata version decodes baseline without alternate colour tagging")
{
  std::vector<uint8_t> file = build_tmap_file(2, 0, 1);
  heif_context* ctx = nullptr;
  auto* tmap = open_tmap(&ctx, file);
  heif_image* image = nullptr;
  const auto error = heif_decode_image(tmap, &image, heif_colorspace_undefined, heif_chroma_undefined, nullptr);
  INFO(error.message);
  REQUIRE(error.code == heif_error_Ok);
  REQUIRE(image);
  heif_color_profile_nclx* profile = nullptr;
  REQUIRE(heif_image_get_nclx_color_profile(image, &profile).code == heif_error_Ok);
  REQUIRE(profile->color_primaries == heif_color_primaries_ITU_R_BT_709_5);
  REQUIRE(profile->transfer_characteristics == heif_transfer_characteristic_IEC_61966_2_1);
  size_t stride = 0;
  const auto* pixels = heif_image_get_plane_readonly2(image, heif_channel_Y, &stride);
  REQUIRE(pixels);
  REQUIRE(pixels[0] == 127);
  heif_nclx_color_profile_free(profile);
  heif_image_release(image);
  heif_image_handle_release(tmap);
  heif_context_free(ctx);
}

TEST_CASE("Nested tmap decode fully reconstructs both derived nodes")
{
  const uint16_t transfer = GENERATE(uint16_t{16}, uint16_t{18});
  const auto file = build_two_tmap_file(3, transfer);
  auto* ctx = heif_context_alloc();
  REQUIRE(heif_context_read_from_memory_without_copy(ctx, file.data(), file.size(), nullptr).code == heif_error_Ok);
  heif_image_handle* handle = nullptr;
  REQUIRE(heif_context_get_image_handle(ctx, 5, &handle).code == heif_error_Ok);
  heif_image* image = nullptr;
  const auto error = heif_decode_image(handle, &image, heif_colorspace_undefined, heif_chroma_undefined, nullptr);
  INFO(error.message);
  REQUIRE(error.code == heif_error_Ok);
  REQUIRE(image);
  REQUIRE(heif_image_get_bits_per_pixel_range(image, heif_channel_R) == 16);
  heif_color_profile_nclx* profile = nullptr;
  REQUIRE(heif_image_get_nclx_color_profile(image, &profile).code == heif_error_Ok);
  REQUIRE(profile->color_primaries == heif_color_primaries_ITU_R_BT_2020_2_and_2100_0);
  REQUIRE(profile->transfer_characteristics == transfer);
  heif_nclx_color_profile_free(profile);
  heif_image_release(image);
  heif_image_handle_release(handle);
  heif_context_free(ctx);
}

TEST_CASE("Target headroom is consumed only by the root of a nested tmap")
{
  const uint16_t transfer = GENERATE(uint16_t{16}, uint16_t{18});
  const auto file = build_two_tmap_file(3, transfer);
  auto* ctx = heif_context_alloc();
  REQUIRE(heif_context_read_from_memory_without_copy(ctx, file.data(), file.size(), nullptr).code == heif_error_Ok);
  heif_image_handle* inner = nullptr;
  heif_image_handle* outer = nullptr;
  REQUIRE(heif_context_get_image_handle(ctx, 3, &inner).code == heif_error_Ok);
  REQUIRE(heif_context_get_image_handle(ctx, 5, &outer).code == heif_error_Ok);
  heif_image* full_inner = nullptr;
  heif_image* weighted_outer = nullptr;
  REQUIRE(heif_decode_image(inner, &full_inner, heif_colorspace_RGB, heif_chroma_444, nullptr).code == heif_error_Ok);
  REQUIRE(heif_decode_tone_map_image(outer, &weighted_outer, heif_colorspace_RGB,
                                    heif_chroma_444, nullptr, 0).code == heif_error_Ok);
  // Outer W=0 must retain the fully reconstructed inner baseline. Passing the
  // target recursively would instead remove the inner gain as well.
  for (auto channel : {heif_channel_R, heif_channel_G, heif_channel_B}) {
    size_t inner_stride = 0, outer_stride = 0;
    const auto* a = heif_image_get_plane_readonly2(full_inner, channel, &inner_stride);
    const auto* b = heif_image_get_plane_readonly2(weighted_outer, channel, &outer_stride);
    REQUIRE(a);
    REQUIRE(b);
    for (size_t y = 0; y < 2; ++y) {
      for (size_t x = 0; x < 2; ++x) {
        REQUIRE(reinterpret_cast<const uint16_t*>(a + y * inner_stride)[x] ==
                Catch::Approx(reinterpret_cast<const uint16_t*>(b + y * outer_stride)[x]).margin(1));
      }
    }
  }
  heif_image_release(full_inner);
  heif_image_release(weighted_outer);
  heif_image_handle_release(inner);
  heif_image_handle_release(outer);
  heif_context_free(ctx);
}

TEST_CASE("Baseline fallback applies child transforms when root transforms are suppressed")
{
  const auto file = build_tmap_file(2, 0, 1, 2, 2, 1, true);
  heif_context* ctx = nullptr;
  auto* tmap = open_tmap(&ctx, file);
  auto* options = heif_decoding_options_alloc();
  options->ignore_transformations = true;
  heif_image* image = nullptr;
  const auto error = heif_decode_image(tmap, &image, heif_colorspace_undefined, heif_chroma_undefined, options);
  INFO(error.message);
  REQUIRE(error.code == heif_error_Ok);
  size_t stride = 0;
  const auto* pixels = heif_image_get_plane_readonly2(image, heif_channel_Y, &stride);
  REQUIRE(pixels);
  REQUIRE(pixels[0] == 63);
  REQUIRE(pixels[1] == 255);
  heif_image_release(image);
  heif_decoding_options_free(options);
  heif_image_handle_release(tmap);
  heif_context_free(ctx);
}
