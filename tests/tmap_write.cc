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
#include "test_utils.h"

#include <cstdint>
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
  INFO(error.message ? error.message : "");
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
  INFO(error.message ? error.message : "");
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
  INFO(error.message ? error.message : "");
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
  INFO(error.message ? error.message : "");
  REQUIRE(error.code == heif_error_Ok);
  REQUIRE(tmap != nullptr);

  const heif_item_id tmap_id =
      heif_image_handle_get_item_id(tmap);
  REQUIRE(tmap_id != base_id);
  REQUIRE(tmap_id != gain_id);
  REQUIRE(heif_image_handle_is_tone_map_derived_image(tmap) == 1);

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
