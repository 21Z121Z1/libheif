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

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

TEST_CASE("User-provided ISO sample directory interoperability")
{
  const char* directory = std::getenv("LIBHEIF_TMAP_INTEROP_DIR");
  if (!directory) {
    SKIP("Set LIBHEIF_TMAP_INTEROP_DIR to test external HEIC fixtures without redistributing them");
  }
  size_t adaptive_count = 0, pq_count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (entry.path().extension() != ".heic") {
      continue;
    }
    const std::string filename = entry.path().filename().string();
    INFO(filename);
    const bool adaptive = filename.find("AdaptiveHDR") != std::string::npos;
    if (adaptive) { ++adaptive_count; } else { ++pq_count; }
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
      const bool icc_only = heif_image_handle_get_color_profile_type(handle) != heif_color_profile_type_nclx ||
                            heif_image_handle_get_color_profile_type(base) != heif_color_profile_type_nclx;
      if (icc_only) {
        REQUIRE(error.code == heif_error_Unsupported_feature);
        REQUIRE(error.subcode == heif_suberror_Unsupported_color_conversion);
        REQUIRE(pixels == nullptr);
      }
      else {
        INFO(error.message);
        REQUIRE(error.code == heif_error_Ok);
        heif_image_release(pixels);
      }
      std::cout << filename << " tmap=" << id
                << " channels=" << static_cast<int>(metadata.channel_count)
                << " base=" << heif_image_handle_get_item_id(base)
                << " gain=" << heif_image_handle_get_item_id(gain)
                << " reconstruction=" << (icc_only ? "unsupported ICC" : "success") << '\n';
      heif_image_handle_release(base);
      heif_image_handle_release(gain);
      heif_image_handle_release(handle);
    }
    REQUIRE(tmaps == (adaptive ? 1 : 0));
    if (!adaptive) {
      heif_image_handle* primary = nullptr;
      REQUIRE(heif_context_get_primary_image_handle(ctx, &primary).code == heif_error_Ok);
      heif_image* pixels = nullptr;
      error = heif_decode_image(primary, &pixels, heif_colorspace_undefined, heif_chroma_undefined, options);
      INFO(error.message);
      REQUIRE(error.code == heif_error_Ok);
      heif_image_release(pixels);
      heif_image_handle_release(primary);
      std::cout << filename << " ordinary PQ decode=success\n";
    }
    heif_decoding_options_free(options);
    heif_context_free(ctx);
  }
  REQUIRE(adaptive_count > 0);
  REQUIRE(pq_count > 0);
}
