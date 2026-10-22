/*
  libheif unit tests

  MIT License

  Copyright (c) 2026 Dirk Farin <dirk.farin@gmail.com>

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

// Encoded image handles initialize their decoder configuration and input extent
// before being returned. Queries must work before serialization, and decoding
// must succeed when a decoder plugin is available or fail without a null dereference.

#include "catch_amalgamated.hpp"
#include "libheif/heif.h"

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>

namespace {

constexpr int W = 64;
constexpr int H = 64;

heif_image* create_rgb_image()
{
  heif_image* img = nullptr;
  heif_error err = heif_image_create(W, H, heif_colorspace_RGB, heif_chroma_444, &img);
  REQUIRE(err.code == heif_error_Ok);

  for (heif_channel channel : {heif_channel_R, heif_channel_G, heif_channel_B}) {
    err = heif_image_add_plane(img, channel, W, H, 8);
    REQUIRE(err.code == heif_error_Ok);

    size_t stride = 0;
    uint8_t* p = heif_image_get_plane2(img, channel, &stride);
    REQUIRE(p != nullptr);
    for (int y = 0; y < H; y++) {
      memset(p + y * stride, 0x40, W);
    }
  }

  return img;
}

const char* format_name(heif_compression_format format)
{
  switch (format) {
    case heif_compression_HEVC: return "HEVC";
    case heif_compression_AVC: return "AVC";
    case heif_compression_JPEG: return "JPEG";
    case heif_compression_AV1: return "AV1";
    case heif_compression_VVC: return "VVC";
    case heif_compression_JPEG2000: return "JPEG 2000";
    case heif_compression_uncompressed: return "uncompressed";
    case heif_compression_HTJ2K: return "HT-J2K";
    default: return "?";
  }
}

} // namespace


TEST_CASE("queries and decoding on encoded image handles work before serialization")
{
  int num_formats_tested = 0;

  for (heif_compression_format format : {heif_compression_HEVC, heif_compression_AVC, heif_compression_JPEG,
                                         heif_compression_AV1, heif_compression_VVC, heif_compression_JPEG2000,
                                         heif_compression_uncompressed, heif_compression_HTJ2K}) {
    if (!heif_have_encoder_for_format(format)) {
      continue;
    }

    INFO("format: " << format_name(format));

    heif_image* img = create_rgb_image();
    heif_context* ctx = heif_context_alloc();

    heif_encoder* encoder = nullptr;
    heif_error err = heif_context_get_encoder_for_format(ctx, format, &encoder);
    REQUIRE(err.code == heif_error_Ok);

    heif_image_handle* handle = nullptr;
    err = heif_context_encode_image(ctx, img, encoder, nullptr, &handle);
    if (err.code != heif_error_Ok) {
      // an encoder that does not take this kind of image is not what is tested here
      heif_encoder_release(encoder);
      heif_context_free(ctx);
      heif_image_release(img);
      continue;
    }
    REQUIRE(handle != nullptr);
    num_formats_tested++;

    // --- these work on a handle of an image that is being written

    CHECK(heif_image_handle_get_width(handle) == W);
    CHECK(heif_image_handle_get_height(handle) == H);

    // --- these use the initialized decoder configuration

    CHECK(heif_image_handle_get_luma_bits_per_pixel(handle) == 8);
    CHECK(heif_image_handle_get_chroma_bits_per_pixel(handle) == 8);

    heif_colorspace colorspace;
    heif_chroma chroma;
    err = heif_image_handle_get_preferred_decoding_colorspace(handle, &colorspace, &chroma);
    CHECK(err.code == heif_error_Ok);

    CHECK(heif_image_handle_has_alpha_channel(handle) == 0);

    const bool can_decode = heif_have_decoder_for_format(format) || format == heif_compression_uncompressed;
    heif_image* decoded = nullptr;
    err = heif_decode_image(handle, &decoded, heif_colorspace_undefined, heif_chroma_undefined, nullptr);
    INFO(err.message);
    CHECK((err.code == heif_error_Ok) == can_decode);
    CHECK((err.code == heif_error_Ok) == (decoded != nullptr));
    if (decoded) {
      CHECK(heif_image_get_primary_width(decoded) == W);
      CHECK(heif_image_get_primary_height(decoded) == H);
      heif_image_release(decoded);
    }

    decoded = nullptr;
    err = heif_image_handle_decode_image_tile(handle, &decoded, heif_colorspace_undefined, heif_chroma_undefined,
                                              nullptr, 0, 0);
    CHECK((err.code == heif_error_Ok) == can_decode);
    CHECK((err.code == heif_error_Ok) == (decoded != nullptr));
    if (decoded) {
      CHECK(heif_image_get_primary_width(decoded) == W);
      CHECK(heif_image_get_primary_height(decoded) == H);
      heif_image_release(decoded);
    }

    // --- the same through the primary image handle of the context

    heif_image_handle* primary = nullptr;
    err = heif_context_get_primary_image_handle(ctx, &primary);
    REQUIRE(err.code == heif_error_Ok);
    CHECK(heif_image_handle_get_luma_bits_per_pixel(primary) == 8);
    CHECK(heif_image_handle_has_alpha_channel(primary) == 0);
    heif_image_handle_release(primary);

    heif_image_handle_release(handle);
    heif_encoder_release(encoder);
    heif_context_free(ctx);
    heif_image_release(img);
  }

  if (num_formats_tested == 0) {
    SKIP("no encoder available");
  }
}
