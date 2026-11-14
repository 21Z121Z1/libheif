/*
  libheif GB/T 48347-2026 HEIF production-path integration tests

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

// Exercises the GB/T DRE-AIG integration inside libheif (inspect_dre_aig /
// add_it35_uwa_info) through real HEIF files built with the production writer,
// not just standalone metadata serialization.

#include "catch_amalgamated.hpp"
#include "gbt48347_heif.h"
#include "api_structs.h"
#include "context.h"
#include "box.h"
#include "image-items/image_item.h"
#include "libheif/heif.h"
#include "libheif/heif_experimental.h"
#include "libheif/heif_items.h"
#include "test_utils.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{
heif_error mem_write(heif_context*, const void* data, size_t size, void* userdata)
{
  auto* out = static_cast<std::vector<uint8_t>*>(userdata);
  const auto* b = static_cast<const uint8_t*>(data);
  out->insert(out->end(), b, b + size);
  return {heif_error_Ok, heif_suberror_Unspecified, nullptr};
}

std::vector<uint8_t> write_ctx(heif_context* ctx)
{
  std::vector<uint8_t> encoded;
  heif_writer w{};
  w.writer_api_version = 1;
  w.write = mem_write;
  REQUIRE(heif_context_write(ctx, &w, &encoded).code == heif_error_Ok);
  return encoded;
}

heif_context* reopen(const std::vector<uint8_t>& encoded)
{
  heif_context* ctx = heif_context_alloc();
  REQUIRE(heif_context_read_from_memory_without_copy(ctx, encoded.data(), encoded.size(), nullptr).code == heif_error_Ok);
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

heif_image* rgb_image(int w, int h, uint8_t value, const heif_color_profile_nclx& profile)
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

heif_image* mono_image(int w, int h, uint8_t value, const heif_color_profile_nclx& profile)
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

heif_image_handle* encode(heif_context* ctx, heif_encoder* enc, heif_image* image,
                          const heif_color_profile_nclx& profile)
{
  auto* opts = heif_encoding_options_alloc();
  opts->output_nclx_profile = const_cast<heif_color_profile_nclx*>(&profile);
  heif_image_handle* handle = nullptr;
  REQUIRE(heif_context_encode_image(ctx, image, enc, opts, &handle).code == heif_error_Ok);
  heif_encoding_options_free(opts);
  heif_image_release(image);
  return handle;
}

heif_gain_map_metadata metadata()
{
  heif_gain_map_metadata m{};
  m.struct_version = 1;
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

// Independent sRGB EOTF (IEC 61966-2-1), not the library's colour code.
double srgb_eotf(double codeword)
{
  return codeword <= 0.04045 ? codeword / 12.92
                             : std::pow((codeword + 0.055) / 1.055, 2.4);
}

// Decode a handle to linear BT.709 RGB and return the R channel as a double in
// [0,1] (16-bit output). Used for independent HDR pixel comparison.
double decode_linear_r(heif_image_handle* handle)
{
  auto* dec = heif_decoding_options_alloc();
  dec->output_image_nclx_profile = heif_nclx_color_profile_alloc();
  dec->output_image_nclx_profile->color_primaries = heif_color_primaries_ITU_R_BT_709_5;
  dec->output_image_nclx_profile->transfer_characteristics = heif_transfer_characteristic_linear;
  dec->output_image_nclx_profile->matrix_coefficients = heif_matrix_coefficients_RGB_GBR;
  dec->output_image_nclx_profile->full_range_flag = 1;
  heif_image* image = nullptr;
  REQUIRE(heif_decode_image(handle, &image, heif_colorspace_RGB, heif_chroma_444, dec).code == heif_error_Ok);
  size_t stride = 0;
  const auto* pixels = reinterpret_cast<const uint16_t*>(
      heif_image_get_plane_readonly2(image, heif_channel_R, &stride));
  const double value = pixels[0] / 65535.0;
  heif_image_release(image);
  heif_nclx_color_profile_free(dec->output_image_nclx_profile);
  dec->output_image_nclx_profile = nullptr;
  heif_decoding_options_free(dec);
  return value;
}

// Corrupt the second 'dimg' target of the tmap (in place, no size change) to a
// non-existent item id, to exercise the inspector's malformed-graph rejection.
bool corrupt_dimg_second_target(std::vector<uint8_t>& bytes, heif_item_id tmap_id)
{
  for (size_t i = 0; i + 12 <= bytes.size(); ++i) {
    if (bytes[i] == 'd' && bytes[i + 1] == 'i' && bytes[i + 2] == 'm' && bytes[i + 3] == 'g') {
      const size_t p = i + 4;  // SingleItemTypeReferenceBox payload (no FullBox)
      const uint16_t from = static_cast<uint16_t>((uint16_t(bytes[p]) << 8) | bytes[p + 1]);
      const uint16_t count = static_cast<uint16_t>((uint16_t(bytes[p + 2]) << 8) | bytes[p + 3]);
      if (from == tmap_id && count == 2) {
        bytes[p + 6] = 0xFF;  // second to_item_ID high byte -> 0xFFxx (non-existent)
        bytes[p + 7] = 0xFE;
        return true;
      }
    }
  }
  return false;
}

// Associate an explicit irot property with an item (internal writer API).
void set_item_irot(heif_context* ctx, heif_item_id id, int angle)
{
  auto item = ctx->context->get_image(id, true);
  REQUIRE(item);
  auto irot = std::make_shared<Box_irot>();
  irot->set_rotation_ccw(angle);
  REQUIRE(item->add_property(irot, true) != 0);
}

const heif_color_profile_nclx kBase = nclx(heif_color_primaries_ITU_R_BT_709_5,
    heif_transfer_characteristic_IEC_61966_2_1, heif_matrix_coefficients_RGB_GBR, true);
const heif_color_profile_nclx kGain = nclx(heif_color_primaries_unspecified,
    heif_transfer_characteristic_unspecified, heif_matrix_coefficients_RGB_GBR, true);
const heif_color_profile_nclx kAlt = nclx(heif_color_primaries_ITU_R_BT_709_5,
    heif_transfer_characteristic_ITU_R_BT_2100_0_PQ, heif_matrix_coefficients_RGB_GBR, true);

gbt48347::UWAInfo sample_uwa()
{
  gbt48347::UWAInfo uwa;
  uwa.extended_frame_number = 2;
  uwa.extended_type = gbt48347::kExtendedTypeEnhance;
  uwa.metadata_present = true;
  uwa.static_metadata = {0xDE, 0xAD};
  uwa.dynamic_metadata = {0xBE, 0xEF};
  uwa.extension_present = true;
  uwa.extension.base_present = true;
  uwa.extension.base.flag = 32;
  uwa.extension.base.matrix = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  return uwa;
}

// Build a conformant GB/T dual-layer context. Caller owns the returned context
// and handles. If hide_gain is false the enhancement is left visible (invalid).
struct BuiltFile
{
  heif_context* ctx = nullptr;
  heif_encoder* enc = nullptr;
  heif_image_handle* base = nullptr;
  heif_image_handle* gain = nullptr;
  heif_image_handle* tmap = nullptr;
  heif_item_id it35_id = 0;
};

BuiltFile build_gbt_file(bool hide_gain, bool with_it35, bool valid_it35 = true)
{
  BuiltFile f;
  f.enc = get_encoder_or_skip_test(heif_compression_uncompressed);
  f.ctx = heif_context_alloc();
  f.base = encode(f.ctx, f.enc, rgb_image(4, 4, 127, kBase), kBase);
  f.gain = encode(f.ctx, f.enc, mono_image(4, 4, 255, kGain), kGain);
  if (hide_gain) {
    REQUIRE(heif_item_set_item_hidden(f.ctx, heif_image_handle_get_item_id(f.gain), 1).code == heif_error_Ok);
  }
  auto md = metadata();
  heif_tone_map_options opts{};
  opts.version = 1;
  opts.alternate_nclx = &kAlt;
  REQUIRE(heif_context_add_tone_map_derived_image(f.ctx, f.base, f.gain, &md, &opts, &f.tmap).code == heif_error_Ok);
  if (with_it35) {
    if (valid_it35) {
      REQUIRE(gbt48347::add_it35_uwa_info(*f.ctx->context, sample_uwa(), &f.it35_id) == Error::Ok);
    }
    else {
      // Write a non-conformant it35 payload directly (bypassing the validating
      // writer) so the inspector's rejection path is exercised.
      auto bytes = gbt48347::serialize_uwa_info(sample_uwa());
      REQUIRE(bytes);
      (*bytes)[0] = 0xFF;  // corrupt the T.35 country code
      REQUIRE(heif_context_add_item(f.ctx, "it35", bytes->data(),
                                    static_cast<int>(bytes->size()), &f.it35_id).code == heif_error_Ok);
    }
  }
  return f;
}

void free_built(BuiltFile& f)
{
  if (f.tmap) { heif_image_handle_release(f.tmap); }
  if (f.gain) { heif_image_handle_release(f.gain); }
  if (f.base) { heif_image_handle_release(f.base); }
  if (f.enc) { heif_encoder_release(f.enc); }
  if (f.ctx) { heif_context_free(f.ctx); }
}
}  // namespace


TEST_CASE("GB/T production writer + inspector round-trip a conformant DRE-AIG")
{
  auto f = build_gbt_file(true, true);
  const auto encoded = write_ctx(f.ctx);
  if (const char* dump = std::getenv("LIBHEIF_GBT_DUMP")) {
    if (FILE* fp = std::fopen(dump, "wb")) {
      std::fwrite(encoded.data(), 1, encoded.size(), fp);
      std::fclose(fp);
    }
  }
  auto* read_ctx = reopen(encoded);

  auto aig = gbt48347::inspect_dre_aig(*read_ctx->context);
  REQUIRE(aig);
  CHECK(aig->base_is_primary);
  CHECK(aig->gain_hidden);
  CHECK(aig->has_it35);
  CHECK(aig->base_id == heif_image_handle_get_item_id(f.base));
  CHECK(aig->gain_id == heif_image_handle_get_item_id(f.gain));
  CHECK(aig->tmap_id == heif_image_handle_get_item_id(f.tmap));
  CHECK(aig->it35_id == f.it35_id);
  CHECK(aig->uwa.extended_type == gbt48347::kExtendedTypeEnhance);
  CHECK(aig->uwa.static_metadata == sample_uwa().static_metadata);
  CHECK(aig->uwa.dynamic_metadata == sample_uwa().dynamic_metadata);
  REQUIRE(aig->uwa.extension_present);
  CHECK(aig->uwa.extension.base.matrix[0] == 1.0f);

  // Obtain fresh handles from the RE-READ context (not the write context).
  heif_image_handle* read_tmap = nullptr;
  heif_image_handle* read_base = nullptr;
  REQUIRE(heif_context_get_image_handle(read_ctx, aig->tmap_id, &read_tmap).code == heif_error_Ok);
  REQUIRE(heif_context_get_image_handle(read_ctx, aig->base_id, &read_base).code == heif_error_Ok);

  // Destroy the original write context BEFORE decoding, so the decode below can
  // only rely on the serialized, re-read file. This proves the full closed loop:
  // write -> serialize -> re-read -> parse -> HDR/SDR decode -> independent pixels.
  free_built(f);

  // Independent HDR oracle: base sRGB codeword 127, mono gain normalized 1.0,
  // min=-1/max=1/gamma=1, zero offsets, W=1 -> alternate = 2 * srgb_eotf(127/255).
  const double base_linear = srgb_eotf(127.0 / 255.0);
  const double expected_hdr = 2.0 * base_linear;
  const double hdr = decode_linear_r(read_tmap);
  INFO("base_linear=" << base_linear << " hdr=" << hdr << " expected=" << expected_hdr);
  CHECK(std::abs(hdr - expected_hdr) < 1e-4);

  // SDR base after re-read: native decode returns the original sRGB codeword.
  heif_image* sdr = nullptr;
  REQUIRE(heif_decode_image(read_base, &sdr, heif_colorspace_RGB, heif_chroma_444, nullptr).code == heif_error_Ok);
  int sdr_stride = 0;
  const auto* sdr_plane = heif_image_get_plane_readonly(sdr, heif_channel_R, &sdr_stride);
  CHECK(sdr_plane[0] == 127);
  heif_image_release(sdr);

  heif_image_handle_release(read_tmap);
  heif_image_handle_release(read_base);
  heif_context_free(read_ctx);
}


TEST_CASE("GB/T DRE-AIG without it35 still re-reads and decodes HDR")
{
  auto f = build_gbt_file(true, false);
  const auto encoded = write_ctx(f.ctx);
  auto* read_ctx = reopen(encoded);
  auto aig = gbt48347::inspect_dre_aig(*read_ctx->context);
  REQUIRE(aig);
  CHECK_FALSE(aig->has_it35);
  CHECK(aig->gain_hidden);

  heif_image_handle* read_tmap = nullptr;
  REQUIRE(heif_context_get_image_handle(read_ctx, aig->tmap_id, &read_tmap).code == heif_error_Ok);
  free_built(f);  // decode must rely only on the re-read file
  const double hdr = decode_linear_r(read_tmap);
  CHECK(std::abs(hdr - 2.0 * srgb_eotf(127.0 / 255.0)) < 1e-4);
  heif_image_handle_release(read_tmap);
  heif_context_free(read_ctx);
}


TEST_CASE("GB/T inspector rejects a tmap with a malformed dimg target")
{
  auto f = build_gbt_file(true, true);
  auto encoded = write_ctx(f.ctx);
  const auto tmap_id = heif_image_handle_get_item_id(f.tmap);
  REQUIRE(corrupt_dimg_second_target(encoded, tmap_id));
  auto* read_ctx = reopen(encoded);
  auto aig = gbt48347::inspect_dre_aig(*read_ctx->context);
  CHECK_FALSE(aig);  // second dimg target no longer exists
  heif_context_free(read_ctx);
  free_built(f);
}


TEST_CASE("GB/T inspector rejects a visible enhancement item")
{
  auto f = build_gbt_file(false, true);  // gain not hidden
  const auto encoded = write_ctx(f.ctx);
  auto* read_ctx = reopen(encoded);
  auto aig = gbt48347::inspect_dre_aig(*read_ctx->context);
  CHECK_FALSE(aig);
  heif_context_free(read_ctx);
  free_built(f);
}


TEST_CASE("GB/T inspector rejects a non-conformant it35 UWAInfo")
{
  auto f = build_gbt_file(true, true, false);  // bad T.35 country code
  const auto encoded = write_ctx(f.ctx);
  auto* read_ctx = reopen(encoded);
  auto aig = gbt48347::inspect_dre_aig(*read_ctx->context);
  CHECK_FALSE(aig);
  heif_context_free(read_ctx);
  free_built(f);
}


TEST_CASE("GB/T inspector rejects a file without a tmap")
{
  auto* enc = get_encoder_or_skip_test(heif_compression_uncompressed);
  auto* ctx = heif_context_alloc();
  auto* base = encode(ctx, enc, rgb_image(4, 4, 127, kBase), kBase);
  const auto encoded = write_ctx(ctx);
  auto* read_ctx = reopen(encoded);
  auto aig = gbt48347::inspect_dre_aig(*read_ctx->context);
  CHECK_FALSE(aig);
  heif_context_free(read_ctx);
  heif_image_handle_release(base);
  heif_encoder_release(enc);
  heif_context_free(ctx);
}


TEST_CASE("GB/T it35 writer requires an existing tmap")
{
  auto* enc = get_encoder_or_skip_test(heif_compression_uncompressed);
  auto* ctx = heif_context_alloc();
  auto* base = encode(ctx, enc, rgb_image(4, 4, 127, kBase), kBase);
  heif_item_id id = 0;
  Error err = gbt48347::add_it35_uwa_info(*ctx->context, sample_uwa(), &id);
  CHECK(err);  // no tmap present -> error
  heif_image_handle_release(base);
  heif_encoder_release(enc);
  heif_context_free(ctx);
}

// --- Round-3 irot consistency (GB/T 9.3.2d "irot默认为0", 9.3.3c/9.3.4c) ---

TEST_CASE("GB/T irot: missing properties default to 0 and are consistent")
{
  // No irot on any item: GB/T 9.3.2d defaults irot to 0, so all match.
  auto f = build_gbt_file(true, true);
  const auto encoded = write_ctx(f.ctx);
  auto* read_ctx = reopen(encoded);
  auto aig = gbt48347::inspect_dre_aig(*read_ctx->context);
  REQUIRE(aig);  // missing irot == 0 on base/gain/tmap is conformant
  heif_context_free(read_ctx);
  free_built(f);
}

TEST_CASE("GB/T irot: explicit matching rotation on base/gain/tmap is accepted")
{
  auto f = build_gbt_file(true, true);
  set_item_irot(f.ctx, heif_image_handle_get_item_id(f.base), 90);
  set_item_irot(f.ctx, heif_image_handle_get_item_id(f.gain), 90);
  set_item_irot(f.ctx, heif_image_handle_get_item_id(f.tmap), 90);
  const auto encoded = write_ctx(f.ctx);
  auto* read_ctx = reopen(encoded);
  auto aig = gbt48347::inspect_dre_aig(*read_ctx->context);
  REQUIRE(aig);  // all three explicitly 90 -> consistent
  heif_context_free(read_ctx);
  free_built(f);
}

TEST_CASE("GB/T irot: explicit zero is equivalent to missing (default 0)")
{
  auto f = build_gbt_file(true, true);
  set_item_irot(f.ctx, heif_image_handle_get_item_id(f.base), 0);
  // gain/tmap have no irot (default 0); base explicit 0 must still match.
  const auto encoded = write_ctx(f.ctx);
  auto* read_ctx = reopen(encoded);
  auto aig = gbt48347::inspect_dre_aig(*read_ctx->context);
  REQUIRE(aig);
  heif_context_free(read_ctx);
  free_built(f);
}

TEST_CASE("GB/T irot: mismatched rotation between base and gain is rejected")
{
  auto f = build_gbt_file(true, true);
  set_item_irot(f.ctx, heif_image_handle_get_item_id(f.base), 90);
  // gain/tmap remain default 0 -> base 90 != gain 0 -> non-conformant.
  const auto encoded = write_ctx(f.ctx);
  auto* read_ctx = reopen(encoded);
  auto aig = gbt48347::inspect_dre_aig(*read_ctx->context);
  CHECK_FALSE(aig);
  heif_context_free(read_ctx);
  free_built(f);
}
