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

TEST_CASE("Stock libultrahdr and libheif exchange ISO containers and HDR pixels")
{
  const char* directory = std::getenv("LIBHEIF_TMAP_ULTRAHDR_DIR");
  if (!directory) { return; }
  const double baseline = std::pow((192.0 / 255.0 + 0.055) / 1.055, 2.4);
  for (const char* name : {"libheif-mono-pq.heic", "libheif-rgb-pq.heic",
                          "google-mono.heic", "google-rgb.heic",
                          "google-mono.avif", "google-rgb.avif"}) {
    INFO(name);
    const auto path = std::filesystem::path(directory) / name;
    const bool rgb = std::string(name).find("rgb") != std::string::npos;
    const bool google = std::string(name).find("google") == 0;
    auto* ctx = heif_context_alloc();
    REQUIRE(ctx);
    auto error = heif_context_read_from_file(ctx, path.string().c_str(), nullptr);
    INFO(error.message);
    REQUIRE(error.code == heif_error_Ok);
    std::vector<heif_item_id> ids(static_cast<size_t>(heif_context_get_number_of_items(ctx)));
    REQUIRE(heif_context_get_list_of_item_IDs(ctx, ids.data(), static_cast<int>(ids.size())) == static_cast<int>(ids.size()));
    std::vector<heif_item_id> tmaps;
    for (auto id : ids) {
      if (heif_item_get_item_type(ctx, id) == heif_fourcc('t', 'm', 'a', 'p')) { tmaps.push_back(id); }
    }
    REQUIRE(tmaps.size() == 1);
    heif_image_handle* handle = nullptr;
    REQUIRE(heif_context_get_image_handle(ctx, tmaps.front(), &handle).code == heif_error_Ok);
    heif_gain_map_metadata metadata{};
    metadata.struct_version = 1;
    REQUIRE(heif_image_handle_get_gain_map_metadata(handle, &metadata).code == heif_error_Ok);
    REQUIRE(metadata.channel_count == (rgb ? 3 : 1));
    auto* profile = heif_nclx_color_profile_alloc();
    profile->color_primaries = heif_color_primaries_ITU_R_BT_709_5;
    profile->transfer_characteristics = heif_transfer_characteristic_linear;
    profile->matrix_coefficients = heif_matrix_coefficients_RGB_GBR;
    profile->full_range_flag = 1;
    auto* options = heif_decoding_options_alloc();
    options->output_image_nclx_profile = profile;
    options->output_image_nclx_profile_passthrough = true;
    options->color_conversion_options.only_use_preferred_chroma_algorithm = true;
    options->color_conversion_options.preferred_chroma_upsampling_algorithm = heif_chroma_upsampling_bilinear;
    heif_image* pixels = nullptr;
    error = heif_decode_tone_map_image_float32(handle, &pixels, options, 32,
                                             heif_gain_map_resampling_phase_co_sited);
    INFO(error.message);
    REQUIRE(error.code == heif_error_Ok);
    const int size = google ? 128 : 64;
    REQUIRE(heif_image_get_width(pixels, heif_channel_R) == size);
    REQUIRE(heif_image_get_height(pixels, heif_channel_R) == size);
    std::vector<uint8_t> reference(static_cast<size_t>(size * size) * 8);
    std::ifstream file(path.string() + ".rgba16f", std::ios::binary);
    file.read(reinterpret_cast<char*>(reference.data()), static_cast<std::streamsize>(reference.size()));
    REQUIRE(file.good());
    REQUIRE(file.peek() == std::char_traits<char>::eof());
    double maximum_error = 0;
    for (int c = 0; c < 3; ++c) {
      auto channel = static_cast<heif_channel>(heif_channel_R + c);
      REQUIRE(heif_image_get_bits_per_pixel_range(pixels, channel) == 32);
      int stride = 0;
      const auto* plane = heif_image_get_plane_readonly(pixels, channel, &stride);
      REQUIRE(plane);
      const double gain = !rgb || c == 0 ? 4 : google ? (c == 1 ? 3 : 2) :
                          std::exp2((c == 1 ? 128.0 : 64.0) / 255.0);
      for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
          const size_t offset = (static_cast<size_t>(y * size + x) * 4 + c) * 2;
          const uint16_t half = static_cast<uint16_t>(reference[offset] | (reference[offset + 1] << 8));
          const int exponent = (half >> 10) & 31;
          REQUIRE(exponent != 31);
          double independent = std::ldexp((half & 1023) + (exponent ? 1024 : 0),
                                          exponent ? exponent - 25 : -24);
          if (half & 0x8000) { independent = -independent; }
          float actual = 0;
          std::memcpy(&actual, plane + y * stride + x * 4, sizeof(actual));
          INFO("x=" << x << " y=" << y << " c=" << c);
          // Independently constructed intentions rule out an SDR-only decode.
          REQUIRE(independent == Catch::Approx(baseline * gain).margin(0.01));
          REQUIRE(std::isfinite(actual));
          // Same LUT/half precision allowance as the raw libultrahdr oracle.
          REQUIRE(actual == Catch::Approx(independent).margin(0.0015 + 0.003 * std::abs(independent)));
          maximum_error = std::max(maximum_error, std::abs(actual - independent));
        }
      }
    }
    std::cout << name << " stock libultrahdr/libheif maxLinearError=" << maximum_error << '\n';
    heif_image_release(pixels);
    heif_decoding_options_free(options);
    heif_nclx_color_profile_free(profile);
    heif_image_handle_release(handle);
    heif_context_free(ctx);
  }
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
