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
#include "libheif/heif.h"
#include "libheif/heif_experimental.h"
#include "libheif/heif_items.h"
#include "gain_map_color.h"

#include <cstdlib>
#include <cmath>
#include <cstring>
#include <fstream>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

TEST_CASE("Apple synthetic ISO producer matches independent HDR intention")
{
  const char* path = std::getenv("LIBHEIF_TMAP_APPLE_FIXTURE");
  if (!path) { return; }
  std::vector<float> reference(64 * 64 * 4);
  std::ifstream reference_file(std::string(path) + ".rgba32f", std::ios::binary);
  reference_file.read(reinterpret_cast<char*>(reference.data()),
                      static_cast<std::streamsize>(reference.size() * sizeof(float)));
  REQUIRE(reference_file.good());
  auto* ctx = heif_context_alloc();
  REQUIRE(heif_context_read_from_file(ctx, path, nullptr).code == heif_error_Ok);
  std::vector<heif_item_id> ids(static_cast<size_t>(heif_context_get_number_of_items(ctx)));
  REQUIRE(heif_context_get_list_of_item_IDs(ctx, ids.data(), static_cast<int>(ids.size())) == static_cast<int>(ids.size()));
  size_t tmaps = 0;
  for (auto id : ids) {
    if (heif_item_get_item_type(ctx, id) != heif_fourcc('t', 'm', 'a', 'p')) { continue; }
    ++tmaps;
    heif_image_handle* handle = nullptr;
    REQUIRE(heif_context_get_image_handle(ctx, id, &handle).code == heif_error_Ok);
    auto* options = heif_decoding_options_alloc();
    options->output_image_nclx_profile_passthrough = true;
    heif_image* pixels = nullptr;
    auto error = heif_decode_image(handle, &pixels, heif_colorspace_RGB, heif_chroma_444, options);
    INFO(error.message);
    REQUIRE(error.code == heif_error_Ok);
    REQUIRE(heif_image_get_width(pixels, heif_channel_R) == 64);
    heif_color_profile_nclx* profile = nullptr;
    REQUIRE(heif_image_handle_get_nclx_color_profile(handle, &profile).code == heif_error_Ok);
    REQUIRE(profile->transfer_characteristics == 16);
    double maximum_error = 0;
    size_t component = 0;
    for (auto channel : {heif_channel_R, heif_channel_G, heif_channel_B}) {
      int stride = 0;
      auto* plane = heif_image_get_plane_readonly(pixels, channel, &stride);
      REQUIRE(heif_image_get_bits_per_pixel_range(pixels, channel) == 16);
      for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
          uint16_t sample = 0;
          std::memcpy(&sample, plane + y * stride + x * 2, sizeof(sample));
          auto linear = gain_map_decode_transfer(sample / 65535.0, 16);
          REQUIRE(linear);
          const double expected = reference[static_cast<size_t>(y * 64 + x) * 4 + component];
          REQUIRE(std::isfinite(expected));
          maximum_error = std::max(maximum_error, std::abs(*linear - expected));
        }
      }
      ++component;
    }
    INFO("Apple/libheif maximum linear pixel error: " << maximum_error);
    REQUIRE(maximum_error < 0.025);
    heif_nclx_color_profile_free(profile);
    heif_image_release(pixels);
    heif_decoding_options_free(options);
    heif_image_handle_release(handle);
  }
  REQUIRE(tmaps == 1);
  heif_context_free(ctx);
}

TEST_CASE("User-provided ISO sample directory interoperability")
{
  const char* directory = std::getenv("LIBHEIF_TMAP_INTEROP_DIR");
  if (!directory) {
    SKIP("Set LIBHEIF_TMAP_INTEROP_DIR to test external HEIC fixtures without redistributing them");
  }
  size_t adaptive_count = 0, ordinary_count = 0;
  const auto traversal_options = std::filesystem::directory_options::skip_permission_denied;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(directory, traversal_options)) {
    if (!entry.is_regular_file() ||
        (entry.path().extension() != ".heic" && entry.path().extension() != ".HEIC" &&
         entry.path().extension() != ".avif")) {
      continue;
    }
    const std::string filename = entry.path().filename().string();
    INFO(filename);
    auto* ctx = heif_context_alloc();
    REQUIRE(ctx);
    auto error = heif_context_read_from_file(ctx, entry.path().string().c_str(), nullptr);
    INFO(error.message);
    REQUIRE(error.code == heif_error_Ok);
    const int count = heif_context_get_number_of_items(ctx);
    std::vector<heif_item_id> ids(static_cast<size_t>(count));
    REQUIRE(heif_context_get_list_of_item_IDs(ctx, ids.data(), count) == count);
    size_t tmaps = 0;
    auto* options = heif_decoding_options_alloc();
    options->output_image_nclx_profile_passthrough = true;
    for (auto id : ids) {
      if (heif_item_get_item_type(ctx, id) != heif_fourcc('t', 'm', 'a', 'p')) {
        continue;
      }
      ++tmaps;
      heif_image_handle* handle = nullptr;
      REQUIRE(heif_context_get_image_handle(ctx, id, &handle).code == heif_error_Ok);
      REQUIRE(heif_image_handle_is_tone_map_derived_image(handle));
      REQUIRE(heif_image_handle_get_gain_map_metadata_status(handle) == heif_gain_map_metadata_status_parsed);
      heif_gain_map_metadata metadata{};
      metadata.struct_version = 1;
      error = heif_image_handle_get_gain_map_metadata(handle, &metadata);
      INFO(error.message);
      REQUIRE(error.code == heif_error_Ok);
      heif_image_handle* base = nullptr;
      heif_image_handle* gain = nullptr;
      REQUIRE(heif_image_handle_get_tone_map_base_image_handle(handle, &base).code == heif_error_Ok);
      REQUIRE(heif_image_handle_get_tone_map_gain_map_image_handle(handle, &gain).code == heif_error_Ok);
      for (auto* child : {base, gain}) {
        heif_image* pixels = nullptr;
        error = heif_decode_image(child, &pixels, heif_colorspace_undefined, heif_chroma_undefined, options);
        INFO(error.message);
        REQUIRE(error.code == heif_error_Ok);
        REQUIRE(pixels);
        heif_image_release(pixels);
      }
      heif_image* pixels = nullptr;
      error = heif_decode_image(handle, &pixels, heif_colorspace_undefined, heif_chroma_undefined, options);
      INFO(error.message);
      REQUIRE(error.code == heif_error_Ok);
      REQUIRE(pixels);
      heif_image_release(pixels);
      std::cout << filename << " tmap=" << id
                << " channels=" << static_cast<int>(metadata.channel_count)
                << " base=" << heif_image_handle_get_item_id(base)
                << " gain=" << heif_image_handle_get_item_id(gain)
                << " reconstruction=success" << '\n';
      heif_image_handle_release(base);
      heif_image_handle_release(gain);
      heif_image_handle_release(handle);
    }
    // Classify by the actual item graph, not camera-specific filenames.
    // ImageIO may expose ISO auxiliary metadata synthesized from a legacy
    // Apple auxl gain map; that is not an ISO tmap container fixture.
    if (tmaps) {
      ++adaptive_count;
    }
    else {
      ++ordinary_count;
      heif_image_handle* primary = nullptr;
      REQUIRE(heif_context_get_primary_image_handle(ctx, &primary).code == heif_error_Ok);
      heif_image* pixels = nullptr;
      error = heif_decode_image(primary, &pixels, heif_colorspace_undefined, heif_chroma_undefined, options);
      INFO(error.message);
      REQUIRE(error.code == heif_error_Ok);
      heif_image_release(pixels);
      heif_image_handle_release(primary);
      std::cout << filename << " primary decode=success (no tmap)\n";
    }
    heif_decoding_options_free(options);
    heif_context_free(ctx);
  }
  REQUIRE(adaptive_count > 0);
  std::cout << "tmap files=" << adaptive_count << " other files=" << ordinary_count << '\n';
}
