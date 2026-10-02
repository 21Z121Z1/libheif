/*
  libheif ISO 21496-1 HEIF tmap writer tests

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
#include "libheif/heif.h"
#include "libheif/heif_experimental.h"
#include "libheif/heif_items.h"
#include "libheif/heif_properties.h"
#include "libheif/heif_entity_groups.h"
#include "test_utils.h"
#include "gain_map_color.h"

#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>


namespace
{

heif_error memory_writer(
    heif_context*,
    const void* data,
    size_t size,
    void* userdata)
{
  auto* output = static_cast<std::vector<uint8_t>*>(userdata);
  const auto* bytes = static_cast<const uint8_t*>(data);
  output->insert(output->end(), bytes, bytes + size);
  return {heif_error_Ok, heif_suberror_Unspecified, nullptr};
}


heif_color_profile_nclx make_nclx(
    heif_color_primaries primaries,
    heif_transfer_characteristics transfer,
    heif_matrix_coefficients matrix,
    bool full_range)
{
  heif_color_profile_nclx nclx{};
  nclx.version = 1;
  nclx.color_primaries = primaries;
  nclx.transfer_characteristics = transfer;
  nclx.matrix_coefficients = matrix;
  nclx.full_range_flag = full_range ? 1 : 0;
  return nclx;
}


heif_image* make_ycbcr_image(
    const heif_color_profile_nclx& nclx)
{
  constexpr int width = 4;
  constexpr int height = 4;

  heif_image* image = nullptr;
  REQUIRE(heif_image_create(
              width, height,
              heif_colorspace_YCbCr,
              heif_chroma_420,
              &image).code == heif_error_Ok);
  REQUIRE(image != nullptr);

  fill_new_plane(
      image, heif_channel_Y, width, height);
  fill_new_plane(
      image, heif_channel_Cb, width / 2, height / 2);
  fill_new_plane(
      image, heif_channel_Cr, width / 2, height / 2);
  REQUIRE(heif_image_set_nclx_color_profile(
              image, &nclx).code == heif_error_Ok);

  return image;
}


heif_image_handle* encode_image_with_profile(
    heif_context* ctx,
    heif_encoder* encoder,
    const heif_color_profile_nclx& nclx)
{
  heif_image* image = make_ycbcr_image(nclx);
  heif_encoding_options* options =
      heif_encoding_options_alloc();
  REQUIRE(options != nullptr);
  options->output_nclx_profile =
      const_cast<heif_color_profile_nclx*>(&nclx);

  heif_image_handle* handle = nullptr;
  heif_error error = heif_context_encode_image(
      ctx, image, encoder, options, &handle);
  INFO((error.message ? error.message : ""));
  REQUIRE(error.code == heif_error_Ok);
  REQUIRE(handle != nullptr);

  heif_encoding_options_free(options);
  heif_image_release(image);
  return handle;
}


heif_gain_map_metadata make_metadata()
{
  heif_gain_map_metadata metadata{};
  metadata.struct_version = 1;
  metadata.minimum_version = 0;
  metadata.writer_version = 0;
  metadata.channel_count = 1;
  metadata.use_base_colour_space = 1;
  metadata.base_hdr_headroom = {0, 1};
  metadata.alternate_hdr_headroom = {2, 1};
  metadata.channels[0].gain_map_min = {-1, 1};
  metadata.channels[0].gain_map_max = {1, 1};
  metadata.channels[0].gamma = {1, 1};
  metadata.channels[0].base_offset = {0, 1};
  metadata.channels[0].alternate_offset = {0, 1};
  return metadata;
}


heif_tone_map_options make_options(
    const heif_color_profile_nclx* alternate)
{
  heif_tone_map_options options{};
  options.version = 1;
  options.alternate_nclx = alternate;
  return options;
}


std::vector<uint8_t> write_context(heif_context* ctx)
{
  std::vector<uint8_t> encoded;
  heif_writer writer{};
  writer.writer_api_version = 1;
  writer.write = memory_writer;
  heif_error error =
      heif_context_write(ctx, &writer, &encoded);
  INFO((error.message ? error.message : ""));
  REQUIRE(error.code == heif_error_Ok);
  return encoded;
}


heif_context* reopen(
    const std::vector<uint8_t>& encoded)
{
  heif_context* ctx = heif_context_alloc();
  REQUIRE(ctx != nullptr);
  heif_error error =
      heif_context_read_from_memory_without_copy(
          ctx, encoded.data(), encoded.size(), nullptr);
  INFO((error.message ? error.message : ""));
  REQUIRE(error.code == heif_error_Ok);
  return ctx;
}


bool item_has_property(
    heif_context* ctx,
    heif_item_id id,
    uint32_t type)
{
  const int count =
      heif_item_get_properties_of_type(
          ctx, id, heif_item_property_type_invalid,
          nullptr, 0);
  std::vector<heif_property_id> properties(
      static_cast<size_t>(count));
  if (count > 0) {
    REQUIRE(heif_item_get_properties_of_type(
                ctx, id,
                heif_item_property_type_invalid,
                properties.data(), count) == count);
  }

  for (heif_property_id property : properties) {
    if (static_cast<uint32_t>(
            heif_item_get_property_type(
                ctx, id, property)) == type) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST_CASE("Generate synthetic HEVC tmap files for independent consumers")
{
  const char* directory = std::getenv("LIBHEIF_TMAP_WRITE_DIR");
  if (!directory) { return; }
  std::filesystem::create_directories(directory);
  REQUIRE(heif_have_encoder_for_format(heif_compression_HEVC));
  for (bool rgb : {false, true}) {
    auto* ctx = heif_context_alloc();
    REQUIRE(ctx);
    auto* encoder = get_encoder_or_skip_test(heif_compression_HEVC);
    REQUIRE(heif_encoder_set_lossless(encoder, 1).code == heif_error_Ok);
    REQUIRE(heif_encoder_set_logging_level(encoder, 0).code == heif_error_Ok);
    const auto baseline = make_nclx(heif_color_primaries_ITU_R_BT_709_5,
        heif_transfer_characteristic_IEC_61966_2_1,
        heif_matrix_coefficients_ITU_R_BT_601_6, true);
    const auto alternate = make_nclx(heif_color_primaries_ITU_R_BT_709_5,
        heif_transfer_characteristic_ITU_R_BT_2100_0_PQ,
        heif_matrix_coefficients_RGB_GBR, true);
    heif_image* base_pixels = nullptr;
    REQUIRE(heif_image_create(64, 64, heif_colorspace_RGB,
                             heif_chroma_444, &base_pixels).code == heif_error_Ok);
    for (auto c : {heif_channel_R, heif_channel_G, heif_channel_B}) {
      REQUIRE(heif_image_add_plane(base_pixels, c, 64, 64, 8).code == heif_error_Ok);
      int stride = 0;
      auto* plane = heif_image_get_plane(base_pixels, c, &stride);
      REQUIRE(plane);
      for (int y = 0; y < 64; ++y) { std::memset(plane + y * stride, 192, 64); }
    }
    REQUIRE(heif_image_set_nclx_color_profile(base_pixels, &baseline).code == heif_error_Ok);
    auto* encoding = heif_encoding_options_alloc();
    encoding->output_nclx_profile = const_cast<heif_color_profile_nclx*>(&baseline);
    heif_image_handle* base = nullptr;
    auto error = heif_context_encode_image(ctx, base_pixels, encoder, encoding, &base);
    INFO(error.message);
    REQUIRE(error.code == heif_error_Ok);
    heif_encoding_options_free(encoding);
    heif_image_release(base_pixels);

    heif_image* gain_pixels = nullptr;
    REQUIRE(heif_image_create(64, 64, rgb ? heif_colorspace_RGB : heif_colorspace_monochrome,
        rgb ? heif_chroma_444 : heif_chroma_monochrome, &gain_pixels).code == heif_error_Ok);
    const std::vector<heif_channel> channels = rgb ?
        std::vector<heif_channel>{heif_channel_R, heif_channel_G, heif_channel_B} :
        std::vector<heif_channel>{heif_channel_Y};
    for (auto c : channels) {
      REQUIRE(heif_image_add_plane(gain_pixels, c, 64, 64, 8).code == heif_error_Ok);
      int stride = 0;
      auto* plane = heif_image_get_plane(gain_pixels, c, &stride);
      REQUIRE(plane);
      const uint8_t value = c == heif_channel_G ? 128 : (c == heif_channel_B ? 64 : 255);
      for (int y = 0; y < 64; ++y) { std::memset(plane + y * stride, value, 64); }
    }
    heif_image_handle* gain = nullptr;
    error = heif_context_encode_gain_map_image(ctx, gain_pixels, encoder, nullptr, nullptr, &gain);
    INFO(error.message);
    REQUIRE(error.code == heif_error_Ok);
    heif_image_release(gain_pixels);
    auto metadata = make_metadata();
    metadata.channel_count = rgb ? 3 : 1;
    for (uint8_t c = 0; c < metadata.channel_count; ++c) {
      metadata.channels[c] = metadata.channels[0];
      metadata.channels[c].gain_map_min = {0, 1};
      metadata.channels[c].gain_map_max = {c == 0 ? 2 : 1, 1};
    }
    auto* options = heif_tone_map_options_alloc();
    options->alternate_nclx = &alternate;
    options->pixi_num_channels = 3;
    for (uint8_t& bits : options->pixi_bits_per_channel) { bits = 16; }
    heif_image_handle* tmap = nullptr;
    error = heif_context_add_tone_map_derived_image(ctx, base, gain, &metadata, options, &tmap);
    INFO(error.message);
    REQUIRE(error.code == heif_error_Ok);
    auto* decode_options = heif_decoding_options_alloc();
    decode_options->output_image_nclx_profile_passthrough = true;
    heif_image* reconstructed = nullptr;
    error = heif_decode_image(tmap, &reconstructed, heif_colorspace_RGB, heif_chroma_444, decode_options);
    INFO(error.message);
    REQUIRE(error.code == heif_error_Ok);
    const double base_linear = std::pow((192.0 / 255.0 + 0.055) / 1.055, 2.4);
    uint8_t component = 0;
    for (auto channel : {heif_channel_R, heif_channel_G, heif_channel_B}) {
      int stride = 0;
      const auto* plane = heif_image_get_plane_readonly(reconstructed, channel, &stride);
      REQUIRE(plane);
      uint16_t sample = 0;
      std::memcpy(&sample, plane, sizeof(sample));
      auto linear = gain_map_decode_transfer(sample / 65535.0, 16);
      REQUIRE(linear);
      const double log_gain = !rgb || component == 0 ? 2.0 :
                              (component == 1 ? 128.0 : 64.0) / 255.0;
      REQUIRE(*linear == Catch::Approx(base_linear * std::exp2(log_gain)).margin(0.025));
      ++component;
    }
    heif_image_release(reconstructed);
    heif_decoding_options_free(decode_options);
    const auto bytes = write_context(ctx);
    const auto path = std::filesystem::path(directory) / (rgb ? "libheif-rgb-pq.heic" : "libheif-mono-pq.heic");
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(output.good());
    heif_image_handle_release(tmap);
    heif_tone_map_options_free(options);
    heif_image_handle_release(gain);
    heif_image_handle_release(base);
    heif_encoder_release(encoder);
    heif_context_free(ctx);
  }
}

TEST_CASE("Gain-map encoder defaults to hidden mono without selecting primary")
{
  auto* ctx = heif_context_alloc();
  auto* encoder = get_encoder_or_skip_test(heif_compression_AV1);
  auto* options = heif_gain_map_image_options_alloc();
  REQUIRE(options);
  REQUIRE(options->hidden == 1);
  heif_image* pixels = nullptr;
  REQUIRE(heif_image_create(4, 4, heif_colorspace_monochrome,
                           heif_chroma_monochrome, &pixels).code == heif_error_Ok);
  fill_new_plane(pixels, heif_channel_Y, 4, 4);
  const auto original_profile = make_nclx(heif_color_primaries_ITU_R_BT_709_5,
      heif_transfer_characteristic_IEC_61966_2_1, heif_matrix_coefficients_unspecified, true);
  REQUIRE(heif_image_set_nclx_color_profile(pixels, &original_profile).code == heif_error_Ok);

  heif_image_handle* gain = nullptr;
  REQUIRE(heif_context_encode_gain_map_image(ctx, pixels, encoder, nullptr,
                                            options, &gain).code == heif_error_Ok);
  const auto gain_id = heif_image_handle_get_item_id(gain);
  REQUIRE(heif_item_is_item_hidden(ctx, gain_id));
  REQUIRE(heif_context_get_number_of_top_level_images(ctx) == 0);
  REQUIRE(item_has_property(ctx, gain_id, heif_fourcc('c','o','l','r')));
  heif_color_profile_nclx* profile = nullptr;
  REQUIRE(heif_image_get_nclx_color_profile(pixels, &profile).code == heif_error_Ok);
  REQUIRE(profile->color_primaries == original_profile.color_primaries);
  REQUIRE(profile->transfer_characteristics == original_profile.transfer_characteristics);
  heif_nclx_color_profile_free(profile);

  auto base_profile = make_nclx(heif_color_primaries_ITU_R_BT_709_5,
      heif_transfer_characteristic_IEC_61966_2_1, heif_matrix_coefficients_ITU_R_BT_709_5, true);
  auto* base = encode_image_with_profile(ctx, encoder, base_profile);
  auto alternate = base_profile;
  alternate.transfer_characteristics = heif_transfer_characteristic_ITU_R_BT_2100_0_PQ;
  auto metadata = make_metadata();
  auto* tmap_options = heif_tone_map_options_alloc();
  REQUIRE(tmap_options);
  tmap_options->alternate_nclx = &alternate;
  heif_image_handle* tmap = nullptr;
  REQUIRE(heif_context_add_tone_map_derived_image(ctx, base, gain, &metadata,
                                                tmap_options, &tmap).code == heif_error_Ok);
  const auto tmap_id = heif_image_handle_get_item_id(tmap);
  heif_item_id primary = 0;
  REQUIRE(heif_context_get_primary_image_ID(ctx, &primary).code == heif_error_Ok);
  REQUIRE(primary == heif_image_handle_get_item_id(base));
  REQUIRE(heif_context_is_top_level_image_ID(ctx, tmap_id));
  const auto items_before = heif_context_get_number_of_items(ctx);
  heif_image_handle* duplicate = nullptr;
  const auto duplicate_error = heif_context_add_tone_map_derived_image(
      ctx, base, gain, &metadata, tmap_options, &duplicate);
  REQUIRE(duplicate_error.code == heif_error_Usage_error);
  REQUIRE(duplicate == nullptr);
  REQUIRE(heif_context_get_number_of_items(ctx) == items_before);
  REQUIRE(heif_item_set_item_hidden(ctx, tmap_id, 1).code == heif_error_Usage_error);
  REQUIRE_FALSE(heif_item_is_item_hidden(ctx, tmap_id));
  const auto encoded = write_context(ctx);
  auto* read_ctx = reopen(encoded);
  REQUIRE(heif_item_is_item_hidden(read_ctx, gain_id));
  int group_count = 0;
  auto* groups = heif_context_get_entity_groups(read_ctx, heif_entity_group_altr, 0, &group_count);
  REQUIRE(group_count == 1);
  REQUIRE(groups[0].num_entities == 2);
  REQUIRE(groups[0].entities[0] == tmap_id);
  REQUIRE(groups[0].entities[1] == primary);
  heif_entity_groups_release(groups, group_count);
  heif_image_handle* read_tmap = nullptr;
  REQUIRE(heif_context_get_image_handle(read_ctx, tmap_id, &read_tmap).code == heif_error_Ok);
  heif_image* decoded = nullptr;
  auto error = heif_decode_image(read_tmap, &decoded, heif_colorspace_undefined,
                                 heif_chroma_undefined, nullptr);
  INFO(error.message);
  REQUIRE(error.code == heif_error_Ok);
  REQUIRE(decoded);
  heif_image_release(decoded);
  heif_image_handle_release(read_tmap);
  heif_context_free(read_ctx);
  heif_tone_map_options_free(tmap_options);
  heif_gain_map_image_options_free(options);
  heif_image_release(pixels);
  heif_image_handle_release(tmap);
  heif_image_handle_release(gain);
  heif_image_handle_release(base);
  heif_encoder_release(encoder);
  heif_context_free(ctx);
}

TEST_CASE("Gain-map encoder rejects ambiguous YCbCr and preserves primary on error")
{
  auto* ctx = heif_context_alloc();
  auto* encoder = get_encoder_or_skip_test(heif_compression_AV1);
  const auto colour = make_nclx(heif_color_primaries_ITU_R_BT_709_5,
      heif_transfer_characteristic_IEC_61966_2_1, heif_matrix_coefficients_ITU_R_BT_709_5, true);
  auto* base = encode_image_with_profile(ctx, encoder, colour);
  auto* pixels = make_ycbcr_image(colour);
  heif_image_handle* gain = nullptr;
  auto error = heif_context_encode_gain_map_image(ctx, pixels, encoder, nullptr, nullptr, &gain);
  REQUIRE(error.code == heif_error_Usage_error);
  REQUIRE(gain == nullptr);
  const auto gain_colour = make_nclx(heif_color_primaries_unspecified,
      heif_transfer_characteristic_unspecified, heif_matrix_coefficients_ITU_R_BT_709_5, false);
  heif_gain_map_image_options gain_options{1, &gain_colour, 0};
  REQUIRE(heif_context_encode_gain_map_image(ctx, pixels, encoder, nullptr,
                                            &gain_options, &gain).code == heif_error_Ok);
  REQUIRE_FALSE(heif_item_is_item_hidden(ctx, heif_image_handle_get_item_id(gain)));
  heif_color_profile_nclx* profile = nullptr;
  REQUIRE(heif_image_handle_get_nclx_color_profile(gain, &profile).code == heif_error_Ok);
  REQUIRE(profile->color_primaries == heif_color_primaries_unspecified);
  REQUIRE(profile->transfer_characteristics == heif_transfer_characteristic_unspecified);
  REQUIRE(profile->matrix_coefficients == gain_colour.matrix_coefficients);
  REQUIRE(profile->full_range_flag == 0);
  heif_nclx_color_profile_free(profile);
  heif_item_id primary = 0;
  REQUIRE(heif_context_get_primary_image_ID(ctx, &primary).code == heif_error_Ok);
  REQUIRE(primary == heif_image_handle_get_item_id(base));
  auto encoded = write_context(ctx);
  REQUIRE_FALSE(heif_has_compatible_brand(encoded.data(), static_cast<int>(encoded.size()), "tmap"));
  heif_image_release(pixels);
  heif_image_handle_release(gain);
  heif_image_handle_release(base);
  heif_encoder_release(encoder);
  heif_context_free(ctx);
}

TEST_CASE("Tone-map writer preserves exact ICC bytes and supports canonical reconstruction")
{
  auto* ctx = heif_context_alloc();
  auto* encoder = get_encoder_or_skip_test(heif_compression_AV1);
  auto colour = make_nclx(heif_color_primaries_ITU_R_BT_709_5,
      heif_transfer_characteristic_IEC_61966_2_1, heif_matrix_coefficients_ITU_R_BT_709_5, true);
  auto* base = encode_image_with_profile(ctx, encoder, colour);
  auto gain_colour = colour;
  gain_colour.color_primaries = heif_color_primaries_unspecified;
  gain_colour.transfer_characteristics = heif_transfer_characteristic_unspecified;
  auto* gain = encode_image_with_profile(ctx, encoder, gain_colour);
  auto metadata = make_metadata();
  // Synthetic ICC header plus a CICP tag. No external profile is redistributed.
  std::vector<uint8_t> icc(156, 0);
  icc[3] = 156;
  std::memcpy(icc.data() + 12, "mntrRGB XYZ ", 12);
  std::memcpy(icc.data() + 36, "acsp", 4);
  icc[131] = 1;
  std::memcpy(icc.data() + 132, "cicp", 4);
  icc[139] = 144;
  icc[143] = 12;
  std::memcpy(icc.data() + 144, "cicp", 4);
  icc[152] = 9; icc[153] = 16; icc[154] = 0; icc[155] = 1;
  auto* options = heif_tone_map_options_alloc();
  options->alternate_icc_type = heif_color_profile_type_prof;
  options->alternate_icc = icc.data();
  options->alternate_icc_size = icc.size();
  heif_image_handle* tmap = nullptr;
  REQUIRE(heif_context_add_tone_map_derived_image(ctx, base, gain, &metadata,
                                                options, &tmap).code == heif_error_Ok);
  const auto tmap_id = heif_image_handle_get_item_id(tmap);
  const auto encoded = write_context(ctx);
  auto* read_ctx = reopen(encoded);
  heif_image_handle* read_tmap = nullptr;
  REQUIRE(heif_context_get_image_handle(read_ctx, tmap_id, &read_tmap).code == heif_error_Ok);
  REQUIRE(heif_image_handle_get_color_profile_type(read_tmap) == heif_color_profile_type_prof);
  REQUIRE(heif_image_handle_get_raw_color_profile_size(read_tmap) == icc.size());
  std::vector<uint8_t> read_icc(icc.size());
  REQUIRE(heif_image_handle_get_raw_color_profile(read_tmap, read_icc.data()).code == heif_error_Ok);
  REQUIRE(read_icc == icc);
  heif_image* decoded = nullptr;
  const auto error = heif_decode_image(read_tmap, &decoded, heif_colorspace_undefined,
                                       heif_chroma_undefined, nullptr);
  INFO(error.message);
  REQUIRE(error.code == heif_error_Ok);
  REQUIRE(heif_image_get_color_profile_type(decoded) == heif_color_profile_type_prof);
  REQUIRE(heif_image_get_bits_per_pixel_range(decoded, heif_channel_R) == 16);
  heif_image_release(decoded);
  heif_image_handle_release(read_tmap);
  heif_context_free(read_ctx);
  heif_tone_map_options_free(options);
  heif_image_handle_release(tmap);
  heif_image_handle_release(gain);
  heif_image_handle_release(base);
  heif_encoder_release(encoder);
  heif_context_free(ctx);
}


TEST_CASE("tmap writer round-trips graph metadata colour and brand")
{
  heif_context* ctx = heif_context_alloc();
  REQUIRE(ctx != nullptr);
  heif_encoder* encoder =
      get_encoder_or_skip_test(heif_compression_AV1);

  const auto base_nclx = make_nclx(
      heif_color_primaries_ITU_R_BT_709_5,
      heif_transfer_characteristic_IEC_61966_2_1,
      heif_matrix_coefficients_ITU_R_BT_709_5,
      true);
  const auto gain_nclx = make_nclx(
      heif_color_primaries_unspecified,
      heif_transfer_characteristic_unspecified,
      heif_matrix_coefficients_ITU_R_BT_709_5,
      true);
  const auto alternate_nclx = make_nclx(
      heif_color_primaries_ITU_R_BT_2020_2_and_2100_0,
      heif_transfer_characteristic_ITU_R_BT_2100_0_PQ,
      heif_matrix_coefficients_ITU_R_BT_2020_2_non_constant_luminance,
      true);

  heif_image_handle* base =
      encode_image_with_profile(ctx, encoder, base_nclx);
  heif_image_handle* gain =
      encode_image_with_profile(ctx, encoder, gain_nclx);
  heif_encoder_release(encoder);

  const heif_item_id base_id =
      heif_image_handle_get_item_id(base);
  const heif_item_id gain_id =
      heif_image_handle_get_item_id(gain);

  heif_item_id primary_id = 0;
  REQUIRE(heif_context_get_primary_image_ID(
              ctx, &primary_id).code == heif_error_Ok);
  REQUIRE(primary_id == base_id);

  heif_gain_map_metadata metadata = make_metadata();
  heif_tone_map_options options =
      make_options(&alternate_nclx);
  options.has_clli = 1;
  options.clli = {1000, 400};
  options.pixi_num_channels = 3;
  options.pixi_bits_per_channel[0] = 12;
  options.pixi_bits_per_channel[1] = 12;
  options.pixi_bits_per_channel[2] = 12;

  heif_image_handle* tmap = nullptr;
  heif_error error =
      heif_context_add_tone_map_derived_image(
          ctx, base, gain, &metadata, &options, &tmap);
  INFO((error.message ? error.message : ""));
  REQUIRE(error.code == heif_error_Ok);
  REQUIRE(tmap != nullptr);

  const heif_item_id tmap_id =
      heif_image_handle_get_item_id(tmap);
  REQUIRE(tmap_id != base_id);
  REQUIRE(tmap_id != gain_id);
  REQUIRE(heif_image_handle_is_tone_map_derived_image(tmap) == 1);

  // Exercise codec-config queries and reconstruction before serialization.
  // Encoded AV1 items used to have a null decoder here.
  REQUIRE(heif_image_handle_get_luma_bits_per_pixel(gain) == 8);
  REQUIRE(heif_image_handle_get_chroma_bits_per_pixel(gain) == 8);
  heif_image* reconstructed = nullptr;
  error = heif_decode_image(tmap, &reconstructed,
                           heif_colorspace_undefined, heif_chroma_undefined, nullptr);
  INFO((error.message ? error.message : ""));
  REQUIRE(error.code == heif_error_Ok);
  REQUIRE(reconstructed != nullptr);
  REQUIRE(heif_image_get_bits_per_pixel_range(reconstructed, heif_channel_R) == 16);
  heif_image_release(reconstructed);

  REQUIRE(heif_context_get_primary_image_ID(
              ctx, &primary_id).code == heif_error_Ok);
  REQUIRE(primary_id == base_id);

  std::vector<uint8_t> encoded = write_context(ctx);
  REQUIRE(heif_has_compatible_brand(
              encoded.data(),
              static_cast<int>(encoded.size()),
              "tmap") == 1);

  heif_image_handle_release(tmap);
  heif_image_handle_release(gain);
  heif_image_handle_release(base);
  heif_context_free(ctx);

  ctx = reopen(encoded);

  REQUIRE(heif_context_get_primary_image_ID(
              ctx, &primary_id).code == heif_error_Ok);
  REQUIRE(primary_id == base_id);

  heif_image_handle* read_tmap = nullptr;
  REQUIRE(heif_context_get_image_handle(
              ctx, tmap_id, &read_tmap).code == heif_error_Ok);
  REQUIRE(read_tmap != nullptr);
  REQUIRE(heif_image_handle_is_tone_map_derived_image(
              read_tmap) == 1);

  heif_item_id* references = nullptr;
  uint32_t reference_type = 0;
  const size_t reference_count =
      heif_context_get_item_references(
          ctx, tmap_id, 0,
          &reference_type, &references);
  REQUIRE(reference_type == heif_fourcc('d', 'i', 'm', 'g'));
  REQUIRE(reference_count == 2);
  REQUIRE(references != nullptr);
  REQUIRE(references[0] == base_id);
  REQUIRE(references[1] == gain_id);
  heif_release_item_references(ctx, &references);

  heif_gain_map_metadata round_trip{};
  REQUIRE(heif_image_handle_get_gain_map_metadata(
              read_tmap, &round_trip).code ==
          heif_error_Ok);
  REQUIRE(round_trip.minimum_version == 0);
  REQUIRE(round_trip.writer_version == 0);
  REQUIRE(round_trip.channel_count == 1);
  REQUIRE(round_trip.channels[0].gain_map_min.numerator == -1);
  REQUIRE(round_trip.channels[0].gain_map_max.numerator == 1);
  REQUIRE(round_trip.alternate_hdr_headroom.numerator == 2);

  heif_color_profile_nclx* read_nclx = nullptr;
  REQUIRE(heif_image_handle_get_nclx_color_profile(
              read_tmap, &read_nclx).code ==
          heif_error_Ok);
  REQUIRE(read_nclx != nullptr);
  REQUIRE(read_nclx->color_primaries ==
          heif_color_primaries_ITU_R_BT_2020_2_and_2100_0);
  REQUIRE(read_nclx->transfer_characteristics ==
          heif_transfer_characteristic_ITU_R_BT_2100_0_PQ);
  heif_nclx_color_profile_free(read_nclx);

  heif_content_light_level clli{};
  REQUIRE(heif_image_handle_get_content_light_level(
              read_tmap, &clli) == 1);
  REQUIRE(clli.max_content_light_level == 1000);
  REQUIRE(clli.max_pic_average_light_level == 400);

  REQUIRE(item_has_property(
      ctx, tmap_id, heif_fourcc('p', 'i', 'x', 'i')));
  REQUIRE(item_has_property(
      ctx, tmap_id, heif_fourcc('i', 's', 'p', 'e')));
  REQUIRE(item_has_property(
      ctx, tmap_id, heif_fourcc('c', 'o', 'l', 'r')));

  heif_image_handle_release(read_tmap);
  heif_context_free(ctx);
}


TEST_CASE("tmap writer supports shared base and does not invent PIXI")
{
  heif_context* ctx = heif_context_alloc();
  REQUIRE(ctx != nullptr);
  heif_encoder* encoder =
      get_encoder_or_skip_test(heif_compression_AV1);

  const auto base_nclx = make_nclx(
      heif_color_primaries_ITU_R_BT_709_5,
      heif_transfer_characteristic_IEC_61966_2_1,
      heif_matrix_coefficients_ITU_R_BT_709_5,
      true);
  const auto gain_nclx = make_nclx(
      heif_color_primaries_unspecified,
      heif_transfer_characteristic_unspecified,
      heif_matrix_coefficients_ITU_R_BT_709_5,
      true);
  const auto alternate_nclx = make_nclx(
      heif_color_primaries_ITU_R_BT_2020_2_and_2100_0,
      heif_transfer_characteristic_ITU_R_BT_2100_0_PQ,
      heif_matrix_coefficients_ITU_R_BT_2020_2_non_constant_luminance,
      true);

  heif_image_handle* base =
      encode_image_with_profile(ctx, encoder, base_nclx);
  const heif_item_id base_id =
      heif_image_handle_get_item_id(base);
  heif_image_handle* gain_a =
      encode_image_with_profile(ctx, encoder, gain_nclx);
  heif_image_handle* gain_b =
      encode_image_with_profile(ctx, encoder, gain_nclx);
  heif_encoder_release(encoder);

  heif_gain_map_metadata metadata = make_metadata();
  heif_tone_map_options options =
      make_options(&alternate_nclx);

  heif_image_handle* first = nullptr;
  heif_image_handle* second = nullptr;
  REQUIRE(heif_context_add_tone_map_derived_image(
              ctx, base, gain_a,
              &metadata, &options, &first).code ==
          heif_error_Ok);
  REQUIRE(heif_context_add_tone_map_derived_image(
              ctx, base, gain_b,
              &metadata, &options, &second).code ==
          heif_error_Ok);

  const heif_item_id first_id =
      heif_image_handle_get_item_id(first);
  const heif_item_id second_id =
      heif_image_handle_get_item_id(second);
  REQUIRE(first_id != second_id);

  std::vector<uint8_t> encoded = write_context(ctx);

  heif_image_handle_release(second);
  heif_image_handle_release(first);
  heif_image_handle_release(gain_b);
  heif_image_handle_release(gain_a);
  heif_image_handle_release(base);
  heif_context_free(ctx);

  ctx = reopen(encoded);

  for (heif_item_id id : {first_id, second_id}) {
    heif_image_handle* handle = nullptr;
    REQUIRE(heif_context_get_image_handle(
                ctx, id, &handle).code == heif_error_Ok);
    REQUIRE(handle != nullptr);
    REQUIRE(!item_has_property(
        ctx, id, heif_fourcc('p', 'i', 'x', 'i')));

    heif_image_handle* read_base = nullptr;
    REQUIRE(heif_image_handle_get_tone_map_base_image_handle(
                handle, &read_base).code ==
            heif_error_Ok);
    REQUIRE(heif_image_handle_get_item_id(read_base) ==
            base_id);

    heif_image_handle_release(read_base);
    heif_image_handle_release(handle);
  }

  heif_context_free(ctx);
}


TEST_CASE("tmap writer rejects invalid roles without changing primary")
{
  heif_context* ctx = heif_context_alloc();
  REQUIRE(ctx != nullptr);
  heif_encoder* encoder =
      get_encoder_or_skip_test(heif_compression_AV1);

  const auto base_nclx = make_nclx(
      heif_color_primaries_ITU_R_BT_709_5,
      heif_transfer_characteristic_IEC_61966_2_1,
      heif_matrix_coefficients_ITU_R_BT_709_5,
      true);
  const auto wrong_gain_nclx = make_nclx(
      heif_color_primaries_ITU_R_BT_709_5,
      heif_transfer_characteristic_IEC_61966_2_1,
      heif_matrix_coefficients_ITU_R_BT_709_5,
      true);
  const auto alternate_nclx = make_nclx(
      heif_color_primaries_ITU_R_BT_2020_2_and_2100_0,
      heif_transfer_characteristic_ITU_R_BT_2100_0_PQ,
      heif_matrix_coefficients_ITU_R_BT_2020_2_non_constant_luminance,
      true);

  heif_image_handle* base =
      encode_image_with_profile(ctx, encoder, base_nclx);
  heif_image_handle* gain =
      encode_image_with_profile(ctx, encoder, wrong_gain_nclx);
  heif_encoder_release(encoder);

  const heif_item_id base_id =
      heif_image_handle_get_item_id(base);
  heif_gain_map_metadata metadata = make_metadata();
  heif_tone_map_options options =
      make_options(&alternate_nclx);

  heif_image_handle* tmap = nullptr;
  heif_error error =
      heif_context_add_tone_map_derived_image(
          ctx, base, gain, &metadata, &options, &tmap);
  REQUIRE(error.code == heif_error_Invalid_input);
  REQUIRE(tmap == nullptr);

  heif_item_id primary_id = 0;
  REQUIRE(heif_context_get_primary_image_ID(
              ctx, &primary_id).code == heif_error_Ok);
  REQUIRE(primary_id == base_id);

  error = heif_context_add_tone_map_derived_image(
      ctx, base, base, &metadata, &options, &tmap);
  REQUIRE(error.code == heif_error_Usage_error);
  REQUIRE(tmap == nullptr);

  heif_image_handle_release(gain);
  heif_image_handle_release(base);
  heif_context_free(ctx);
}

TEST_CASE("tmap writer preflights primary visibility group and ICC errors")
{
  auto* ctx = heif_context_alloc();
  auto* encoder = get_encoder_or_skip_test(heif_compression_AV1);
  auto gain_colour = make_nclx(heif_color_primaries_unspecified,
      heif_transfer_characteristic_unspecified, heif_matrix_coefficients_ITU_R_BT_709_5, true);
  auto* gain = encode_image_with_profile(ctx, encoder, gain_colour);
  auto base_colour = make_nclx(heif_color_primaries_ITU_R_BT_709_5,
      heif_transfer_characteristic_IEC_61966_2_1, heif_matrix_coefficients_ITU_R_BT_709_5, true);
  auto* base = encode_image_with_profile(ctx, encoder, base_colour);
  auto metadata = make_metadata();
  auto* options = heif_tone_map_options_alloc();
  options->alternate_nclx = &base_colour;
  const auto before = heif_context_get_number_of_items(ctx);
  heif_image_handle* output = nullptr;
  REQUIRE(heif_context_add_tone_map_derived_image(ctx, base, gain, &metadata,
                                                options, &output).code == heif_error_Usage_error);
  REQUIRE(output == nullptr);
  REQUIRE(heif_context_get_number_of_items(ctx) == before);
  REQUIRE_FALSE(heif_item_is_item_hidden(ctx, heif_image_handle_get_item_id(gain)));
  // Avoiding hiding permits a primary gain, without silently selecting base.
  options->hide_gain_map = 0;
  REQUIRE(heif_item_set_item_hidden(ctx, heif_image_handle_get_item_id(base), 1).code == heif_error_Ok);
  REQUIRE(heif_context_add_tone_map_derived_image(ctx, base, gain, &metadata,
                                                options, &output).code == heif_error_Usage_error);
  REQUIRE(heif_context_get_number_of_items(ctx) == before);
  REQUIRE(heif_item_set_item_hidden(ctx, heif_image_handle_get_item_id(base), 0).code == heif_error_Ok);
  options->create_altr_group = 0;
  options->alternate_nclx = nullptr;
  const uint8_t opaque_icc[] = {1, 2, 3};
  options->alternate_icc = opaque_icc;
  options->alternate_icc_size = sizeof(opaque_icc);
  options->alternate_icc_type = heif_color_profile_type_nclx;
  REQUIRE(heif_context_add_tone_map_derived_image(ctx, base, gain, &metadata,
                                                options, &output).code == heif_error_Usage_error);
  REQUIRE(heif_context_get_number_of_items(ctx) == before);
  options->alternate_icc_type = heif_color_profile_type_rICC;
  heif_context_get_security_limits(ctx)->max_color_profile_size = 2;
  REQUIRE(heif_context_add_tone_map_derived_image(ctx, base, gain, &metadata,
                                                options, &output).code == heif_error_Usage_error);
  REQUIRE(heif_context_get_number_of_items(ctx) == before);
  heif_context_get_security_limits(ctx)->max_color_profile_size = 0;
  REQUIRE(heif_context_add_tone_map_derived_image(ctx, base, gain, &metadata,
                                                options, &output).code == heif_error_Ok);
  heif_item_id primary = 0;
  REQUIRE(heif_context_get_primary_image_ID(ctx, &primary).code == heif_error_Ok);
  REQUIRE(primary == heif_image_handle_get_item_id(gain));
  // Opaque ICC preservation does not imply unsupported colour transforms work.
  heif_image* pixels = nullptr;
  REQUIRE(heif_decode_image(output, &pixels, heif_colorspace_undefined,
                           heif_chroma_undefined, nullptr).code != heif_error_Ok);
  REQUIRE(pixels == nullptr);
  heif_image_handle_release(output);
  heif_tone_map_options_free(options);
  heif_image_handle_release(base);
  heif_image_handle_release(gain);
  heif_encoder_release(encoder);
  heif_context_free(ctx);
}
