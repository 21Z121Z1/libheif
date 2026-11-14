/*
  libheif GB/T 48347-2026 HEIF dual-layer integration tests

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

// GB/T 48347-2026 clause 9.3 HEIF dual-layer format. The 'tmap' item payload is
// the ISO 21496-1 ToneMapImage (table 27 is byte-identical), so reconstruction
// reuses the ISO path; these tests add the GB/T-specific 'it35'/UWAInfo item,
// the DRE-AIG graph checks, and an independent numeric HDR/SDR oracle.

#include "catch_amalgamated.hpp"
#include "gbt48347.h"
#include "libheif/heif.h"
#include "libheif/heif_experimental.h"
#include "libheif/heif_items.h"
#include "test_utils.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace
{
heif_error memory_write(heif_context*, const void* data, size_t size, void* userdata)
{
  auto* out = static_cast<std::vector<uint8_t>*>(userdata);
  const auto* bytes = static_cast<const uint8_t*>(data);
  out->insert(out->end(), bytes, bytes + size);
  return {heif_error_Ok, heif_suberror_Unspecified, nullptr};
}

std::vector<uint8_t> write_context(heif_context* ctx)
{
  std::vector<uint8_t> encoded;
  heif_writer writer{};
  writer.writer_api_version = 1;
  writer.write = memory_write;
  REQUIRE(heif_context_write(ctx, &writer, &encoded).code == heif_error_Ok);
  return encoded;
}

heif_context* reopen(const std::vector<uint8_t>& encoded)
{
  heif_context* ctx = heif_context_alloc();
  REQUIRE(heif_context_read_from_memory_without_copy(
              ctx, encoded.data(), encoded.size(), nullptr).code == heif_error_Ok);
  return ctx;
}

heif_color_profile_nclx nclx(heif_color_primaries p, heif_transfer_characteristics t,
                             heif_matrix_coefficients m, bool full)
{
  heif_color_profile_nclx n{};
  n.version = 1;
  n.color_primaries = p;
  n.transfer_characteristics = t;
  n.matrix_coefficients = m;
  n.full_range_flag = full ? 1 : 0;
  return n;
}

// Independent sRGB EOTF (IEC 61966-2-1), not the library's colour code.
double srgb_eotf(double codeword)
{
  return codeword <= 0.04045 ? codeword / 12.92
                             : std::pow((codeword + 0.055) / 1.055, 2.4);
}

heif_image* make_rgb_image(int w, int h, uint8_t value, const heif_color_profile_nclx& profile)
{
  heif_image* image = nullptr;
  REQUIRE(heif_image_create(w, h, heif_colorspace_RGB, heif_chroma_444, &image).code == heif_error_Ok);
  for (auto c : {heif_channel_R, heif_channel_G, heif_channel_B}) {
    REQUIRE(heif_image_add_plane(image, c, w, h, 8).code == heif_error_Ok);
    int stride = 0;
    auto* plane = heif_image_get_plane(image, c, &stride);
    for (int y = 0; y < h; ++y) { std::memset(plane + y * stride, value, w); }
  }
  REQUIRE(heif_image_set_nclx_color_profile(image, &profile).code == heif_error_Ok);
  return image;
}

heif_image* make_mono_image(int w, int h, uint8_t value, const heif_color_profile_nclx& profile)
{
  heif_image* image = nullptr;
  REQUIRE(heif_image_create(w, h, heif_colorspace_monochrome, heif_chroma_monochrome, &image).code == heif_error_Ok);
  REQUIRE(heif_image_add_plane(image, heif_channel_Y, w, h, 8).code == heif_error_Ok);
  int stride = 0;
  auto* plane = heif_image_get_plane(image, heif_channel_Y, &stride);
  for (int y = 0; y < h; ++y) { std::memset(plane + y * stride, value, w); }
  REQUIRE(heif_image_set_nclx_color_profile(image, &profile).code == heif_error_Ok);
  return image;
}

heif_image_handle* encode(heif_context* ctx, heif_encoder* encoder, heif_image* image,
                          const heif_color_profile_nclx& profile)
{
  auto* options = heif_encoding_options_alloc();
  options->output_nclx_profile = const_cast<heif_color_profile_nclx*>(&profile);
  heif_image_handle* handle = nullptr;
  REQUIRE(heif_context_encode_image(ctx, image, encoder, options, &handle).code == heif_error_Ok);
  heif_encoding_options_free(options);
  heif_image_release(image);
  return handle;
}

heif_gain_map_metadata metadata()
{
  heif_gain_map_metadata m{};
  m.struct_version = 1;
  m.minimum_version = 0;
  m.writer_version = 0;
  m.channel_count = 1;
  m.use_base_colour_space = 1;
  m.base_hdr_headroom = {0, 1};
  m.alternate_hdr_headroom = {2, 1};
  m.channels[0].gain_map_min = {-1, 1};
  m.channels[0].gain_map_max = {1, 1};
  m.channels[0].gamma = {1, 1};
  m.channels[0].base_offset = {0, 1};
  m.channels[0].alternate_offset = {0, 1};
  return m;
}

// Find the single 'it35' item and return its raw payload.
std::vector<uint8_t> read_it35(heif_context* ctx, heif_item_id* out_id)
{
  heif_item_id ids[32];
  const int count = heif_context_get_list_of_item_IDs(ctx, ids, 32);
  for (int i = 0; i < count; ++i) {
    if (heif_item_get_item_type(ctx, ids[i]) == heif_fourcc('i', 't', '3', '5')) {
      uint8_t* data = nullptr;
      size_t size = 0;
      REQUIRE(heif_item_get_item_data(ctx, ids[i], nullptr, &data, &size).code == heif_error_Ok);
      std::vector<uint8_t> payload(data, data + size);
      heif_release_item_data(ctx, &data);
      if (out_id) { *out_id = ids[i]; }
      return payload;
    }
  }
  return {};
}
}  // namespace


TEST_CASE("GB/T HEIF dual-layer file carries it35 UWAInfo and a valid tmap graph")
{
  auto* encoder = get_encoder_or_skip_test(heif_compression_uncompressed);
  auto* ctx = heif_context_alloc();
  const auto base_profile = nclx(heif_color_primaries_ITU_R_BT_709_5,
                                 heif_transfer_characteristic_IEC_61966_2_1,
                                 heif_matrix_coefficients_RGB_GBR, true);
  const auto gain_profile = nclx(heif_color_primaries_unspecified,
                                 heif_transfer_characteristic_unspecified,
                                 heif_matrix_coefficients_RGB_GBR, true);
  auto* base = encode(ctx, encoder, make_rgb_image(4, 4, 192, base_profile), base_profile);
  auto* gain = encode(ctx, encoder, make_mono_image(4, 4, 255, gain_profile), gain_profile);
  // GB/T 9.3.3a: the enhancement image item shall be hidden.
  REQUIRE(heif_item_set_item_hidden(ctx, heif_image_handle_get_item_id(gain), 1).code == heif_error_Ok);

  auto md = metadata();
  const auto alternate = nclx(heif_color_primaries_ITU_R_BT_709_5,
                              heif_transfer_characteristic_ITU_R_BT_2100_0_PQ,
                              heif_matrix_coefficients_RGB_GBR, true);
  heif_tone_map_options options{};
  options.version = 1;
  options.alternate_nclx = &alternate;
  heif_image_handle* tmap = nullptr;
  REQUIRE(heif_context_add_tone_map_derived_image(ctx, base, gain, &md, &options, &tmap).code == heif_error_Ok);
  const auto tmap_id = heif_image_handle_get_item_id(tmap);

  // GB/T it35 item: enhancement UWAInfo with static + dynamic metadata and an
  // extend_info_extension carrying an fp32 identity base mapping.
  gbt48347::UWAInfo info;
  info.extended_frame_number = 2;
  info.extended_type = gbt48347::kExtendedTypeEnhance;  // meta=1, enhance=1
  info.metadata_present = true;
  info.static_metadata = {0x00, 0x01, 0x02, 0x03};
  info.dynamic_metadata = {0x10, 0x20};
  info.extension_present = true;
  info.extension.base_present = true;
  info.extension.base.flag = 32;
  info.extension.base.matrix = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  auto uwa = gbt48347::serialize_uwa_info(info);
  REQUIRE(uwa);
  heif_item_id it35_id = 0;
  REQUIRE(heif_context_add_item(ctx, "it35", uwa->data(), static_cast<int>(uwa->size()), &it35_id).code == heif_error_Ok);

  const auto encoded = write_context(ctx);
  auto* read_ctx = reopen(encoded);

  // it35 discovery + UWAInfo parse.
  heif_item_id found_id = 0;
  const auto payload = read_it35(read_ctx, &found_id);
  REQUIRE_FALSE(payload.empty());
  CHECK(found_id == it35_id);
  auto parsed = gbt48347::parse_uwa_info(payload);
  REQUIRE(parsed.status == gbt48347::ParseStatus::parsed);
  CHECK(parsed.info->extended_type == gbt48347::kExtendedTypeEnhance);
  CHECK(parsed.info->static_metadata == info.static_metadata);
  CHECK(parsed.info->dynamic_metadata == info.dynamic_metadata);
  REQUIRE(parsed.info->extension_present);
  REQUIRE(parsed.info->extension.base_present);
  CHECK(parsed.info->extension.base.flag == 32);
  CHECK(parsed.info->extension.base.matrix[0] == 1.0f);
  CHECK(parsed.info->extension.base.matrix[4] == 1.0f);

  // GB/T graph: tmap with two ordered dimg inputs, gain hidden.
  heif_image_handle* read_tmap = nullptr;
  REQUIRE(heif_context_get_image_handle(read_ctx, tmap_id, &read_tmap).code == heif_error_Ok);
  CHECK(heif_image_handle_is_tone_map_derived_image(read_tmap) == 1);
  heif_image_handle* read_base = nullptr;
  heif_image_handle* read_gain = nullptr;
  REQUIRE(heif_image_handle_get_tone_map_base_image_handle(read_tmap, &read_base).code == heif_error_Ok);
  REQUIRE(heif_image_handle_get_tone_map_gain_map_image_handle(read_tmap, &read_gain).code == heif_error_Ok);
  CHECK(heif_image_handle_get_item_id(read_base) == heif_image_handle_get_item_id(base));
  CHECK(heif_image_handle_get_item_id(read_gain) == heif_image_handle_get_item_id(gain));
  CHECK(heif_item_is_item_hidden(read_ctx, heif_image_handle_get_item_id(read_gain)) == 1);

  heif_image_handle_release(read_base);
  heif_image_handle_release(read_gain);
  heif_image_handle_release(read_tmap);
  heif_context_free(read_ctx);
  heif_image_handle_release(tmap);
  heif_image_handle_release(gain);
  heif_image_handle_release(base);
  heif_encoder_release(encoder);
  heif_context_free(ctx);
}


TEST_CASE("GB/T HEIF HDR reconstruction matches an independent ISO numeric oracle")
{
  auto* encoder = get_encoder_or_skip_test(heif_compression_uncompressed);
  auto* ctx = heif_context_alloc();
  const auto base_profile = nclx(heif_color_primaries_ITU_R_BT_709_5,
                                 heif_transfer_characteristic_IEC_61966_2_1,
                                 heif_matrix_coefficients_RGB_GBR, true);
  const auto gain_profile = nclx(heif_color_primaries_unspecified,
                                 heif_transfer_characteristic_unspecified,
                                 heif_matrix_coefficients_RGB_GBR, true);
  const uint8_t base_codeword = 127;
  auto* base = encode(ctx, encoder, make_rgb_image(4, 4, base_codeword, base_profile), base_profile);
  // gain normalized = 1.0 -> unnormalized log2 gain = (max-min)*1 + min = 1.
  auto* gain = encode(ctx, encoder, make_mono_image(4, 4, 255, gain_profile), gain_profile);

  auto md = metadata();
  const auto alternate = nclx(heif_color_primaries_ITU_R_BT_709_5,
                              heif_transfer_characteristic_ITU_R_BT_2100_0_PQ,
                              heif_matrix_coefficients_RGB_GBR, true);
  heif_tone_map_options options{};
  options.version = 1;
  options.alternate_nclx = &alternate;
  heif_image_handle* tmap = nullptr;
  REQUIRE(heif_context_add_tone_map_derived_image(ctx, base, gain, &md, &options, &tmap).code == heif_error_Ok);
  const auto encoded = write_context(ctx);
  auto* read_ctx = reopen(encoded);
  heif_image_handle* read_tmap = nullptr;
  REQUIRE(heif_context_get_image_handle(read_ctx, heif_image_handle_get_item_id(tmap), &read_tmap).code == heif_error_Ok);
  heif_image_handle* read_base = nullptr;
  REQUIRE(heif_context_get_image_handle(read_ctx, heif_image_handle_get_item_id(base), &read_base).code == heif_error_Ok);

  // Independent ISO 21496-1 oracle: sRGB EOTF (IEC 61966-2-1) then Formula (2)
  // with W=1, log2 gain=1, zero offsets -> alternate = 2 * base_linear.
  const double base_linear = srgb_eotf(base_codeword / 255.0);
  const double expected_hdr = 2.0 * base_linear;

  auto* dec = heif_decoding_options_alloc();
  dec->output_image_nclx_profile = heif_nclx_color_profile_alloc();
  dec->output_image_nclx_profile->color_primaries = heif_color_primaries_ITU_R_BT_709_5;
  dec->output_image_nclx_profile->transfer_characteristics = heif_transfer_characteristic_linear;
  dec->output_image_nclx_profile->matrix_coefficients = heif_matrix_coefficients_RGB_GBR;
  dec->output_image_nclx_profile->full_range_flag = 1;

  auto linear_pixel = [&](heif_image_handle* handle) {
    heif_image* image = nullptr;
    REQUIRE(heif_decode_image(handle, &image, heif_colorspace_RGB, heif_chroma_444, dec).code == heif_error_Ok);
    size_t stride = 0;
    const auto* pixels = reinterpret_cast<const uint16_t*>(
        heif_image_get_plane_readonly2(image, heif_channel_R, &stride));
    const double value = pixels[0] / 65535.0;
    heif_image_release(image);
    return value;
  };

  const double hdr = linear_pixel(read_tmap);   // fully-applied alternate (HDR)
  INFO("base_linear=" << base_linear << " hdr=" << hdr << " expected_hdr=" << expected_hdr);
  // HDR direction: independent ISO 21496-1 Formula (2) oracle.
  CHECK(std::abs(hdr - expected_hdr) < 1e-4);

  // SDR direction (GB/T 11.2): the base image is the SDR rendition; decoding it
  // in its native profile must return the original sRGB codeword unchanged.
  heif_image* sdr_image = nullptr;
  REQUIRE(heif_decode_image(read_base, &sdr_image, heif_colorspace_RGB, heif_chroma_444, nullptr).code == heif_error_Ok);
  int sdr_stride = 0;
  const auto* sdr_plane = heif_image_get_plane_readonly(sdr_image, heif_channel_R, &sdr_stride);
  CHECK(sdr_plane[0] == base_codeword);
  heif_image_release(sdr_image);

  heif_nclx_color_profile_free(dec->output_image_nclx_profile);
  dec->output_image_nclx_profile = nullptr;
  heif_decoding_options_free(dec);
  heif_image_handle_release(read_base);
  heif_image_handle_release(read_tmap);
  heif_context_free(read_ctx);
  heif_image_handle_release(tmap);
  heif_image_handle_release(gain);
  heif_image_handle_release(base);
  heif_encoder_release(encoder);
  heif_context_free(ctx);
}


TEST_CASE("GB/T it35 with invalid T.35 identifiers is rejected on parse")
{
  gbt48347::UWAInfo info;
  info.extended_type = gbt48347::kExtendedTypeBase;
  auto valid = gbt48347::serialize_uwa_info(info);
  REQUIRE(valid);
  auto bad = *valid;
  bad[0] = 0xFF;  // wrong country code
  CHECK(gbt48347::parse_uwa_info(bad).status == gbt48347::ParseStatus::malformed);

  // Truncated it35 payload.
  const std::vector<uint8_t> truncated(valid->begin(), valid->begin() + 4);
  CHECK(gbt48347::parse_uwa_info(truncated).status == gbt48347::ParseStatus::malformed);
}