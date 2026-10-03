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
#include <cstring>
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

TEST_CASE("Target headroom clamps correctly when rational endpoints round identically")
{
  const uint32_t n = UINT32_MAX;
  GainMapMetadata metadata;
  metadata.base_hdr_headroom = {n - 2, n - 1};
  metadata.alternate_hdr_headroom = {n - 1, n};
  // Both endpoints are below the exactly representable 1 - 2^-32, but round
  // to it as doubles. Subtracting a rounded baseline would incorrectly give 0.
  REQUIRE(*gain_map_target_weight(metadata, 1.0 - std::ldexp(1.0, -32)) == 1);
  REQUIRE(*gain_map_target_weight(metadata, std::numeric_limits<double>::max()) == 1);
  metadata.base_hdr_headroom = {n - 1, n - 2};
  metadata.alternate_hdr_headroom = {n, n - 1};
  // Reverse direction: both endpoints exceed 1 + 2^-32, so the full weight is -1.
  REQUIRE(*gain_map_target_weight(metadata, 1.0 + std::ldexp(1.0, -32)) == -1);
  REQUIRE(*gain_map_target_weight(metadata, std::numeric_limits<double>::max()) == 0);
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

TEST_CASE("Linear transfer retains signed floating values")
{
  for (double value : {-4.0, -0.125, 0.0, 0.5, 8.0}) {
    REQUIRE(*gain_map_decode_transfer(value, 8) == value);
    REQUIRE(*gain_map_encode_transfer(value, 8) == value);
  }
  for (double value : {std::numeric_limits<double>::infinity(),
                       std::numeric_limits<double>::quiet_NaN()}) {
    REQUIRE_FALSE(gain_map_decode_transfer(value, 8));
    REQUIRE_FALSE(gain_map_encode_transfer(value, 8));
  }
}

TEST_CASE("BT2020 SDR code points use continuous H273 transfer curves")
{
  const uint16_t transfer = GENERATE(uint16_t{1}, uint16_t{6}, uint16_t{14}, uint16_t{15});
  REQUIRE(gain_map_supports_transfer(transfer));
  // H.273 8.2 continuity constants, independently evaluated with Decimal.
  REQUIRE(*gain_map_decode_transfer(0.5, transfer) == Catch::Approx(0.25971943710117881).margin(1e-14));
  REQUIRE(*gain_map_decode_transfer(0.081, transfer) == Catch::Approx(0.018).margin(1e-14));
  REQUIRE(*gain_map_encode_transfer(0.018, transfer) == Catch::Approx(0.081).margin(1e-14));
  REQUIRE(*gain_map_encode_transfer(0.25971943710117881, transfer) == Catch::Approx(0.5).margin(1e-14));
  constexpr double beta = 0.018053968510807807;
  REQUIRE(*gain_map_decode_transfer(4.5 * beta, transfer) == Catch::Approx(beta).margin(1e-14));
  REQUIRE(*gain_map_encode_transfer(beta, transfer) == Catch::Approx(4.5 * beta).margin(1e-14));
}

TEST_CASE("Remaining H273 transfer curves match independent reference values")
{
  for (uint16_t transfer : std::array<uint16_t, 9>{4, 5, 7, 9, 10, 11, 12, 13, 17}) {
    REQUIRE(gain_map_supports_transfer(transfer));
  }
  REQUIRE(*gain_map_decode_transfer(0.5, 4) == Catch::Approx(0.21763764082403103).margin(1e-14));
  REQUIRE(*gain_map_decode_transfer(0.5, 5) == Catch::Approx(0.14358729437462938).margin(1e-14));
  REQUIRE(*gain_map_decode_transfer(0.5, 7) == Catch::Approx(0.26506701270008923).margin(1e-14));
  REQUIRE(*gain_map_decode_transfer(0.5, 9) == Catch::Approx(0.1).margin(1e-14));
  REQUIRE(*gain_map_decode_transfer(0.6, 10) == Catch::Approx(0.1).margin(1e-14));
  REQUIRE(*gain_map_decode_transfer(-0.5, 11) == Catch::Approx(-0.25971943710117881).margin(1e-14));
  REQUIRE(*gain_map_decode_transfer(-0.2, 12) == Catch::Approx(-0.16000581150475236).margin(1e-14));
  REQUIRE(*gain_map_decode_transfer(0.96704267531793354, 17) == Catch::Approx(48.0 / 203).margin(1e-14));
  REQUIRE(*gain_map_decode_transfer(1, 17) == Catch::Approx(52.37 / 203).margin(1e-14));
  REQUIRE(*gain_map_encode_transfer(0.1, 9) == Catch::Approx(0.5).margin(1e-14));
  REQUIRE(*gain_map_encode_transfer(0.1, 10) == Catch::Approx(0.6).margin(1e-14));
  // The logarithmic zero signal represents a flat interval; choose black.
  REQUIRE(*gain_map_decode_transfer(0, 9) == 0);
  REQUIRE(*gain_map_decode_transfer(0, 10) == 0);
  REQUIRE(*gain_map_encode_transfer(0.001, 9) == 0);
  REQUIRE(*gain_map_encode_transfer(0.001, 10) == 0);
  for (uint16_t transfer : std::array<uint16_t, 7>{4, 5, 7, 11, 12, 13, 17}) {
    for (double signal : {0.0, 0.01, 0.08, 0.5, 0.9, 1.0}) {
      auto linear = gain_map_decode_transfer(signal, transfer);
      REQUIRE(linear);
      auto encoded = gain_map_encode_transfer(*linear, transfer);
      REQUIRE(encoded);
      REQUIRE(*encoded == Catch::Approx(signal).margin(1e-13));
    }
  }
  for (uint16_t transfer : std::array<uint16_t, 2>{11, 12}) {
    for (double signal : {-0.001, -0.02, -0.2}) {
      REQUIRE(*gain_map_encode_transfer(*gain_map_decode_transfer(signal, transfer), transfer) ==
              Catch::Approx(signal).margin(1e-13));
    }
  }
}

TEST_CASE("CICP primaries conversions preserve adapted white and match reference red")
{
  auto matrix = gain_map_primaries_matrix(1, 9);
  REQUIRE(matrix);
  const auto red = gain_map_transform(*matrix, {1, 0, 0});
  REQUIRE(red[0] == Catch::Approx(0.627403896).margin(1e-8));
  REQUIRE(red[1] == Catch::Approx(0.069097289).margin(1e-8));
  REQUIRE(red[2] == Catch::Approx(0.016391439).margin(1e-8));
  constexpr std::array<uint16_t, 11> primaries{1, 4, 5, 6, 7, 8, 9, 10, 11, 12, 22};
  for (uint16_t source : primaries) {
    for (uint16_t target : primaries) {
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

TEST_CASE("HLG reference EOTF includes the luminance-dependent OOTF")
{
  nclx_profile profile;
  profile.set_colour_primaries(9);
  profile.set_transfer_characteristics(18);
  profile.set_matrix_coefficients(0);
  profile.set_full_range_flag(true);
  auto white = gain_map_decode_rgb({1, 1, 1}, profile);
  REQUIRE(white);
  REQUIRE((*white)[0] == Catch::Approx(1000.0 / 203.0).margin(1e-8));
  auto reference_white = gain_map_decode_rgb({0.75, 0.75, 0.75}, profile);
  REQUIRE(reference_white);
  REQUIRE((*reference_white)[0] == Catch::Approx(203.152146 / 203.0).margin(1e-8));
  auto red = gain_map_decode_rgb({0.5, 0, 0}, profile);
  REQUIRE(red);
  // At signal 0.5, scene R=1/12 and scene luminance=0.262700212/12.
  const double expected = (1000.0 / 203.0) / 12.0 * std::pow(0.262700212 / 12.0, 0.2);
  REQUIRE((*red)[0] == Catch::Approx(expected).margin(1e-10));
  REQUIRE((*red)[1] == 0);
  REQUIRE((*red)[2] == 0);
  for (const GainMapRGB input : {GainMapRGB{0, 0, 0}, GainMapRGB{0.2, 0.7, 0.9},
                                GainMapRGB{1, 1, 1}}) {
    auto decoded = gain_map_decode_rgb(input, profile);
    REQUIRE(decoded);
    auto encoded = gain_map_encode_rgb(*decoded, profile);
    REQUIRE(encoded);
    for (size_t c = 0; c < 3; ++c) {
      REQUIRE((*encoded)[c] == Catch::Approx(input[c]).margin(1e-9));
    }
  }
  REQUIRE_FALSE(gain_map_decode_rgb({std::numeric_limits<double>::infinity(), 0, 0}, profile));
  REQUIRE_FALSE(gain_map_encode_rgb({std::numeric_limits<double>::quiet_NaN(), 0, 0}, profile));
  profile.set_colour_primaries(2);
  REQUIRE_FALSE(gain_map_decode_rgb({1, 1, 1}, profile));
}

namespace {
std::shared_ptr<HeifPixelImage> make_pixels(uint32_t width, bool mono, uint16_t sample,
                                            uint16_t transfer, uint8_t bit_depth = 16)
{
  auto image = std::make_shared<HeifPixelImage>();
  image->create(width, 1, mono ? heif_colorspace_monochrome : heif_colorspace_RGB,
                mono ? heif_chroma_monochrome : heif_chroma_444);
  for (auto c : mono ? std::vector<heif_channel>{heif_channel_Y} :
                      std::vector<heif_channel>{heif_channel_R, heif_channel_G, heif_channel_B}) {
    REQUIRE_FALSE(image->add_channel(c, width, 1, bit_depth, nullptr));
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

TEST_CASE("Tone maps interpret defined linear raster matrices for both inputs")
{
  const uint16_t matrix = GENERATE(uint16_t{1}, uint16_t{4}, uint16_t{5}, uint16_t{6}, uint16_t{7}, uint16_t{9});
  const bool limited = GENERATE(false, true);
  const bool colour_is_gain = GENERATE(false, true);
  auto colour = std::make_shared<HeifPixelImage>();
  colour->create(1, 1, heif_colorspace_YCbCr, heif_chroma_444);
  for (auto channel : {heif_channel_Y, heif_channel_Cb, heif_channel_Cr}) {
    REQUIRE_FALSE(colour->add_channel(channel, 1, 1, 16, nullptr));
  }
  colour->fill_channel(heif_channel_Y, 32768);
  colour->fill_channel(heif_channel_Cb, 40000);
  colour->fill_channel(heif_channel_Cr, 26000);
  nclx_profile profile;
  profile.set_colour_primaries(colour_is_gain ? 2 : 1);
  profile.set_transfer_characteristics(colour_is_gain ? 2 : 8);
  profile.set_matrix_coefficients(matrix);
  profile.set_full_range_flag(!limited);
  colour->set_color_profile_nclx(profile);
  auto base = colour_is_gain ? make_pixels(1, false, 16384, 8) : colour;
  auto gain = colour_is_gain ? colour : make_pixels(1, true, 0, 2);
  auto alternate = make_pixels(1, false, 0, 8)->get_color_profile_nclx();
  GainMapMetadata metadata;
  metadata.channel_count = 3;
  if (colour_is_gain) {
    for (auto& channel : metadata.channels) { channel.gain_map_max = {1, 1}; }
  }
  const double y = limited ? (32768.0 - 4096) / 56064 : 32768.0 / 65535;
  const double cb = (40000.0 - 32768) / (limited ? 57344 : 65535);
  const double cr = (26000.0 - 32768) / (limited ? 57344 : 65535);
  const double kr = matrix == 1 ? 0.2126 : matrix == 4 ? 0.30 : matrix == 7 ? 0.212 : matrix == 9 ? 0.2627 : 0.299;
  const double kb = matrix == 1 ? 0.0722 : matrix == 4 ? 0.11 : matrix == 7 ? 0.087 : matrix == 9 ? 0.0593 : 0.114;
  const GainMapRGB signal{y + 2 * (1 - kr) * cr,
                           y - 2 * kb * (1 - kb) / (1 - kr - kb) * cb -
                               2 * kr * (1 - kr) / (1 - kr - kb) * cr,
                           y + 2 * (1 - kb) * cb};
  auto* options = heif_decoding_options_alloc();
  REQUIRE(options);
  auto result = reconstruct_tone_map(base, gain, metadata, alternate, *options, nullptr);
  REQUIRE(result);
  const std::array<heif_channel, 3> channels{heif_channel_R, heif_channel_G, heif_channel_B};
  for (size_t c = 0; c < 3; ++c) {
    const double normalized = std::clamp(signal[c], 0.0, 1.0);
    const double expected = colour_is_gain ? 16384 * std::exp2(normalized) : 65535 * normalized;
    REQUIRE(sample_at(**result, channels[c], 0) == Catch::Approx(std::round(expected)).margin(3));
  }
  profile.set_matrix_coefficients(2); // An unspecified matrix must not use a guessed default.
  colour->set_color_profile_nclx(profile);
  result = reconstruct_tone_map(base, gain, metadata, alternate, *options, nullptr);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().error_code == heif_error_Unsupported_feature);
  heif_decoding_options_free(options);
}

TEST_CASE("Tone maps decode H273 YCgCo codewords at their actual RGB depth")
{
  struct Codeword {
    uint16_t matrix;
    int y_bits, c_bits;
    std::array<uint32_t, 3> encoded;
    std::array<uint32_t, 3> rgb;
  };
  // Independently calculated H.273 (2024) Eq.51-65 codewords. The odd
  // negative differences exercise floor division in the reversible lifting.
  const auto code = GENERATE(
      Codeword{8, 8, 8, {116, 115, 31}, {32, 103, 226}},
      Codeword{8, 8, 9, {115, 231, 63}, {32, 103, 225}},
      Codeword{16, 10, 10, {115, 487, 319}, {32, 103, 225}},
      Codeword{17, 9, 9, {115, 231, 63}, {32, 103, 225}},
      Codeword{16, 16, 16, {7250, 30268, 45768}, {15000, 6000, 2000}},
      Codeword{17, 16, 16, {11000, 22768, 60768}, {30000, 6000, 2000}},
      Codeword{0, 32, 32, {1073741825, 2147483649, 3221225473}, {3221225473, 1073741825, 2147483649}},
      Codeword{16, 32, 32, {469762051, 1744830464, 2415919108}, {805306373, 268435459, 536870913}},
      Codeword{17, 32, 32, {939524099, 1342177279, 2684354554}, {1610612737, 536870915, 1073741831}},
      Codeword{16, 32, 32, {0, 2147483648, 3221225472}, {1073741823, 0, 0}});
  const bool limited = GENERATE(false, true);
  const bool colour_is_gain = GENERATE(false, true);
  const auto chroma = GENERATE(heif_chroma_444, heif_chroma_422, heif_chroma_420);
  const bool bilinear = GENERATE(false, true);
  INFO(code.matrix << "/" << code.y_bits << "/" << code.c_bits);
  const int rgb_bits = code.y_bits - (code.matrix == 16 ? 2 : code.matrix == 17 ? 1 : 0);
  auto colour = std::make_shared<HeifPixelImage>();
  colour->create(3, 3, heif_colorspace_YCbCr, chroma);
  const std::array<heif_channel, 3> channels{heif_channel_Y, heif_channel_Cb, heif_channel_Cr};
  for (size_t c = 0; c < channels.size(); ++c) {
    const uint32_t w = c == 0 || chroma == heif_chroma_444 ? 3 : 2;
    const uint32_t h = c == 0 || chroma != heif_chroma_420 ? 3 : 2;
    REQUIRE_FALSE(colour->add_channel(channels[c], w, h, c == 0 ? code.y_bits : code.c_bits, nullptr));
    if (code.y_bits <= 16 && code.c_bits <= 16) {
      colour->fill_channel(channels[c], static_cast<uint16_t>(code.encoded[c]));
    }
    else {
      size_t stride = 0;
      auto* plane = colour->get_channel_memory(channels[c], &stride);
      for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
          reinterpret_cast<uint32_t*>(plane + size_t(y) * stride)[x] = code.encoded[c];
        }
      }
    }
  }
  nclx_profile profile;
  profile.set_colour_primaries(colour_is_gain ? 2 : 1);
  profile.set_transfer_characteristics(colour_is_gain ? 2 : 8);
  profile.set_matrix_coefficients(code.matrix);
  profile.set_full_range_flag(!limited);
  colour->set_color_profile_nclx(profile);
  auto base = colour_is_gain ? make_pixels(3, false, 16384, 8) : colour;
  auto gain = colour_is_gain ? colour : make_pixels(1, true, 0, 2);
  auto alternate = make_pixels(1, false, 0, 8)->get_color_profile_nclx();
  GainMapMetadata metadata;
  metadata.channel_count = 3;
  if (colour_is_gain) {
    for (auto& channel : metadata.channels) { channel.gain_map_max = {1, 1}; }
  }
  auto* options = heif_decoding_options_alloc();
  REQUIRE(options);
  if (bilinear) {
    options->color_conversion_options.preferred_chroma_upsampling_algorithm = heif_chroma_upsampling_bilinear;
    options->color_conversion_options.only_use_preferred_chroma_algorithm = true;
  }
  auto result = reconstruct_tone_map(base, gain, metadata, alternate, *options, nullptr);
  REQUIRE(result);
  const double offset = limited ? std::ldexp(16.0, rgb_bits - 8) : 0;
  const double scale = limited ? std::ldexp(219.0, rgb_bits - 8) : std::ldexp(1.0, rgb_bits) - 1;
  const std::array<heif_channel, 3> rgb_channels{heif_channel_R, heif_channel_G, heif_channel_B};
  for (size_t c = 0; c < rgb_channels.size(); ++c) {
    const double normalized = std::clamp((code.rgb[c] - offset) / scale, 0.0, 1.0);
    const double expected = colour_is_gain ? 16384 * std::exp2(normalized) : 65535 * normalized;
    size_t stride = 0;
    const auto* plane = (*result)->get_channel_memory<uint16_t>(rgb_channels[c], &stride);
    for (uint32_t y = 0; y < (*result)->get_height(); ++y) {
      for (uint32_t x = 0; x < 3; ++x) {
        REQUIRE(plane[size_t(y) * stride / sizeof(uint16_t) + x] ==
                Catch::Approx(std::round(expected)).margin(1));
      }
    }
    // Source depth and encoded codewords are unchanged.
    REQUIRE(colour->get_bits_per_pixel(channels[c]) == (c == 0 ? code.y_bits : code.c_bits));
  }
  heif_decoding_options_free(options);
}

TEST_CASE("Tone maps invert ST2085 with separate luma and chroma normalization")
{
  const int y_bits = GENERATE(8, 10, 16);
  const int c_bits = GENERATE(8, 12, 16);
  const bool limited = GENERATE(false, true);
  const bool colour_is_gain = GENERATE(false, true);
  auto colour = std::make_shared<HeifPixelImage>();
  colour->create(1, 1, heif_colorspace_YCbCr, heif_chroma_444);
  const std::array<heif_channel, 3> channels{heif_channel_Y, heif_channel_Cb, heif_channel_Cr};
  const std::array<uint16_t, 3> values{
      static_cast<uint16_t>(1U << (y_bits - 1)),
      static_cast<uint16_t>((1U << (c_bits - 1)) + (1U << (c_bits - 3))),
      static_cast<uint16_t>((1U << (c_bits - 1)) - (1U << (c_bits - 4)))};
  for (size_t c = 0; c < channels.size(); ++c) {
    REQUIRE_FALSE(colour->add_channel(channels[c], 1, 1, c == 0 ? y_bits : c_bits, nullptr));
    colour->fill_channel(channels[c], values[c]);
  }
  nclx_profile profile;
  profile.set_colour_primaries(colour_is_gain ? 2 : 1);
  profile.set_transfer_characteristics(colour_is_gain ? 2 : 8);
  profile.set_matrix_coefficients(11);
  profile.set_full_range_flag(!limited);
  colour->set_color_profile_nclx(profile);
  auto base = colour_is_gain ? make_pixels(1, false, 16384, 8) : colour;
  auto gain = colour_is_gain ? colour : make_pixels(1, true, 0, 2);
  auto alternate = make_pixels(1, false, 0, 8)->get_color_profile_nclx();
  GainMapMetadata metadata;
  metadata.channel_count = 3;
  if (colour_is_gain) {
    for (auto& channel : metadata.channels) { channel.gain_map_max = {1, 1}; }
  }
  const double y = limited ? (values[0] - 16.0 * (1U << (y_bits - 8))) /
                                (219.0 * (1U << (y_bits - 8))) :
                            static_cast<double>(values[0]) / ((1U << y_bits) - 1);
  const double cb = (values[1] - static_cast<double>(1U << (c_bits - 1))) /
                    (limited ? 224.0 * (1U << (c_bits - 8)) : (1U << c_bits) - 1);
  const double cr = (values[2] - static_cast<double>(1U << (c_bits - 1))) /
                    (limited ? 224.0 * (1U << (c_bits - 8)) : (1U << c_bits) - 1);
  // H.273 Eq.76-78, solved independently for R/G/B.
  const GainMapRGB signal{2 * cr + 0.991902 * y, y, (2 * cb + y) / 0.986566};
  auto* options = heif_decoding_options_alloc();
  REQUIRE(options);
  auto result = reconstruct_tone_map(base, gain, metadata, alternate, *options, nullptr);
  REQUIRE(result);
  const std::array<heif_channel, 3> rgb_channels{heif_channel_R, heif_channel_G, heif_channel_B};
  for (size_t c = 0; c < rgb_channels.size(); ++c) {
    const double normalized = std::clamp(signal[c], 0.0, 1.0);
    const double expected = colour_is_gain ? 16384 * std::exp2(normalized) : 65535 * normalized;
    REQUIRE(sample_at(**result, rgb_channels[c], 0) == Catch::Approx(std::round(expected)).margin(1));
  }
  heif_decoding_options_free(options);
}

TEST_CASE("Reversible YCgCo clips B before deriving R and preserves alpha")
{
  const bool premultiplied = GENERATE(false, true);
  auto base = std::make_shared<HeifPixelImage>();
  base->create(1, 1, heif_colorspace_YCbCr, heif_chroma_444);
  for (auto channel : {heif_channel_Y, heif_channel_Cb, heif_channel_Cr}) {
    REQUIRE_FALSE(base->add_channel(channel, 1, 1, 9, nullptr));
  }
  base->fill_channel(heif_channel_Y, 0);
  base->fill_channel(heif_channel_Cb, 256);
  base->fill_channel(heif_channel_Cr, 511);
  REQUIRE_FALSE(base->add_channel(heif_channel_Alpha, 1, 1, 8, nullptr));
  base->fill_channel(heif_channel_Alpha, 128);
  base->set_premultiplied_alpha(premultiplied);
  auto alternate = make_pixels(1, false, 0, 8)->get_color_profile_nclx();
  auto raster = alternate;
  raster.set_matrix_coefficients(17);
  base->set_color_profile_nclx(raster);
  auto* options = heif_decoding_options_alloc();
  REQUIRE(options);
  auto result = reconstruct_tone_map(base, make_pixels(1, true, 0, 2),
                                     GainMapMetadata{}, alternate, *options, nullptr);
  REQUIRE(result);
  REQUIRE(sample_at(**result, heif_channel_R, 0) == (premultiplied ? 32896 : 65535));
  REQUIRE(sample_at(**result, heif_channel_G, 0) == 0);
  REQUIRE(sample_at(**result, heif_channel_B, 0) == 0);
  REQUIRE((*result)->is_premultiplied_alpha() == premultiplied);
  REQUIRE((*result)->get_bits_per_pixel(heif_channel_Alpha) == 8);
  REQUIRE((*result)->get_channel_memory(heif_channel_Alpha, nullptr)[0] == 128);
  REQUIRE(base->get_channel_memory<uint16_t>(heif_channel_Cr, nullptr)[0] == 511);
  heif_decoding_options_free(options);
}

TEST_CASE("Tone-map matrix bit depths and upsampling requirements are explicit")
{
  auto base = std::make_shared<HeifPixelImage>();
  base->create(1, 1, heif_colorspace_YCbCr, heif_chroma_420);
  for (auto channel : {heif_channel_Y, heif_channel_Cb, heif_channel_Cr}) {
    REQUIRE_FALSE(base->add_channel(channel, 1, 1, 3, nullptr));
  }
  base->fill_channel(heif_channel_Y, 0);
  base->fill_channel(heif_channel_Cb, 4);
  base->fill_channel(heif_channel_Cr, 7);
  auto alternate = make_pixels(1, false, 0, 8)->get_color_profile_nclx();
  auto profile = alternate;
  profile.set_matrix_coefficients(16); // Three coded bits encode one RGB bit.
  base->set_color_profile_nclx(profile);
  auto gain = make_pixels(1, true, 0, 2);
  auto* options = heif_decoding_options_alloc();
  REQUIRE(options);
  auto result = reconstruct_tone_map(base, gain, GainMapMetadata{}, alternate, *options, nullptr);
  REQUIRE(result);
  REQUIRE(sample_at(**result, heif_channel_R, 0) == 65535);
  profile.set_full_range_flag(false);
  base->set_color_profile_nclx(profile);
  REQUIRE_FALSE(reconstruct_tone_map(base, gain, GainMapMetadata{}, alternate, *options, nullptr));
  profile.set_full_range_flag(true);
  profile.set_matrix_coefficients(8);
  base->set_color_profile_nclx(profile);
  options->color_conversion_options.preferred_chroma_upsampling_algorithm = heif_chroma_upsampling_bilinear;
  options->color_conversion_options.only_use_preferred_chroma_algorithm = true;
  REQUIRE(reconstruct_tone_map(base, gain, GainMapMetadata{}, alternate, *options, nullptr));
  options->color_conversion_options.only_use_preferred_chroma_algorithm = false;
  REQUIRE(reconstruct_tone_map(base, gain, GainMapMetadata{}, alternate, *options, nullptr));
  REQUIRE_FALSE(base->add_channel(heif_channel_Cr, 1, 1, 4, nullptr));
  REQUIRE_FALSE(reconstruct_tone_map(base, gain, GainMapMetadata{}, alternate, *options, nullptr));
  heif_decoding_options_free(options);
}


TEST_CASE("Tone maps invert constant luminance with independent high precision references")
{
  struct Reference {
    uint16_t matrix, primaries, transfer;
    bool limited;
    std::array<uint16_t, 3> encoded, rgb;
  };
  // H.273 Eq.66-75, computed independently with 70-digit Decimal arithmetic.
  // Both chroma signs, full/limited range and physical PQ/ST428 scaling are
  // covered. The expected green differs from an NCL matrix conversion.
  const auto reference = GENERATE(
Reference{10, 9, 4, false, {32768, 28768, 37768}, {37321, 31407, 24987}},
      Reference{10, 9, 4, false, {32768, 36768, 27768}, {24062, 34965, 38553}},
      Reference{10, 9, 4, true, {32768, 28768, 37768}, {38720, 31920, 24623}},
      Reference{10, 9, 4, true, {32768, 36768, 27768}, {23566, 35945, 40127}},
      Reference{10, 9, 8, false, {32768, 28768, 37768}, {40141, 30569, 25242}},
      Reference{10, 9, 8, false, {32768, 36768, 27768}, {25395, 34967, 40294}},
      Reference{10, 9, 8, true, {32768, 28768, 37768}, {41942, 31003, 24915}},
      Reference{10, 9, 8, true, {32768, 36768, 27768}, {25089, 36028, 42116}},
      Reference{10, 9, 14, false, {32768, 28768, 37768}, {37737, 31255, 25007}},
      Reference{10, 9, 14, false, {32768, 36768, 27768}, {24177, 34975, 39096}},
      Reference{10, 9, 14, true, {32768, 28768, 37768}, {39195, 31747, 24646}},
      Reference{10, 9, 14, true, {32768, 36768, 27768}, {23697, 35965, 40747}},
      Reference{10, 9, 16, false, {16384, 28768, 37768}, {17813, 16104, 8435}},
      Reference{10, 9, 16, false, {16384, 36768, 27768}, {6705, 17649, 18824}},
      Reference{10, 9, 16, true, {16384, 28768, 37768}, {15997, 13964, 5280}},
      Reference{10, 9, 16, true, {16384, 36768, 27768}, {3302, 15540, 17152}},
      Reference{10, 10, 17, false, {32768, 28768, 37768}, {36788, 31579, 24954}},
      Reference{10, 10, 17, false, {32768, 36768, 27768}, {23874, 34873, 38069}},
      Reference{10, 10, 17, true, {32768, 28768, 37768}, {38110, 32112, 24585}},
      Reference{10, 10, 17, true, {32768, 36768, 27768}, {23351, 35821, 39574}},
      Reference{13, 1, 13, false, {32768, 28768, 37768}, {37783, 31780, 25027}},
      Reference{13, 1, 13, false, {32768, 36768, 27768}, {23768, 34320, 38385}},
      Reference{13, 1, 13, true, {32768, 28768, 37768}, {39247, 32347, 24669}},
      Reference{13, 1, 13, true, {32768, 36768, 27768}, {23230, 35220, 39935}},
      Reference{10, 9, 18, false, {32768, 28768, 37768}, {35285, 32285, 24858}},
      Reference{10, 9, 18, false, {32768, 36768, 27768}, {23329, 35144, 37394}},
      Reference{10, 9, 18, true, {32768, 28768, 37768}, {36392, 32894, 24475}},
      Reference{10, 9, 18, true, {32768, 36768, 27768}, {22728, 36016, 38802}},
      Reference{13, 1, 13, false, {32768, 0, 32768}, {32768, 35479, 0}});
  auto base = std::make_shared<HeifPixelImage>();
  base->create(1, 1, heif_colorspace_YCbCr, heif_chroma_444);
  const std::array<heif_channel, 3> channels{heif_channel_Y, heif_channel_Cb, heif_channel_Cr};
  for (size_t c = 0; c < channels.size(); ++c) {
    REQUIRE_FALSE(base->add_channel(channels[c], 1, 1, 16, nullptr));
    base->fill_channel(channels[c], reference.encoded[c]);
  }
  nclx_profile profile;
  profile.set_colour_primaries(reference.primaries);
  profile.set_transfer_characteristics(reference.transfer);
  profile.set_matrix_coefficients(reference.matrix);
  profile.set_full_range_flag(!reference.limited);
  base->set_color_profile_nclx(profile);
  auto alternate = profile;
  alternate.set_matrix_coefficients(0);
  alternate.set_full_range_flag(true);
  auto* options = heif_decoding_options_alloc();
  REQUIRE(options);
  INFO(reference.matrix << "/" << reference.transfer << "/" << reference.limited);
  auto result = reconstruct_tone_map(base, make_pixels(1, true, 0, 2),
                                     GainMapMetadata{}, alternate, *options, nullptr);
  REQUIRE(result);
  const std::array<heif_channel, 3> rgb_channels{heif_channel_R, heif_channel_G, heif_channel_B};
  for (size_t c = 0; c < rgb_channels.size(); ++c) {
    REQUIRE(sample_at(**result, rgb_channels[c], 0) == Catch::Approx(reference.rgb[c]).margin(1));
    REQUIRE(base->get_channel_memory<uint16_t>(channels[c], nullptr)[0] == reference.encoded[c]);
  }
  heif_decoding_options_free(options);
}

TEST_CASE("Matrix signal transfer is distinct from HDR display linearization")
{
  REQUIRE(*gain_map_decode_matrix_signal(0.5, 18) == Catch::Approx(1.0 / 12).epsilon(1e-12));
  REQUIRE(*gain_map_encode_matrix_signal(1.0 / 12, 18) == Catch::Approx(0.5).epsilon(1e-12));
  REQUIRE(*gain_map_decode_matrix_signal(0.5080784215173991, 16) == Catch::Approx(0.01).epsilon(1e-10));
  REQUIRE(*gain_map_encode_matrix_signal(0.01, 16) == Catch::Approx(0.5080784215173991).epsilon(1e-10));
  REQUIRE(*gain_map_decode_matrix_signal(-0.5, 13) == Catch::Approx(-0.21404114048223255).epsilon(1e-12));
  REQUIRE(*gain_map_encode_matrix_signal(-0.21404114048223255, 13) == Catch::Approx(-0.5).epsilon(1e-12));
  REQUIRE_FALSE(gain_map_decode_matrix_signal(0.5, 2));
  REQUIRE_FALSE(gain_map_encode_matrix_signal(std::numeric_limits<double>::infinity(), 18));
  REQUIRE_FALSE(gain_map_decode_matrix_signal(std::numeric_limits<double>::quiet_NaN(), 18));
}

TEST_CASE("Constant-luminance gain rasters cannot infer missing transfer or primaries")
{
  const uint16_t matrix = GENERATE(uint16_t{10}, uint16_t{13});
  auto gain = std::make_shared<HeifPixelImage>();
  gain->create(1, 1, heif_colorspace_YCbCr, heif_chroma_444);
  for (auto channel : {heif_channel_Y, heif_channel_Cb, heif_channel_Cr}) {
    REQUIRE_FALSE(gain->add_channel(channel, 1, 1, 8, nullptr));
    gain->fill_channel(channel, 128);
  }
  nclx_profile profile;
  profile.set_colour_primaries(2);
  profile.set_transfer_characteristics(2);
  profile.set_matrix_coefficients(matrix);
  profile.set_full_range_flag(true);
  gain->set_color_profile_nclx(profile);
  auto base = make_pixels(1, false, 16384, 8);
  auto* options = heif_decoding_options_alloc();
  REQUIRE(options);
  auto result = reconstruct_tone_map(base, gain, GainMapMetadata{}, base->get_color_profile_nclx(), *options, nullptr);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().error_code == heif_error_Unsupported_feature);
  if (matrix == 13) {
    // An independent item colour description cannot supply missing storage
    // primaries for a chromaticity-derived matrix.
    profile.set_transfer_characteristics(8);
    gain->set_color_profile_nclx(profile);
    result = reconstruct_tone_map(gain, make_pixels(1, true, 0, 2), GainMapMetadata{},
                                  base->get_color_profile_nclx(), *options, nullptr,
                                  GainMapColour(base->get_color_profile_nclx()));
    REQUIRE_FALSE(result);
    REQUIRE(result.error().error_code == heif_error_Unsupported_feature);
  }
  heif_decoding_options_free(options);
}


TEST_CASE("Tone maps invert LMS matrices with independent high precision references")
{
  struct Reference {
    uint16_t matrix, transfer;
    int y_bits, c_bits;
    bool limited;
    std::array<uint16_t, 3> encoded, rgb;
  };
  // Independent 70-digit Gaussian elimination of H.273 Eq.14-22 and
  // Eq.79-87. Mixed Y/C depths and both chroma signs exercise raster scaling.
  const auto reference = GENERATE(
Reference{14, 16, 16, 16, false, {32768, 30720, 33792}, {33231, 32758, 31093}},
      Reference{14, 16, 16, 16, false, {32768, 34816, 31744}, {32297, 32720, 34402}},
      Reference{14, 16, 16, 16, true, {32768, 30720, 33792}, {34044, 33500, 31598}},
      Reference{14, 16, 16, 16, true, {32768, 34816, 31744}, {32977, 33455, 35380}},
      Reference{14, 16, 12, 10, false, {2048, 480, 528}, {33239, 32765, 31099}},
      Reference{14, 16, 12, 10, false, {2048, 544, 496}, {32304, 32727, 34411}},
      Reference{14, 16, 12, 10, true, {2048, 480, 528}, {34044, 33500, 31598}},
      Reference{14, 16, 12, 10, true, {2048, 544, 496}, {32977, 33455, 35380}},
      Reference{14, 18, 16, 16, false, {32768, 30720, 33792}, {33641, 32761, 29715}},
      Reference{14, 18, 16, 16, false, {32768, 34816, 31744}, {31883, 32690, 35767}},
      Reference{14, 18, 16, 16, true, {32768, 30720, 33792}, {34514, 33487, 30023}},
      Reference{14, 18, 16, 16, true, {32768, 34816, 31744}, {32484, 33414, 36938}},
      Reference{14, 18, 12, 10, false, {2048, 480, 528}, {33649, 32769, 29720}},
      Reference{14, 18, 12, 10, false, {2048, 544, 496}, {31890, 32697, 35778}},
      Reference{14, 18, 12, 10, true, {2048, 480, 528}, {34514, 33487, 30023}},
      Reference{14, 18, 12, 10, true, {2048, 544, 496}, {32484, 33414, 36938}},
      Reference{15, 16, 16, 16, false, {32768, 30720, 33792}, {31792, 33544, 31918}},
      Reference{15, 16, 16, 16, false, {32768, 34816, 31744}, {33591, 31923, 33604}},
      Reference{15, 16, 16, 16, true, {32768, 30720, 33792}, {32385, 34397, 32543}},
      Reference{15, 16, 16, 16, true, {32768, 34816, 31744}, {34446, 32543, 34470}},
      Reference{15, 16, 12, 10, false, {2048, 480, 528}, {31799, 33552, 31925}},
      Reference{15, 16, 12, 10, false, {2048, 544, 496}, {33599, 31929, 33612}},
      Reference{15, 16, 12, 10, true, {2048, 480, 528}, {32385, 34397, 32543}},
      Reference{15, 16, 12, 10, true, {2048, 544, 496}, {34446, 32543, 34470}},
      Reference{15, 18, 16, 16, false, {32768, 30720, 33792}, {31851, 33558, 31924}},
      Reference{15, 18, 16, 16, false, {32768, 34816, 31744}, {33625, 31950, 33607}},
      Reference{15, 18, 16, 16, true, {32768, 30720, 33792}, {32436, 34413, 32548}},
      Reference{15, 18, 16, 16, true, {32768, 34816, 31744}, {34479, 32565, 34473}},
      Reference{15, 18, 12, 10, false, {2048, 480, 528}, {31857, 33566, 31930}},
      Reference{15, 18, 12, 10, false, {2048, 544, 496}, {33633, 31957, 33615}},
      Reference{15, 18, 12, 10, true, {2048, 480, 528}, {32436, 34413, 32548}},
      Reference{15, 18, 12, 10, true, {2048, 544, 496}, {34479, 32565, 34473}},
      Reference{15, 8, 16, 16, false, {32768, 30720, 33792}, {31877, 33576, 31925}},
      Reference{15, 8, 16, 16, false, {32768, 34816, 31744}, {33659, 31960, 33611}},
      Reference{15, 8, 16, 16, true, {32768, 30720, 33792}, {32497, 34439, 32553}},
      Reference{15, 8, 16, 16, true, {32768, 34816, 31744}, {34534, 32593, 34478}},
      Reference{15, 8, 12, 10, false, {2048, 480, 528}, {31883, 33584, 31932}},
      Reference{15, 8, 12, 10, false, {2048, 544, 496}, {33668, 31967, 33619}},
      Reference{15, 8, 12, 10, true, {2048, 480, 528}, {32497, 34439, 32553}},
      Reference{15, 8, 12, 10, true, {2048, 544, 496}, {34534, 32593, 34478}});
  auto base = std::make_shared<HeifPixelImage>();
  base->create(1, 1, heif_colorspace_YCbCr, heif_chroma_444);
  const std::array<heif_channel, 3> channels{heif_channel_Y, heif_channel_Cb, heif_channel_Cr};
  for (size_t c = 0; c < channels.size(); ++c) {
    REQUIRE_FALSE(base->add_channel(channels[c], 1, 1, c == 0 ? reference.y_bits : reference.c_bits, nullptr));
    base->fill_channel(channels[c], reference.encoded[c]);
  }
  nclx_profile profile;
  profile.set_colour_primaries(9);
  profile.set_transfer_characteristics(reference.transfer);
  profile.set_matrix_coefficients(reference.matrix);
  profile.set_full_range_flag(!reference.limited);
  base->set_color_profile_nclx(profile);
  auto alternate = profile;
  alternate.set_matrix_coefficients(0);
  alternate.set_full_range_flag(true);
  auto* options = heif_decoding_options_alloc();
  REQUIRE(options);
  INFO(reference.matrix << "/" << reference.transfer << "/" << reference.limited);
  auto result = reconstruct_tone_map(base, make_pixels(1, true, 0, 2),
                                     GainMapMetadata{}, alternate, *options, nullptr);
  REQUIRE(result);
  const std::array<heif_channel, 3> rgb_channels{heif_channel_R, heif_channel_G, heif_channel_B};
  for (size_t c = 0; c < rgb_channels.size(); ++c) {
    REQUIRE(sample_at(**result, rgb_channels[c], 0) == Catch::Approx(reference.rgb[c]).margin(1));
  }
  heif_decoding_options_free(options);
}

TEST_CASE("LMS conversion precedes unequal linear RGB gains and the HLG OOTF")
{
  const uint16_t transfer = GENERATE(uint16_t{16}, uint16_t{18});
  auto base = std::make_shared<HeifPixelImage>();
  base->create(1, 1, heif_colorspace_YCbCr, heif_chroma_444);
  const std::array<heif_channel, 3> channels{heif_channel_Y, heif_channel_Cb, heif_channel_Cr};
  const std::array<uint16_t, 3> values{32768, 30720, 33792};
  for (size_t c = 0; c < channels.size(); ++c) {
    REQUIRE_FALSE(base->add_channel(channels[c], 1, 1, 16, nullptr));
    base->fill_channel(channels[c], values[c]);
  }
  nclx_profile profile;
  profile.set_colour_primaries(9);
  profile.set_transfer_characteristics(transfer);
  profile.set_matrix_coefficients(14);
  profile.set_full_range_flag(true);
  base->set_color_profile_nclx(profile);
  auto alternate = profile;
  alternate.set_matrix_coefficients(0);
  GainMapMetadata metadata;
  metadata.channel_count = 3;
  for (size_t c = 0; c < 3; ++c) { metadata.channels[c].gain_map_max = {static_cast<int32_t>(c), 1}; }
  auto* options = heif_decoding_options_alloc();
  REQUIRE(options);
  auto result = reconstruct_tone_map(base, make_pixels(1, true, 65535, 2), metadata, alternate, *options, nullptr);
  REQUIRE(result);
  // Independent Decimal oracle: RGB gains 1/2/4, with the luminance-dependent
  // HLG OOTF and its inverse around the ISO linear gain equation.
  const std::array<uint16_t, 3> expected = transfer == 16 ? std::array<uint16_t, 3>{33231, 37392, 40396} :
                                                         std::array<uint16_t, 3>{32022, 41636, 48300};
  const std::array<heif_channel, 3> rgb_channels{heif_channel_R, heif_channel_G, heif_channel_B};
  for (size_t c = 0; c < rgb_channels.size(); ++c) {
    REQUIRE(sample_at(**result, rgb_channels[c], 0) == Catch::Approx(expected[c]).margin(2));
  }
  heif_decoding_options_free(options);
}

TEST_CASE("LMS matrices preserve neutral signals and reject unresolved transfer")
{
  const uint16_t matrix = GENERATE(uint16_t{14}, uint16_t{15});
  const uint16_t transfer = GENERATE(uint16_t{16}, uint16_t{18});
  for (double level : {0.0, 0.5, 1.0}) {
    auto result = gain_map_decode_lms_matrix({level, 0, 0}, matrix, transfer);
    REQUIRE(result);
    for (double value : *result) { REQUIRE(value == Catch::Approx(level).margin(1e-6)); }
  }
  REQUIRE_FALSE(gain_map_decode_lms_matrix({0.5, 0, 0}, matrix, 2));
  REQUIRE_FALSE(gain_map_decode_lms_matrix({std::numeric_limits<double>::infinity(), 0, 0}, matrix, transfer));
  REQUIRE_FALSE(gain_map_decode_lms_matrix({0.5, 0, 0}, 2, transfer));
}

TEST_CASE("Tone-map matrix paths honor mandatory bilinear chroma sampling on odd and even borders")
{
  const auto chroma = GENERATE(heif_chroma_422, heif_chroma_420);
  const uint32_t width = GENERATE(5, 6);
  const uint32_t height = GENERATE(5, 6);
  const int y_bits = GENERATE(8, 10, 16);
  const int c_bits = GENERATE(8, 12, 16);
  auto base = std::make_shared<HeifPixelImage>();
  base->create(width, height, heif_colorspace_YCbCr, chroma);
  REQUIRE_FALSE(base->add_channel(heif_channel_Y, width, height, y_bits, nullptr));
  const uint32_t c_height = chroma == heif_chroma_420 ? (height + 1) / 2 : height;
  REQUIRE_FALSE(base->add_channel(heif_channel_Cb, 3, c_height, c_bits, nullptr));
  REQUIRE_FALSE(base->add_channel(heif_channel_Cr, 3, c_height, c_bits, nullptr));
  const auto y_sample = static_cast<uint16_t>(1U << (y_bits - 1));
  const auto c_mid = static_cast<uint16_t>(1U << (c_bits - 1));
  const auto c_unit = static_cast<uint16_t>(1U << (c_bits - 8));
  base->fill_channel(heif_channel_Y, y_sample);
  base->fill_channel(heif_channel_Cr, c_mid);
  size_t stride = 0;
  auto* cb = base->get_channel_memory(heif_channel_Cb, &stride);
  for (uint32_t y = 0; y < c_height; ++y) {
    auto* row = cb + size_t(y) * stride;
    for (uint32_t x = 0; x < 3; ++x) {
      const auto value = static_cast<uint16_t>((120 + 8 * x + 16 * y) * c_unit);
      if (c_bits <= 8) { row[x] = static_cast<uint8_t>(value); }
      else { reinterpret_cast<uint16_t*>(row)[x] = value; }
    }
  }
  REQUIRE_FALSE(base->add_channel(heif_channel_Alpha, width, height, 8, nullptr));
  base->fill_channel(heif_channel_Alpha, 128);
  auto alternate = make_pixels(1, false, 0, 8)->get_color_profile_nclx();
  auto raster = alternate;
  raster.set_matrix_coefficients(11);
  base->set_color_profile_nclx(raster);
  auto* options = heif_decoding_options_alloc();
  REQUIRE(options);
  options->color_conversion_options.preferred_chroma_upsampling_algorithm = heif_chroma_upsampling_bilinear;
  options->color_conversion_options.only_use_preferred_chroma_algorithm = true;
  auto result = reconstruct_tone_map(base, make_pixels(1, true, 0, 2),
                                     GainMapMetadata{}, alternate, *options, nullptr);
  REQUIRE(result);
  const std::array<double, 6> location{0, 0.25, 0.75, 1.25, 1.75, 2};
  size_t out_stride = 0;
  const auto* blue = (*result)->get_channel_memory<uint16_t>(heif_channel_B, &out_stride);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const double c = (120 + 8 * location[x] + 16 * (chroma == heif_chroma_420 ? location[y] : y)) * c_unit;
      const double ey = static_cast<double>(y_sample) / ((1U << y_bits) - 1);
      const double ec = (c - c_mid) / ((1U << c_bits) - 1);
      const double expected = std::clamp((ey + 2 * ec) / 0.986566, 0.0, 1.0) * 65535;
      REQUIRE(blue[size_t(y) * out_stride / 2 + x] == Catch::Approx(std::round(expected)).margin(1));
    }
  }
  REQUIRE((*result)->get_bits_per_pixel(heif_channel_Alpha) == 8);
  REQUIRE((*result)->get_channel_memory(heif_channel_Alpha, nullptr)[0] == 128);
  REQUIRE(base->get_chroma_format() == chroma);
  REQUIRE(base->get_bits_per_pixel(heif_channel_Y) == y_bits);
  REQUIRE(base->get_bits_per_pixel(heif_channel_Cb) == c_bits);
  REQUIRE((c_bits <= 8 ? cb[2] : reinterpret_cast<const uint16_t*>(cb)[2]) == 136 * c_unit);
  heif_decoding_options_free(options);
}

TEST_CASE("Tone maps normalize and interpolate Cb and Cr at independent depths")
{
  const int cb_bits = GENERATE(8, 12, 16);
  const int cr_bits = GENERATE(8, 10, 16);
  const auto chroma = GENERATE(heif_chroma_444, heif_chroma_422, heif_chroma_420);
  const bool full_range = GENERATE(false, true);
  const bool as_gain = GENERATE(false, true);
  auto raster = std::make_shared<HeifPixelImage>();
  raster->create(5, 3, heif_colorspace_YCbCr, chroma);
  REQUIRE_FALSE(raster->add_channel(heif_channel_Y, 5, 3, 8, nullptr));
  raster->fill_channel(heif_channel_Y, 128);
  const std::array<heif_channel, 2> channels{heif_channel_Cb, heif_channel_Cr};
  const std::array<int, 2> depths{cb_bits, cr_bits};
  const uint32_t cw = chroma == heif_chroma_444 ? 5 : 3;
  const uint32_t ch = chroma == heif_chroma_420 ? 2 : 3;
  for (size_t c = 0; c < 2; ++c) {
    REQUIRE_FALSE(raster->add_channel(channels[c], cw, ch, depths[c], nullptr));
    size_t stride = 0;
    auto* plane = raster->get_channel_memory(channels[c], &stride);
    for (uint32_t y = 0; y < ch; ++y) {
      for (uint32_t x = 0; x < cw; ++x) {
        const uint16_t value = static_cast<uint16_t>((120 + 8 * x + 16 * y + 4 * c) << (depths[c] - 8));
        if (depths[c] <= 8) { plane[size_t(y) * stride + x] = static_cast<uint8_t>(value); }
        else { reinterpret_cast<uint16_t*>(plane + size_t(y) * stride)[x] = value; }
      }
    }
  }
  auto alternate = make_pixels(1, false, 0, 8)->get_color_profile_nclx();
  auto profile = alternate;
  profile.set_matrix_coefficients(11);
  profile.set_full_range_flag(full_range);
  raster->set_color_profile_nclx(profile);
  auto* options = heif_decoding_options_alloc();
  REQUIRE(options);
  options->color_conversion_options.preferred_chroma_upsampling_algorithm = heif_chroma_upsampling_bilinear;
  options->color_conversion_options.only_use_preferred_chroma_algorithm = true;
  GainMapMetadata metadata;
  metadata.channels[0].gain_map_max = {1, 1};
  auto base = std::make_shared<HeifPixelImage>();
  base->create(5, 3, heif_colorspace_RGB, heif_chroma_444);
  for (auto channel : {heif_channel_R, heif_channel_G, heif_channel_B}) {
    REQUIRE_FALSE(base->add_channel(channel, 5, 3, 8, nullptr));
    base->fill_channel(channel, 64);
  }
  base->set_color_profile_nclx(alternate);
  auto result = reconstruct_tone_map(as_gain ? base : raster,
                                     as_gain ? raster : make_pixels(1, true, 0, 8),
                                     metadata, alternate, *options, nullptr);
  REQUIRE(result);
  const std::array<double, 5> position{0, 0.25, 0.75, 1.25, 1.75};
  for (uint32_t y = 0; y < 3; ++y) {
    for (uint32_t x = 0; x < 5; ++x) {
      const double cx = chroma == heif_chroma_444 ? x : position[x];
      const double cy = chroma == heif_chroma_420 ? position[y] : y;
      const double ey = full_range ? 128.0 / 255 : 112.0 / 219;
      const double cb = (120 + 8 * cx + 16 * cy - 128) * std::ldexp(1.0, cb_bits - 8) /
                        (full_range ? std::ldexp(1.0, cb_bits) - 1 : std::ldexp(224.0, cb_bits - 8));
      const double cr = (124 + 8 * cx + 16 * cy - 128) * std::ldexp(1.0, cr_bits - 8) /
                        (full_range ? std::ldexp(1.0, cr_bits) - 1 : std::ldexp(224.0, cr_bits - 8));
      const std::array<double, 3> rgb{2 * cr + 0.991902 * ey, ey, (2 * cb + ey) / 0.986566};
      for (size_t c = 0; c < 3; ++c) {
        const double v = std::clamp(rgb[c], 0.0, 1.0);
        const double expected = as_gain ? (64.0 / 255) * std::exp2(v) : v;
        size_t stride = 0;
        const auto* plane = (*result)->get_channel_memory<uint16_t>(static_cast<heif_channel>(heif_channel_R + c), &stride);
        REQUIRE(plane[size_t(y) * stride / 2 + x] == Catch::Approx(std::round(expected * 65535)).margin(1));
      }
    }
  }
  REQUIRE(raster->get_bits_per_pixel(heif_channel_Cb) == cb_bits);
  REQUIRE(raster->get_bits_per_pixel(heif_channel_Cr) == cr_bits);
  heif_decoding_options_free(options);
}

TEST_CASE("Wide integer YCbCr preserves independent depths through gain reconstruction")
{
  const int y_bits = GENERATE(17, 32);
  const uint16_t matrix = GENERATE(uint16_t{6}, uint16_t{11});
  const auto chroma = GENERATE(heif_chroma_444, heif_chroma_422, heif_chroma_420);
  const bool full_range = GENERATE(false, true);
  const bool as_gain = GENERATE(false, true);
  const bool bilinear = GENERATE(false, true);
  const std::array<int, 3> depths{y_bits, 17, 32};
  const std::array<heif_channel, 3> channels{heif_channel_Y, heif_channel_Cb, heif_channel_Cr};
  auto raster = std::make_shared<HeifPixelImage>();
  raster->create(5, 3, heif_colorspace_YCbCr, chroma);
  for (size_t c = 0; c < 3; ++c) {
    const uint32_t cw = c == 0 || chroma == heif_chroma_444 ? 5 : 3;
    const uint32_t ch = c == 0 || chroma != heif_chroma_420 ? 3 : 2;
    REQUIRE_FALSE(raster->add_channel(channels[c], cw, ch, depths[c], nullptr));
    size_t stride = 0;
    auto* plane = raster->get_channel_memory(channels[c], &stride);
    for (uint32_t y = 0; y < ch; ++y) {
      auto* row = reinterpret_cast<uint32_t*>(plane + size_t(y) * stride);
      for (uint32_t x = 0; x < cw; ++x) {
        row[x] = static_cast<uint32_t>(((c == 0 ? 128 : 116 + 4 * c + 8 * x + 16 * y) <<
                                       (depths[c] - 8)) + 1);
      }
    }
  }
  auto alternate = make_pixels(1, false, 0, 8)->get_color_profile_nclx();
  auto profile = alternate;
  profile.set_matrix_coefficients(matrix);
  profile.set_full_range_flag(full_range);
  raster->set_color_profile_nclx(profile);
  auto* options = heif_decoding_options_alloc();
  REQUIRE(options);
  options->color_conversion_options.preferred_chroma_upsampling_algorithm = bilinear ?
      heif_chroma_upsampling_bilinear : heif_chroma_upsampling_nearest_neighbor;
  options->color_conversion_options.only_use_preferred_chroma_algorithm = true;
  GainMapMetadata metadata;
  metadata.channels[0].gain_map_max = {1, 1};
  metadata.channels[0].gamma = {3, 2};
  auto base = std::make_shared<HeifPixelImage>();
  base->create(5, 3, heif_colorspace_RGB, heif_chroma_444);
  for (auto channel : {heif_channel_R, heif_channel_G, heif_channel_B}) {
    REQUIRE_FALSE(base->add_channel(channel, 5, 3, 16, nullptr));
    base->fill_channel(channel, 16384);
  }
  base->set_color_profile_nclx(alternate);
  auto result = reconstruct_tone_map(as_gain ? base : raster,
                                     as_gain ? raster : make_pixels(1, true, 0, 8),
                                     metadata, alternate, *options, nullptr, std::nullopt, std::nullopt, true);
  REQUIRE(result);
  const std::array<double, 5> position{0, 0.25, 0.75, 1.25, 1.75};
  for (uint32_t y = 0; y < 3; ++y) {
    for (uint32_t x = 0; x < 5; ++x) {
      const double cx = chroma == heif_chroma_444 ? x : bilinear ? position[x] : x / 2;
      const double cy = chroma != heif_chroma_420 ? y : bilinear ? position[y] : y / 2;
      const double y_code = std::ldexp(128.0, y_bits - 8) + 1;
      const double ey = full_range ? y_code / (std::ldexp(1.0, y_bits) - 1) :
          (y_code - std::ldexp(16.0, y_bits - 8)) / std::ldexp(219.0, y_bits - 8);
      const double cb = (std::ldexp(-8 + 8 * cx + 16 * cy, 17 - 8) + 1) /
          (full_range ? std::ldexp(1.0, 17) - 1 : std::ldexp(224.0, 17 - 8));
      const double cr = (std::ldexp(-4 + 8 * cx + 16 * cy, 32 - 8) + 1) /
          (full_range ? std::ldexp(1.0, 32) - 1 : std::ldexp(224.0, 32 - 8));
      const double r = matrix == 6 ? ey + 1.402 * cr : 2 * cr + 0.991902 * ey;
      const double b = matrix == 6 ? ey + 1.772 * cb : (2 * cb + ey) / 0.986566;
      const GainMapRGB rgb{r, matrix == 6 ? (ey - 0.299 * r - 0.114 * b) / 0.587 : ey, b};
      for (size_t c = 0; c < 3; ++c) {
        const double v = std::clamp(rgb[c], 0.0, 1.0);
        const double expected = as_gain ? (16384.0 / 65535) * std::exp2(std::pow(v, 2.0 / 3)) : v;
        size_t stride = 0;
        const auto* plane = (*result)->get_channel_memory<float>(static_cast<heif_channel>(heif_channel_R + c), &stride);
        REQUIRE(plane[size_t(y) * stride / sizeof(float) + x] == Catch::Approx(expected).margin(1e-7));
      }
    }
  }
  REQUIRE(raster->get_channel_memory<uint32_t>(heif_channel_Y, nullptr)[0] ==
          (uint32_t{128} << (y_bits - 8)) + 1);
  heif_decoding_options_free(options);
}

TEST_CASE("Tone-map inputs honour declared H273 chroma positions")
{
  const uint8_t location = GENERATE(uint8_t{0}, uint8_t{1}, uint8_t{2}, uint8_t{3}, uint8_t{4}, uint8_t{5});
  const bool as_gain = GENERATE(false, true);
  auto raster = std::make_shared<HeifPixelImage>();
  raster->create(4, 4, heif_colorspace_YCbCr, heif_chroma_420);
  auto profile = make_pixels(1, false, 0, 8)->get_color_profile_nclx();
  profile.set_matrix_coefficients(6);
  raster->set_color_profile_nclx(profile);
  raster->set_chroma_location(location);
  REQUIRE_FALSE(raster->add_channel(heif_channel_Y, 4, 4, 8, nullptr));
  raster->fill_channel(heif_channel_Y, 128);
  for (auto channel : {heif_channel_Cb, heif_channel_Cr}) {
    REQUIRE_FALSE(raster->add_channel(channel, 2, 2, 8, nullptr));
    size_t stride = 0;
    auto* plane = raster->get_channel_memory(channel, &stride);
    for (uint32_t y = 0; y < 2; ++y) {
      for (uint32_t x = 0; x < 2; ++x) {
        plane[size_t(y) * stride + x] = static_cast<uint8_t>((channel == heif_channel_Cb ? 112 : 128) + 16 * x + 32 * y);
      }
    }
  }
  auto* options = heif_decoding_options_alloc();
  REQUIRE(options);
  options->color_conversion_options.preferred_chroma_upsampling_algorithm = heif_chroma_upsampling_bilinear;
  options->color_conversion_options.only_use_preferred_chroma_algorithm = true;
  GainMapMetadata metadata;
  metadata.channels[0].gain_map_max = {as_gain ? 1 : 0, 1};
  auto base = make_pixels(4, false, 8192, 8);
  // Full baseline geometry matches the 4:2:0 raster in either input role.
  if (as_gain) {
    const auto rgb_profile = base->get_color_profile_nclx();
    base = std::make_shared<HeifPixelImage>();
    base->create(4, 4, heif_colorspace_RGB, heif_chroma_444);
    base->set_color_profile_nclx(rgb_profile);
    for (auto channel : {heif_channel_R, heif_channel_G, heif_channel_B}) {
      REQUIRE_FALSE(base->add_channel(channel, 4, 4, 16, nullptr));
      base->fill_channel(channel, 8192);
    }
  }
  auto result = reconstruct_tone_map(as_gain ? base : raster, as_gain ? raster : make_pixels(1, true, 0, 2),
                                     metadata, base->get_color_profile_nclx(), *options, nullptr,
                                     std::nullopt, std::nullopt, true);
  INFO("location=" << int(location));
  REQUIRE(result);
  // H.273 (2024) Table 8: the known ramps at luma position (1,1).
  // Both chroma planes increment by 16 horizontally and 32 vertically.
  const std::array<int, 6> cb_code{128, 124, 136, 132, 120, 116};
  const double cb = (cb_code[location] - 128.0) / 255;
  const double cr = cb_code[location] / 255.0 - 112.0 / 255;
  constexpr double kr = 0.299, kb = 0.114;
  const double ey = 128.0 / 255;
  const double r = ey + 2 * (1 - kr) * cr;
  const double b = ey + 2 * (1 - kb) * cb;
  const std::array<double, 3> rgb{r, (ey - kr * r - kb * b) / (1 - kr - kb), b};
  for (size_t c = 0; c < 3; ++c) {
    size_t stride = 0;
    const auto* output = (*result)->get_channel_memory<float>(static_cast<heif_channel>(heif_channel_R + c), &stride);
    const double expected = as_gain ? (8192.0 / 65535) * std::exp2(rgb[c]) : rgb[c];
    REQUIRE(output[stride / sizeof(float) + 1] == Catch::Approx(expected).margin(1e-7));
  }
  REQUIRE(raster->get_chroma_location() == location);
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

TEST_CASE("Explicit gain-map phases interpolate log gains after inverse gamma")
{
  const bool centered = GENERATE(false, true);
  auto base = make_pixels(4, false, 16384, 8);
  auto gain = std::make_shared<HeifPixelImage>();
  gain->create(2, 1, heif_colorspace_monochrome, heif_chroma_monochrome);
  REQUIRE_FALSE(gain->add_channel(heif_channel_Y, 2, 1, 32, nullptr,
                                  heif_component_datatype_floating_point));
  auto* pixels = gain->get_channel_memory<float>(heif_channel_Y, nullptr);
  pixels[0] = 0.25F;
  pixels[1] = 0.5625F;
  GainMapMetadata metadata;
  metadata.channels[0].gain_map_max = {2, 1};
  metadata.channels[0].gamma = {2, 1};
  auto* options = heif_decoding_options_alloc();
  REQUIRE(options);
  auto result = reconstruct_tone_map(base, gain, metadata, base->get_color_profile_nclx(),
                                     *options, nullptr, std::nullopt, std::nullopt, true, centered);
  REQUIRE(result);
  // Inverse gamma produces log gains 1 and 1.5 before either interpolation.
  const std::array<double, 4> log_gain = centered ? std::array<double, 4>{1, 1.125, 1.375, 1.5} :
                                                 std::array<double, 4>{1, 1.25, 1.5, 1.5};
  for (auto channel : {heif_channel_R, heif_channel_G, heif_channel_B}) {
    const auto* output = (*result)->get_channel_memory<float>(channel, nullptr);
    for (size_t x = 0; x < 4; ++x) {
      REQUIRE(output[x] == Catch::Approx((16384.0 / 65535) * std::exp2(log_gain[x])).margin(1e-7));
    }
  }
  heif_decoding_options_free(options);
}

TEST_CASE("Reader supports full-range gain maps below the recommended 8 bits")
{
  auto base = make_pixels(1, false, 16384, 8);
  auto gain = make_pixels(1, true, 15, 2, 4);
  GainMapMetadata metadata;
  auto* options = heif_decoding_options_alloc();
  REQUIRE(options);

  auto result = reconstruct_tone_map(
      base, gain, metadata, base->get_color_profile_nclx(), *options, nullptr);
  REQUIRE(result);
  REQUIRE(sample_at(**result, heif_channel_R, 0) == 16384);

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

TEST_CASE("Premultiplied baseline reconstructs straight colours before restoring alpha")
{
  auto base = make_pixels(3, false, 39321, 13); // Straight sRGB signal 0.6.
  const uint8_t alpha_bits = GENERATE(uint8_t{8}, uint8_t{16});
  REQUIRE_FALSE(base->add_channel(heif_channel_Alpha, 3, 1, alpha_bits, nullptr));
  if (alpha_bits == 8) {
    auto* alpha = base->get_channel_memory(heif_channel_Alpha, nullptr);
    alpha[0] = 0;
    alpha[1] = 85;
    alpha[2] = 255;
  }
  else {
    auto* alpha = base->get_channel_memory<uint16_t>(heif_channel_Alpha, nullptr);
    alpha[0] = 0;
    alpha[1] = 21845;
    alpha[2] = 65535;
  }
  // Exactly one third alpha; premultiplied signal is 0.2.
  for (auto channel : {heif_channel_R, heif_channel_G, heif_channel_B}) {
    base->get_channel_memory<uint16_t>(channel, nullptr)[1] = 13107;
  }
  base->set_premultiplied_alpha(true);
  auto gain = make_pixels(1, true, 65535, 2);
  GainMapMetadata metadata;
  metadata.channels[0].gain_map_max = {1, 1};
  metadata.channels[0].base_offset = {1, 8};
  metadata.channels[0].alternate_offset = {1, 16};
  auto alternate = base->get_color_profile_nclx();
  alternate.set_transfer_characteristics(16);
  auto* options = heif_decoding_options_alloc();
  REQUIRE(options);
  auto result = reconstruct_tone_map(base, gain, metadata, alternate, *options, nullptr);
  REQUIRE(result);
  REQUIRE((*result)->is_premultiplied_alpha());
  // Independent sRGB EOTF, ISO Formula (2), then ST 2084 encoding.
  const double linear = (std::pow((0.6 + 0.055) / 1.055, 2.4) + 0.125) * 2 - 0.0625;
  const double p = std::pow(linear * 203 / 10000, 2610.0 / 16384);
  const double encoded = std::pow((3424.0 / 4096 + 2413.0 / 128 * p) /
                                  (1 + 2392.0 / 128 * p), 2523.0 / 32);
  for (auto channel : {heif_channel_R, heif_channel_G, heif_channel_B}) {
    REQUIRE(sample_at(**result, channel, 0) == 0);
    REQUIRE(sample_at(**result, channel, 1) == Catch::Approx(std::round(encoded / 3 * 65535)).margin(1));
    REQUIRE(sample_at(**result, channel, 2) == Catch::Approx(std::round(encoded * 65535)).margin(1));
    REQUIRE(sample_at(*base, channel, 1) == 13107);
  }
  REQUIRE((*result)->get_bits_per_pixel(heif_channel_Alpha) == 16);
  const auto* output_alpha = (*result)->get_channel_memory<uint16_t>(heif_channel_Alpha, nullptr);
  REQUIRE(output_alpha[0] == 0);
  REQUIRE(output_alpha[1] == 21845);
  REQUIRE(output_alpha[2] == 65535);

  SECTION("Clip the straight alternate before premultiplication") {
    alternate.set_transfer_characteristics(8);
    metadata.channels[0].gain_map_max = {3, 1};
    result = reconstruct_tone_map(base, gain, metadata, alternate, *options, nullptr);
    REQUIRE(result);
    REQUIRE(sample_at(**result, heif_channel_R, 1) == 21845);
    REQUIRE(sample_at(**result, heif_channel_R, 2) == 65535);
  }
  heif_decoding_options_free(options);
}

TEST_CASE("Premultiplied tone-map input requires an alpha channel")
{
  auto base = make_pixels(1, false, 16384, 8);
  base->set_premultiplied_alpha(true);
  auto gain = make_pixels(1, true, 65535, 2);
  auto* options = heif_decoding_options_alloc();
  REQUIRE(options);
  auto result = reconstruct_tone_map(base, gain, GainMapMetadata{}, base->get_color_profile_nclx(), *options, nullptr);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().error_code == heif_error_Invalid_input);
  heif_decoding_options_free(options);
}

TEST_CASE("Requested output converts primaries on straight colours")
{
  auto image = make_pixels(2, false, 39321, 8);
  image->fill_channel(heif_channel_G, 0);
  image->fill_channel(heif_channel_B, 0);
  REQUIRE_FALSE(image->add_channel(heif_channel_Alpha, 2, 1, 8, nullptr));
  auto* alpha = image->get_channel_memory(heif_channel_Alpha, nullptr);
  alpha[0] = 0;
  alpha[1] = 85;
  image->get_channel_memory<uint16_t>(heif_channel_R, nullptr)[1] = 13107;
  image->set_premultiplied_alpha(true);
  heif_color_profile_nclx requested{};
  requested.color_primaries = heif_color_primaries_ITU_R_BT_2020_2_and_2100_0;
  requested.transfer_characteristics = heif_transfer_characteristic_IEC_61966_2_1;
  requested.matrix_coefficients = heif_matrix_coefficients_RGB_GBR;
  requested.full_range_flag = 1;
  auto* options = heif_decoding_options_alloc();
  REQUIRE(options);
  auto converted = convert_tone_map_colour(image, requested, *options, nullptr);
  REQUIRE(converted);
  REQUIRE((*converted)->is_premultiplied_alpha());
  const std::array<double, 3> red{0.627403896, 0.069097289, 0.016391439};
  const std::array<heif_channel, 3> channels{heif_channel_R, heif_channel_G, heif_channel_B};
  for (size_t c = 0; c < 3; ++c) {
    const double linear = 0.6 * red[c];
    const double encoded = linear <= 0.0031308 ? 12.92 * linear :
                           1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
    REQUIRE(sample_at(**converted, channels[c], 0) == 0);
    REQUIRE(sample_at(**converted, channels[c], 1) == Catch::Approx(std::round(encoded / 3 * 65535)).margin(1));
  }
  REQUIRE(sample_at(*image, heif_channel_R, 1) == 13107);
  REQUIRE((*converted)->get_color_profile_nclx().m_transfer_characteristics == 13);
  heif_decoding_options_free(options);
}


TEST_CASE("Floating RGB and monochrome retain HDR samples through reverse gain")
{
  const bool mono = GENERATE(false, true);
  const int bits = GENERATE(32, 64);
  auto base = std::make_shared<HeifPixelImage>();
  base->create(3, 2, mono ? heif_colorspace_monochrome : heif_colorspace_RGB,
               mono ? heif_chroma_monochrome : heif_chroma_444);
  const auto channels = mono ? std::vector<heif_channel>{heif_channel_Y} :
                              std::vector<heif_channel>{heif_channel_R, heif_channel_G, heif_channel_B};
  for (auto channel : channels) {
    REQUIRE_FALSE(base->add_channel(channel, 3, 2, bits, nullptr, heif_component_datatype_floating_point));
    size_t stride = 0;
    auto* plane = base->get_channel_memory(channel, &stride);
    for (size_t y = 0; y < 2; ++y) {
      for (size_t x = 0; x < 3; ++x) {
        const double value = 1.5 + static_cast<double>(x) * 0.5 + static_cast<double>(y) * 0.25;
        if (bits == 32) { reinterpret_cast<float*>(plane + y * stride)[x] = static_cast<float>(value); }
        else { reinterpret_cast<double*>(plane + y * stride)[x] = value; }
      }
    }
  }
  auto profile = make_pixels(1, false, 0, 8)->get_color_profile_nclx();
  base->set_color_profile_nclx(profile);
  auto gain = make_pixels(1, true, 65535, 2);
  GainMapMetadata metadata;
  metadata.base_hdr_headroom = {2, 1};
  metadata.alternate_hdr_headroom = {0, 1};
  metadata.channels[0].gain_map_max = {2, 1};
  auto* options = heif_decoding_options_alloc();
  REQUIRE(options);
  auto result = reconstruct_tone_map(base, gain, metadata, profile, *options, nullptr);
  REQUIRE(result);
  for (auto channel : {heif_channel_R, heif_channel_G, heif_channel_B}) {
    size_t stride = 0;
    const auto* plane = (*result)->get_channel_memory<uint16_t>(channel, &stride);
    for (size_t y = 0; y < 2; ++y) {
      for (size_t x = 0; x < 3; ++x) {
        REQUIRE(plane[y * (stride / 2) + x] == std::round((1.5 + static_cast<double>(x) * 0.5 + static_cast<double>(y) * 0.25) / 4 * 65535));
      }
    }
  }
  REQUIRE(base->get_datatype(channels[0]) == heif_component_datatype_floating_point);
  REQUIRE(base->get_bits_per_pixel(channels[0]) == bits);
  heif_decoding_options_free(options);
}

TEST_CASE("Wide unsigned RGB is normalized without a 16-bit intermediate")
{
  const int bits = GENERATE(17, 24, 32, 48, 64);
  auto base = std::make_shared<HeifPixelImage>();
  base->create(1, 1, heif_colorspace_RGB, heif_chroma_444);
  // A very small baseline amplified by 2^16 must survive until gain application.
  const uint64_t code = uint64_t{1} << (bits - 17);
  for (auto channel : {heif_channel_R, heif_channel_G, heif_channel_B}) {
    REQUIRE_FALSE(base->add_channel(channel, 1, 1, bits, nullptr));
    if (bits <= 32) { base->get_channel_memory<uint32_t>(channel, nullptr)[0] = static_cast<uint32_t>(code); }
    else { base->get_channel_memory<uint64_t>(channel, nullptr)[0] = code; }
  }
  auto profile = make_pixels(1, false, 0, 8)->get_color_profile_nclx();
  base->set_color_profile_nclx(profile);
  GainMapMetadata metadata;
  metadata.channels[0].gain_map_max = {16, 1};
  auto* options = heif_decoding_options_alloc();
  REQUIRE(options);
  auto result = reconstruct_tone_map(base, make_pixels(1, true, 65535, 2), metadata, profile, *options, nullptr);
  REQUIRE(result);
  const long double maximum = std::ldexp(1.0L, bits) - 1;
  const auto expected = std::round(static_cast<long double>(code) / maximum * 65536 * 65535);
  for (auto channel : {heif_channel_R, heif_channel_G, heif_channel_B}) {
    REQUIRE(sample_at(**result, channel, 0) == expected);
  }
  heif_decoding_options_free(options);
}

TEST_CASE("Floating gain samples are unnormalized before interpolation")
{
  const int bits = GENERATE(32, 64);
  const bool interleaved = GENERATE(false, true);
  auto gain = std::make_shared<HeifPixelImage>();
  gain->create(2, 1, interleaved ? heif_colorspace_RGB : heif_colorspace_monochrome,
                interleaved ? heif_chroma_interleaved_RGB : heif_chroma_monochrome);
  const auto channel = interleaved ? heif_channel_interleaved : heif_channel_Y;
  REQUIRE_FALSE(gain->add_channel(channel, 2, 1, bits, nullptr, heif_component_datatype_floating_point));
  const double g0 = bits == 32 ? static_cast<double>(float{1.0f / 3}) : 1.0 / 3;
  const size_t components = interleaved ? 3 : 1;
  for (size_t c = 0; c < components; ++c) {
    if (bits == 32) {
      auto* plane = gain->get_channel_memory<float>(channel, nullptr);
      plane[c] = static_cast<float>(g0); plane[components + c] = 0.75f;
    }
    else {
      auto* plane = gain->get_channel_memory<double>(channel, nullptr);
      plane[c] = g0; plane[components + c] = 0.75;
    }
  }
  auto profile = make_pixels(1, true, 0, 2)->get_color_profile_nclx();
  gain->set_color_profile_nclx(profile);
  auto base = make_pixels(4, false, 8192, 8);
  GainMapMetadata metadata;
  metadata.channels[0].gain_map_max = {2, 1};
  metadata.channels[0].gamma = {2, 1};
  auto* options = heif_decoding_options_alloc();
  REQUIRE(options);
  auto result = reconstruct_tone_map(base, gain, metadata, base->get_color_profile_nclx(), *options, nullptr);
  REQUIRE(result);
  // x=1 is halfway between independently inverse-gamma processed gain samples.
  const double log_gain = std::sqrt(g0) + std::sqrt(0.75);
  REQUIRE(sample_at(**result, heif_channel_R, 1) == std::round(8192 * std::exp2(log_gain)));
  heif_decoding_options_free(options);
}

TEST_CASE("Typed alpha is used before final quantization and rejects invalid opacity")
{
  const bool ycbcr = GENERATE(false, true);
  const bool floating = GENERATE(false, true);
  const bool premultiplied = GENERATE(false, true);
  const int bits = GENERATE(32, 64);
  auto base = make_pixels(3, false, 13107, 8);
  if (ycbcr) {
    auto profile = base->get_color_profile_nclx();
    profile.set_matrix_coefficients(6);
    base = std::make_shared<HeifPixelImage>();
    base->create(3, 1, heif_colorspace_YCbCr, heif_chroma_444);
    base->set_color_profile_nclx(profile);
    for (auto channel : {heif_channel_Y, heif_channel_Cb, heif_channel_Cr}) {
      REQUIRE_FALSE(base->add_channel(channel, 3, 1, 16, nullptr));
      base->fill_channel(channel, channel == heif_channel_Y ? 13107 : 32768);
    }
  }
  REQUIRE_FALSE(base->add_channel(heif_channel_Alpha, 3, 1, bits, nullptr,
      floating ? heif_component_datatype_floating_point : heif_component_datatype_unsigned_integer));
  auto set_alpha = [&](size_t x, double value) {
    if (floating && bits == 32) { base->get_channel_memory<float>(heif_channel_Alpha, nullptr)[x] = static_cast<float>(value); }
    else if (floating) { base->get_channel_memory<double>(heif_channel_Alpha, nullptr)[x] = value; }
    else if (bits == 32) { base->get_channel_memory<uint32_t>(heif_channel_Alpha, nullptr)[x] = static_cast<uint32_t>(value * UINT32_MAX); }
    else { base->get_channel_memory<uint64_t>(heif_channel_Alpha, nullptr)[x] = value == 1 ? UINT64_MAX :
            static_cast<uint64_t>(value * static_cast<double>(UINT64_MAX)); }
  };
  set_alpha(0, 0); set_alpha(1, 0.25); set_alpha(2, 1);
  base->set_premultiplied_alpha(premultiplied);
  auto gain = make_pixels(1, true, 65535, 2);
  GainMapMetadata metadata;
  metadata.channels[0].gain_map_max = {1, 1};
  metadata.channels[0].base_offset = {1, 8};
  metadata.channels[0].alternate_offset = {1, 16};
  auto* options = heif_decoding_options_alloc();
  REQUIRE(options);
  auto result = reconstruct_tone_map(base, gain, metadata, base->get_color_profile_nclx(), *options, nullptr);
  REQUIRE(result);
  const auto* alpha = (*result)->get_channel_memory<uint16_t>(heif_channel_Alpha, nullptr);
  REQUIRE(alpha[0] == 0); REQUIRE(alpha[1] == 16384); REQUIRE(alpha[2] == 65535);
  REQUIRE(sample_at(**result, heif_channel_R, 1) ==
          std::round(std::min((0.2 / (premultiplied ? 0.25 : 1) + 0.125) * 2 - 0.0625, 1.0) *
                     (premultiplied ? 0.25 : 1) * 65535));
  // Zero opacity yields zero premultiplied output even with nonzero offsets.
  if (premultiplied) { REQUIRE(sample_at(**result, heif_channel_R, 0) == 0); }
  if (floating) {
    for (double invalid : {-0.1, 1.1, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
      set_alpha(1, invalid);
      result = reconstruct_tone_map(base, gain, metadata, base->get_color_profile_nclx(), *options, nullptr);
      REQUIRE_FALSE(result);
      REQUIRE(result.error().error_code == heif_error_Invalid_input);
    }
  }
  heif_decoding_options_free(options);
}

TEST_CASE("Typed tone-map samples reject nonfinite and unresolved numeric formats")
{
  const bool colour_is_gain = GENERATE(false, true);
  auto typed = std::make_shared<HeifPixelImage>();
  typed->create(1, 1, heif_colorspace_RGB, heif_chroma_444);
  for (auto channel : {heif_channel_R, heif_channel_G, heif_channel_B}) {
    REQUIRE_FALSE(typed->add_channel(channel, 1, 1, 64, nullptr, heif_component_datatype_floating_point));
    typed->get_channel_memory<double>(channel, nullptr)[0] = 0.25;
  }
  auto colour = make_pixels(1, false, 0, 8)->get_color_profile_nclx();
  typed->set_color_profile_nclx(colour);
  auto base = colour_is_gain ? make_pixels(1, false, 16384, 8) : typed;
  auto gain = colour_is_gain ? typed : make_pixels(1, true, 0, 2);
  auto* options = heif_decoding_options_alloc();
  REQUIRE(options);
  for (double invalid : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
    typed->get_channel_memory<double>(heif_channel_G, nullptr)[0] = invalid;
    auto result = reconstruct_tone_map(base, gain, GainMapMetadata{}, colour, *options, nullptr);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().error_code == heif_error_Invalid_input);
  }
  typed->get_channel_memory<double>(heif_channel_G, nullptr)[0] = 0.25;
  colour.set_full_range_flag(false);
  typed->set_color_profile_nclx(colour);
  auto result = reconstruct_tone_map(base, gain, GainMapMetadata{}, colour, *options, nullptr);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().error_code == heif_error_Unsupported_feature);
  heif_decoding_options_free(options);
}

TEST_CASE("Mixed RGB sample types preserve signed extended transfer values")
{
  auto base = std::make_shared<HeifPixelImage>();
  base->create(1, 1, heif_colorspace_RGB, heif_chroma_444);
  REQUIRE_FALSE(base->add_channel(heif_channel_R, 1, 1, 32, nullptr, heif_component_datatype_floating_point));
  REQUIRE_FALSE(base->add_channel(heif_channel_G, 1, 1, 24, nullptr));
  REQUIRE_FALSE(base->add_channel(heif_channel_B, 1, 1, 64, nullptr, heif_component_datatype_floating_point));
  base->get_channel_memory<float>(heif_channel_R, nullptr)[0] = -0.2f;
  base->get_channel_memory<uint32_t>(heif_channel_G, nullptr)[0] = 1;
  base->get_channel_memory<double>(heif_channel_B, nullptr)[0] = -0.05;
  auto profile = make_pixels(1, false, 0, 11)->get_color_profile_nclx();
  base->set_color_profile_nclx(profile);
  auto alternate = profile;
  alternate.set_transfer_characteristics(8);
  GainMapMetadata metadata;
  metadata.channels[0].gain_map_max = {1, 1};
  metadata.channels[0].base_offset = {1, 4};
  auto* options = heif_decoding_options_alloc();
  REQUIRE(options);
  auto result = reconstruct_tone_map(base, make_pixels(1, true, 65535, 2), metadata, alternate, *options, nullptr);
  REQUIRE(result);
  const std::array<double, 3> signal{static_cast<double>(-0.2f), 1.0 / 16777215, -0.05};
  const std::array<heif_channel, 3> channels{heif_channel_R, heif_channel_G, heif_channel_B};
  // IEC 61966-2-4/H.273 signed BT.709 EOTF, followed by ISO Formula (2).
  for (size_t c = 0; c < 3; ++c) {
    const double v = std::abs(signal[c]);
    const double linear = std::copysign(v < 0.081242858298635 ? v / 4.5 :
        std::pow((v + 0.099296826809442) / 1.099296826809442, 1 / 0.45), signal[c]);
    REQUIRE(sample_at(**result, channels[c], 0) == std::round((linear + 0.25) * 2 * 65535));
  }
  REQUIRE(base->get_channel_memory<double>(heif_channel_B, nullptr)[0] == -0.05);
  heif_decoding_options_free(options);
}

TEST_CASE("Interleaved integer RGB keeps byte order, range and premultiplied opacity")
{
  const auto chroma = GENERATE(heif_chroma_interleaved_RGB, heif_chroma_interleaved_RGBA,
                               heif_chroma_interleaved_RRGGBB_LE, heif_chroma_interleaved_RRGGBB_BE,
                               heif_chroma_interleaved_RRGGBBAA_LE, heif_chroma_interleaved_RRGGBBAA_BE);
  const bool full_range = GENERATE(false, true);
  const bool premultiplied = GENERATE(false, true);
  const bool alpha = is_interleaved_with_alpha(chroma);
  if (premultiplied && !alpha) { return; }
  const bool byte_samples = chroma == heif_chroma_interleaved_RGB || chroma == heif_chroma_interleaved_RGBA;
  const bool big_endian = chroma == heif_chroma_interleaved_RRGGBB_BE || chroma == heif_chroma_interleaved_RRGGBBAA_BE;
  const int bits = byte_samples ? 8 : 12;
  const uint32_t max = (1U << bits) - 1;
  const double offset = full_range ? 0 : 16U << (bits - 8);
  const double scale = full_range ? max : 219U << (bits - 8);
  auto base = std::make_shared<HeifPixelImage>();
  base->create(3, 2, heif_colorspace_RGB, chroma);
  REQUIRE_FALSE(base->add_channel(heif_channel_interleaved, 3, 2, bits, nullptr));
  auto profile = make_pixels(1, false, 0, 8)->get_color_profile_nclx();
  profile.set_full_range_flag(full_range);
  base->set_color_profile_nclx(profile);
  base->set_premultiplied_alpha(premultiplied);
  size_t stride = 0;
  auto* data = base->get_channel_memory(heif_channel_interleaved, &stride);
  const size_t components = alpha ? 4 : 3;
  auto store = [&](uint32_t x, uint32_t y, size_t c, uint32_t value) {
    auto* p = data + size_t(y) * stride + (x * components + c) * (byte_samples ? 1 : 2);
    if (byte_samples) { p[0] = static_cast<uint8_t>(value); }
    else {
      p[big_endian ? 1 : 0] = static_cast<uint8_t>(value);
      p[big_endian ? 0 : 1] = static_cast<uint8_t>(value >> 8);
    }
  };
  std::array<std::array<double, 3>, 6> straight{};
  std::array<double, 6> opacity{};
  for (uint32_t y = 0; y < 2; ++y) {
    for (uint32_t x = 0; x < 3; ++x) {
      const uint32_t a = alpha ? (x == 0 ? 0 : x == 1 ? max / 2 : max) : max;
      opacity[y * 3 + x] = a / double(max);
      if (alpha) { store(x, y, 3, a); }
      for (size_t c = 0; c < 3; ++c) {
        const double signal = static_cast<double>(c + 1 + y) / 5.0;
        const uint32_t code = static_cast<uint32_t>(std::round(offset + scale * signal * (premultiplied ? a / double(max) : 1)));
        store(x, y, c, code);
        const double normalized = (code - offset) / scale;
        straight[y * 3 + x][c] = premultiplied ? (a ? normalized / opacity[y * 3 + x] : 0) : normalized;
      }
    }
  }
  GainMapMetadata metadata;
  metadata.channels[0].gain_map_min = {1, 1};
  metadata.channels[0].gain_map_max = {1, 1};
  metadata.channels[0].base_offset = {1, 8};
  metadata.channels[0].alternate_offset = {1, 16};
  profile.set_transfer_characteristics(16);
  profile.set_full_range_flag(true);
  auto* options = heif_decoding_options_alloc();
  REQUIRE(options);
  auto output = reconstruct_tone_map(base, make_pixels(3, true, 0, 2), metadata, profile, *options, nullptr);
  heif_decoding_options_free(options);
  REQUIRE(output);
  for (uint32_t y = 0; y < 2; ++y) {
    for (uint32_t x = 0; x < 3; ++x) {
      const size_t pixel = y * 3 + x;
      for (size_t c = 0; c < 3; ++c) {
        // Independent ST 2084 equation after the known gain/offset operation.
        const double linear = (straight[pixel][c] + 0.125) * 2 - 0.0625;
        const double p = std::pow(linear * 203 / 10000, 2610.0 / 16384);
        const double pq = std::pow((3424.0 / 4096 + (2413.0 / 128) * p) /
                                   (1 + (2392.0 / 128) * p), 2523.0 / 32);
        size_t output_stride = 0;
        const auto* plane = (*output)->get_channel_memory<uint16_t>(static_cast<heif_channel>(heif_channel_R + c),
                                                                    &output_stride);
        const double expected = pq * (premultiplied ? opacity[pixel] : 1) * 65535;
        REQUIRE(plane[size_t(y) * output_stride / 2 + x] == Catch::Approx(std::round(expected)).margin(1));
      }
      if (alpha) {
        size_t alpha_stride = 0;
        const auto* plane = (*output)->get_channel_memory<uint16_t>(heif_channel_Alpha, &alpha_stride);
        REQUIRE(plane[size_t(y) * alpha_stride / 2 + x] == std::round(opacity[pixel] * 65535));
      }
    }
  }
}

TEST_CASE("Typed interleaved RGB retains floating headroom and wide integer precision")
{
  const bool floating = GENERATE(false, true);
  const bool alpha = GENERATE(false, true);
  const int bits = alpha ? 64 : 32; // RGBA32 is the API's legacy alias for RGBA8.
  auto base = std::make_shared<HeifPixelImage>();
  base->create(2, 2, heif_colorspace_RGB, alpha ? heif_chroma_interleaved_RGBA : heif_chroma_interleaved_RGB);
  REQUIRE_FALSE(base->add_channel(heif_channel_interleaved, 2, 2, bits, nullptr,
                                  floating ? heif_component_datatype_floating_point : heif_component_datatype_unsigned_integer));
  auto profile = make_pixels(1, false, 0, 8)->get_color_profile_nclx();
  base->set_color_profile_nclx(profile);
  size_t stride = 0;
  auto* data = base->get_channel_memory(heif_channel_interleaved, &stride);
  const size_t components = alpha ? 4 : 3;
  for (uint32_t y = 0; y < 2; ++y) {
    for (uint32_t x = 0; x < 2; ++x) {
      for (size_t c = 0; c < components; ++c) {
        auto* p = data + size_t(y) * stride + (x * components + c) * static_cast<size_t>(bits / 8);
        const double value = c == 3 ? 0.5 : static_cast<double>(x + y + c + 1) * 0.5;
        if (floating && bits == 32) { const float f = static_cast<float>(value); std::memcpy(p, &f, sizeof(f)); }
        else if (floating) { std::memcpy(p, &value, sizeof(value)); }
        else if (bits == 32) {
          const uint32_t code = c == 3 ? UINT32_MAX / 2 : static_cast<uint32_t>(x + y + c + 1) * 32768U;
          std::memcpy(p, &code, sizeof(code));
        }
        else {
          const uint64_t code = c == 3 ? UINT64_MAX / 2 : uint64_t(x + y + c + 1) << 47;
          std::memcpy(p, &code, sizeof(code));
        }
      }
    }
  }
  GainMapMetadata metadata;
  metadata.channels[0].gain_map_min = {16, 1};
  metadata.channels[0].gain_map_max = {16, 1};
  auto* options = heif_decoding_options_alloc();
  REQUIRE(options);
  profile.set_transfer_characteristics(16);
  // Float values are already HDR; zero weight isolates their preservation.
  const auto target = floating ? std::optional<double>{0.0} : std::nullopt;
  auto output = reconstruct_tone_map(base, make_pixels(2, true, 0, 2), metadata, profile, *options, nullptr,
                                    std::nullopt, target);
  heif_decoding_options_free(options);
  REQUIRE(output);
  for (uint32_t y = 0; y < 2; ++y) {
    for (uint32_t x = 0; x < 2; ++x) {
      for (size_t c = 0; c < 3; ++c) {
        size_t output_stride = 0;
        const auto* plane = (*output)->get_channel_memory<uint16_t>(static_cast<heif_channel>(heif_channel_R + c),
                                                                    &output_stride);
        const double signal = plane[size_t(y) * output_stride / 2 + x] / 65535.0;
        auto linear = gain_map_decode_transfer(signal, 16);
        REQUIRE(linear);
        const double expected = static_cast<double>(x + y + c + 1) *
                                (floating ? 0.5 : 65536 * std::ldexp(1.0, bits == 32 ? 15 : 47) /
                                                 (std::ldexp(1.0, bits) - 1));
        REQUIRE(*linear == Catch::Approx(expected).margin(0.0002));
      }
    }
  }
}

TEST_CASE("Wide limited-range mono normalization uses its own code depth")
{
  const int bits = GENERATE(24, 32, 64);
  auto base = std::make_shared<HeifPixelImage>();
  base->create(3, 1, heif_colorspace_monochrome, heif_chroma_monochrome);
  REQUIRE_FALSE(base->add_channel(heif_channel_Y, 3, 1, bits, nullptr));
  const uint64_t shift = uint64_t{1} << (bits - 8);
  const std::array<uint64_t, 3> codes{16 * shift, 125 * shift, 235 * shift};
  for (size_t x = 0; x < 3; ++x) {
    if (bits <= 32) { base->get_channel_memory<uint32_t>(heif_channel_Y, nullptr)[x] = static_cast<uint32_t>(codes[x]); }
    else { base->get_channel_memory<uint64_t>(heif_channel_Y, nullptr)[x] = codes[x]; }
  }
  auto profile = make_pixels(1, false, 0, 8)->get_color_profile_nclx();
  profile.set_full_range_flag(false);
  base->set_color_profile_nclx(profile);
  auto alternate = profile;
  alternate.set_full_range_flag(true);
  auto* options = heif_decoding_options_alloc();
  REQUIRE(options);
  auto result = reconstruct_tone_map(base, make_pixels(1, true, 0, 2), GainMapMetadata{}, alternate, *options, nullptr);
  REQUIRE(result);
  REQUIRE(sample_at(**result, heif_channel_R, 0) == 0);
  REQUIRE(sample_at(**result, heif_channel_R, 1) == std::round(109.0 / 219 * 65535));
  REQUIRE(sample_at(**result, heif_channel_R, 2) == 65535);
  heif_decoding_options_free(options);
}

TEST_CASE("ST2085 matrix inversion retains precision until amplified gain")
{
  auto base = std::make_shared<HeifPixelImage>();
  base->create(1, 1, heif_colorspace_YCbCr, heif_chroma_444);
  for (auto channel : {heif_channel_Y, heif_channel_Cb, heif_channel_Cr}) {
    REQUIRE_FALSE(base->add_channel(channel, 1, 1, 16, nullptr));
    base->fill_channel(channel, channel == heif_channel_Y ? 1 : 32768);
  }
  auto profile = make_pixels(1, false, 0, 8)->get_color_profile_nclx();
  profile.set_matrix_coefficients(11);
  base->set_color_profile_nclx(profile);
  auto alternate = profile;
  alternate.set_matrix_coefficients(0);
  GainMapMetadata metadata;
  metadata.channels[0].gain_map_max = {15, 1};
  auto* options = heif_decoding_options_alloc();
  REQUIRE(options);
  auto result = reconstruct_tone_map(base, make_pixels(1, true, 65535, 2), metadata, alternate, *options, nullptr);
  REQUIRE(result);
  REQUIRE(sample_at(**result, heif_channel_R, 0) == std::round(32768 * 0.991902));
  REQUIRE(sample_at(**result, heif_channel_G, 0) == 32768);
  REQUIRE(sample_at(**result, heif_channel_B, 0) == std::round(32768 / 0.986566));
  heif_decoding_options_free(options);
}

TEST_CASE("YCbCr extended signals survive matrix inversion until ISO offsets")
{
  const int matrix = GENERATE(1, 6, 9, 11, 12);
  const int transfer = GENERATE(11, 12, 13);
  auto base = std::make_shared<HeifPixelImage>();
  base->create(1, 1, heif_colorspace_YCbCr, heif_chroma_444);
  const std::array<heif_channel, 3> channels{heif_channel_Y, heif_channel_Cb, heif_channel_Cr};
  const std::array<uint16_t, 3> codes{8192, 32768, 24768};
  for (size_t c = 0; c < 3; ++c) {
    REQUIRE_FALSE(base->add_channel(channels[c], 1, 1, 16, nullptr));
    base->fill_channel(channels[c], codes[c]);
  }
  auto profile = make_pixels(1, false, 0, static_cast<uint16_t>(transfer))->get_color_profile_nclx();
  profile.set_matrix_coefficients(static_cast<uint16_t>(matrix));
  base->set_color_profile_nclx(profile);
  auto alternate = profile;
  alternate.set_transfer_characteristics(8);
  alternate.set_matrix_coefficients(0);
  GainMapMetadata metadata;
  metadata.channels[0].base_offset = {1, 4};
  auto* options = heif_decoding_options_alloc();
  REQUIRE(options);
  auto result = reconstruct_tone_map(base, make_pixels(1, true, 0, 2), metadata, alternate, *options, nullptr);
  REQUIRE(result);
  // H.273 Eq.45-47 / ST2085 Eq.76-78, then signed EOTF and ISO Formula (2).
  const double kr = matrix == 6 ? 0.299 : matrix == 9 ? 0.2627 :
                    matrix == 12 ? 0.2126390058715104 : 0.2126;
  const double red = matrix == 11 ? 0.991902 * codes[0] / 65535.0 - 16000.0 / 65535 :
                                  codes[0] / 65535.0 - 16000.0 / 65535 * (1 - kr);
  REQUIRE(red < 0);
  const double v = std::abs(red);
  auto bt = [](double value) {
    return value < 0.081242858298635 ? value / 4.5 :
           std::pow((value + 0.099296826809442) / 1.099296826809442, 1 / 0.45);
  };
  const double linear = transfer == 11 ? -bt(v) : transfer == 12 ? -bt(4 * v) / 4 :
      -(v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4));
  REQUIRE(sample_at(**result, heif_channel_R, 0) == Catch::Approx(std::round((linear + 0.25) * 65535)).margin(1));
  heif_decoding_options_free(options);
}

TEST_CASE("sYCC RGB transfer follows the original non-identity matrix description")
{
  auto profile = make_pixels(1, false, 0, 13)->get_color_profile_nclx();
  profile.set_matrix_coefficients(6);
  auto decoded = gain_map_decode_rgb({-0.5, 0, 0.5}, profile);
  REQUIRE(decoded);
  REQUIRE((*decoded)[0] == Catch::Approx(-0.21404114048223255).epsilon(1e-12));
  auto encoded = gain_map_encode_rgb(*decoded, profile);
  REQUIRE(encoded);
  REQUIRE((*encoded)[0] == Catch::Approx(-0.5).epsilon(1e-12));
  profile.set_matrix_coefficients(0);
  decoded = gain_map_decode_rgb({-0.5, 0, 0.5}, profile);
  REQUIRE(decoded);
  REQUIRE((*decoded)[0] == 0);
}
