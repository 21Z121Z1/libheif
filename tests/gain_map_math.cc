/*
  libheif ISO 21496-1 gain map metadata tests

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
#include "gain_map_math.h"
#include "gain_map_color.h"
#include "gain_map_reconstruction.h"

#include <cmath>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

TEST_CASE("ISO gain weight includes HDR-to-SDR direction and clamps")
{
  GainMapMetadata m;
  m.base_hdr_headroom = {0, 1};
  m.alternate_hdr_headroom = {2, 1};
  REQUIRE(gain_map_full_weight(m) == 1);
  REQUIRE(*gain_map_target_weight(m, -1) == 0);
  REQUIRE(*gain_map_target_weight(m, 1) == 0.5);
  REQUIRE(*gain_map_target_weight(m, 3) == 1);
  std::swap(m.base_hdr_headroom, m.alternate_hdr_headroom);
  REQUIRE(gain_map_full_weight(m) == -1);
  REQUIRE(*gain_map_target_weight(m, 3) == 0);
  REQUIRE(*gain_map_target_weight(m, 1) == -0.5);
  REQUIRE(*gain_map_target_weight(m, -1) == -1);
  REQUIRE_FALSE(gain_map_target_weight(m, std::numeric_limits<double>::quiet_NaN()));
  m.alternate_hdr_headroom = {4, 2};
  REQUIRE_FALSE(gain_map_target_weight(m, 1));
}

TEST_CASE("ISO inverse gamma, signed gains and offsets have hand-calculated values")
{
  GainMapChannel c;
  c.gain_map_min = {-1, 1};
  c.gain_map_max = {3, 1};
  c.gamma = {2, 1};
  c.base_offset = {1, 4};
  c.alternate_offset = {-1, 8};
  REQUIRE(*gain_map_unnormalize(0.25, c) == 1);
  REQUIRE(*gain_map_apply(0.25, 1, c, 1) == 1.125);
  REQUIRE(*gain_map_apply(0.25, 1, c, -1) == 0.375);
  REQUIRE(*gain_map_unnormalize(-1, c) == -1);
  REQUIRE(*gain_map_unnormalize(2, c) == 3);
  REQUIRE_FALSE(gain_map_apply(1, 2000, c, 1));
  REQUIRE_FALSE(gain_map_apply(std::numeric_limits<double>::infinity(), 0, c, 1));
  c.gain_map_min = {INT32_MIN, UINT32_MAX};
  REQUIRE(std::isfinite(*gain_map_unnormalize(0, c)));
}

TEST_CASE("sRGB and PQ use independent reference values")
{
  REQUIRE(*gain_map_decode_transfer(0.5, 13) == Catch::Approx(0.2140411405).margin(1e-10));
  REQUIRE(*gain_map_encode_transfer(0.2140411405, 13) == Catch::Approx(0.5).margin(1e-9));
  // ST 2084: PQ signal at 100 cd/m2, normalized to HDR reference white 203.
  REQUIRE(*gain_map_decode_transfer(0.5080784215, 16) == Catch::Approx(100.0 / 203).margin(1e-9));
  REQUIRE(*gain_map_encode_transfer(100.0 / 203, 16) == Catch::Approx(0.5080784215).margin(1e-9));
  REQUIRE(*gain_map_decode_transfer(1, 16) == Catch::Approx(10000.0 / 203).margin(1e-9));
  REQUIRE_FALSE(gain_map_decode_transfer(0.5, 18));
  REQUIRE_FALSE(gain_map_decode_transfer(0.5, 2));
}

TEST_CASE("D65 primaries conversions preserve white and match reference red")
{
  auto matrix = gain_map_primaries_matrix(1, 9);
  REQUIRE(matrix);
  const auto red = gain_map_transform(*matrix, {1, 0, 0});
  REQUIRE(red[0] == Catch::Approx(0.627403896).margin(1e-8));
  REQUIRE(red[1] == Catch::Approx(0.069097289).margin(1e-8));
  REQUIRE(red[2] == Catch::Approx(0.016391439).margin(1e-8));
  for (uint16_t source : {1, 9, 12}) {
    for (uint16_t target : {1, 9, 12}) {
      auto transform = gain_map_primaries_matrix(source, target);
      REQUIRE(transform);
      const auto white = gain_map_transform(*transform, {1, 1, 1});
      for (double value : white) {
        REQUIRE(value == Catch::Approx(1).margin(1e-8));
      }
    }
  }
  REQUIRE_FALSE(gain_map_primaries_matrix(2, 9));
}

namespace {
std::shared_ptr<HeifPixelImage> make_pixels(uint32_t width, bool mono, uint16_t sample, uint16_t transfer)
{
  auto image = std::make_shared<HeifPixelImage>();
  image->create(width, 1, mono ? heif_colorspace_monochrome : heif_colorspace_RGB,
                mono ? heif_chroma_monochrome : heif_chroma_444);
  for (auto c : mono ? std::vector<heif_channel>{heif_channel_Y} :
                      std::vector<heif_channel>{heif_channel_R, heif_channel_G, heif_channel_B}) {
    REQUIRE_FALSE(image->add_channel(c, width, 1, 16, nullptr));
    image->fill_channel(c, sample);
  }
  nclx_profile profile;
  profile.set_colour_primaries(mono ? 2 : 1);
  profile.set_transfer_characteristics(transfer);
  profile.set_matrix_coefficients(0);
  profile.set_full_range_flag(true);
  image->set_color_profile_nclx(profile);
  return image;
}

uint16_t sample_at(const HeifPixelImage& image, heif_channel channel, uint32_t x)
{
  return image.get_channel_memory<uint16_t>(channel, nullptr)[x];
}
}  // namespace

TEST_CASE("Reconstruction reconciles mono pixels with RGB metadata and the reverse")
{
  auto base = make_pixels(3, false, 16384, 8);
  auto gain = make_pixels(1, true, 65535, 2);
  auto alternate = base->get_color_profile_nclx();
  GainMapMetadata m;
  m.channel_count = 3;
  for (size_t c = 0; c < 3; ++c) {
    m.channels[c].gain_map_max = {static_cast<int32_t>(c), 1};
  }
  auto options = heif_decoding_options_alloc();
  REQUIRE(options);
  auto result = reconstruct_tone_map(base, gain, m, alternate, *options, nullptr);
  REQUIRE(result);
  REQUIRE(sample_at(**result, heif_channel_R, 0) == 16384);
  REQUIRE(sample_at(**result, heif_channel_G, 1) == 32768);
  REQUIRE(sample_at(**result, heif_channel_B, 2) == 65535);
  m.channel_count = 1;
  m.channels[0].gain_map_max = {1, 1};
  gain = make_pixels(1, false, 65535, 2);
  gain->fill_channel(heif_channel_G, 0);
  result = reconstruct_tone_map(base, gain, m, alternate, *options, nullptr);
  REQUIRE(result);
  REQUIRE(sample_at(**result, heif_channel_R, 0) == 32768);
  REQUIRE(sample_at(**result, heif_channel_G, 0) == 16384);
  heif_decoding_options_free(options);
}

TEST_CASE("Resampling interpolates unnormalized log gain at co-sited phase")
{
  auto base = make_pixels(4, false, 8192, 8);
  auto gain = make_pixels(2, true, 0, 2);
  gain->get_channel_memory<uint16_t>(heif_channel_Y, nullptr)[1] = 65535;
  GainMapMetadata m;
  m.channels[0].gain_map_max = {2, 1};
  m.channels[0].gamma = {2, 1};
  auto options = heif_decoding_options_alloc();
  REQUIRE(options);
  auto result = reconstruct_tone_map(base, gain, m, base->get_color_profile_nclx(), *options, nullptr);
  REQUIRE(result);
  REQUIRE(sample_at(**result, heif_channel_R, 0) == 8192);
  REQUIRE(sample_at(**result, heif_channel_R, 1) == 16384);
  REQUIRE(sample_at(**result, heif_channel_R, 2) == 32768);
  REQUIRE(sample_at(**result, heif_channel_R, 3) == 32768);
  heif_decoding_options_free(options);
}

TEST_CASE("Mono limited-range gain endpoints are normalized then clipped")
{
  auto base = make_pixels(1, false, 16384, 8);
  auto gain = make_pixels(1, true, 16 * 256, 2);
  auto profile = gain->get_color_profile_nclx();
  profile.set_full_range_flag(false);
  gain->set_color_profile_nclx(profile);
  GainMapMetadata m;
  m.channels[0].gain_map_max = {1, 1};
  auto* options = heif_decoding_options_alloc();
  REQUIRE(options);
  auto result = reconstruct_tone_map(base, gain, m, base->get_color_profile_nclx(), *options, nullptr);
  REQUIRE(result);
  REQUIRE(sample_at(**result, heif_channel_R, 0) == Catch::Approx(16384).margin(2));
  gain->fill_channel(heif_channel_Y, 235 * 256);
  result = reconstruct_tone_map(base, gain, m, base->get_color_profile_nclx(), *options, nullptr);
  REQUIRE(result);
  REQUIRE(sample_at(**result, heif_channel_R, 0) == Catch::Approx(32768).margin(2));
  gain->fill_channel(heif_channel_Y, 65535);
  result = reconstruct_tone_map(base, gain, m, base->get_color_profile_nclx(), *options, nullptr);
  REQUIRE(result);
  REQUIRE(sample_at(**result, heif_channel_R, 0) == 32768);
  heif_decoding_options_free(options);
}
