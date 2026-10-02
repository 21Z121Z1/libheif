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

TEST_CASE("Tone maps interpret FCC and SMPTE240 raster matrices for both inputs")
{
  const uint16_t matrix = GENERATE(uint16_t{4}, uint16_t{7});
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
  const double kr = matrix == 4 ? 0.30 : 0.212;
  const double kb = matrix == 4 ? 0.11 : 0.087;
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
  profile.set_matrix_coefficients(10); // CL must not silently use the NCL matrix.
  colour->set_color_profile_nclx(profile);
  result = reconstruct_tone_map(base, gain, metadata, alternate, *options, nullptr);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().error_code == heif_error_Unsupported_feature);
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
