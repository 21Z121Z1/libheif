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

#include <cstdint>
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
    uint16_t minimum_version = 0)
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
    uint16_t primary_item = 1)
{
  constexpr uint32_t width = 2;
  constexpr uint32_t height = 2;

  std::vector<uint8_t> tmap_payload =
      make_tone_map_payload(outer_version, minimum_version);

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
  put_u16_be(iinf_payload, 3);
  for (uint16_t id = 1; id <= 3; ++id) {
    std::vector<uint8_t> infe;
    put_u16_be(infe, id);
    put_u16_be(infe, 0);
    append_fourcc(infe, id == 3 ? "tmap" : "mski");
    infe.push_back(0);
    append(iinf_payload,
           make_box("infe", infe, true, 2, id == 2 ? 1 : 0));
  }
  auto iinf = make_box("iinf", iinf_payload, true);

  std::vector<uint8_t> ispe_payload;
  put_u32_be(ispe_payload, width);
  put_u32_be(ispe_payload, height);
  auto ispe = make_box("ispe", ispe_payload, true);

  auto mskC = make_box("mskC", std::vector<uint8_t>{8}, true);
  auto base_colr = make_nclx(1, 13, 1, true);
  auto gain_colr =
      make_nclx(gain_primaries, gain_transfer, 0, true);
  auto tmap_colr = make_nclx(9, 16, 9, true);

  std::vector<uint8_t> ipco_payload;
  append(ipco_payload, ispe);
  append(ipco_payload, mskC);
  append(ipco_payload, base_colr);
  append(ipco_payload, gain_colr);
  append(ipco_payload, tmap_colr);
  auto ipco = make_box("ipco", ipco_payload);

  std::vector<uint8_t> ipma_payload;
  put_u32_be(ipma_payload, 3);

  put_u16_be(ipma_payload, 1);
  ipma_payload.push_back(3);
  ipma_payload.push_back(0x80 | 1);
  ipma_payload.push_back(0x80 | 2);
  ipma_payload.push_back(3);

  put_u16_be(ipma_payload, 2);
  ipma_payload.push_back(3);
  ipma_payload.push_back(0x80 | 1);
  ipma_payload.push_back(0x80 | 2);
  ipma_payload.push_back(4);

  put_u16_be(ipma_payload, 3);
  ipma_payload.push_back(2);
  ipma_payload.push_back(0x80 | 1);
  ipma_payload.push_back(5);

  auto ipma = make_box("ipma", ipma_payload, true);

  std::vector<uint8_t> iprp_payload;
  append(iprp_payload, ipco);
  append(iprp_payload, ipma);
  auto iprp = make_box("iprp", iprp_payload);

  std::vector<uint8_t> idat_payload(width * height * 2, 0x7F);
  idat_payload.insert(
      idat_payload.end(), tmap_payload.begin(), tmap_payload.end());
  auto idat = make_box("idat", idat_payload);

  std::vector<uint8_t> iloc_payload;
  iloc_payload.push_back((4 << 4) | 4);
  iloc_payload.push_back(0);
  put_u16_be(iloc_payload, 3);

  const uint32_t item_lengths[3] = {
      width * height,
      width * height,
      static_cast<uint32_t>(tmap_payload.size())
  };
  uint32_t offset = 0;
  for (uint16_t id = 1; id <= 3; ++id) {
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
  auto iref = make_box(
      "iref", make_box("dimg", dimg_payload), true);

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


TEST_CASE("tmap decode is explicitly unsupported until reconstruction lands")
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

  REQUIRE(error.code == heif_error_Unsupported_feature);
  REQUIRE(image == nullptr);

  heif_image_handle_release(tmap);
  heif_context_free(context);
}
