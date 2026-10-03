/*
  libheif ISO 21496-1 libultrahdr interoperability tests

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

// Links an independently built upstream implementation, never a copied parser.
#include "catch_amalgamated.hpp"
#include "gain_map_metadata.h"
#include "gain_map_reconstruction.h"
#include "ultrahdr/gainmapmetadata.h"

#include <array>
#include <cmath>
#include <memory>
#include <vector>

TEST_CASE("Final Annex C bytes interoperate with upstream libultrahdr")
{
  for (uint8_t count : {uint8_t{1}, uint8_t{3}}) {
    for (bool use_base : {false, true}) {
      GainMapMetadata ours;
      ultrahdr::uhdr_gainmap_metadata_frac google;
      ours.channel_count = count;
      ours.use_base_colour_space = google.useBaseColorSpace = use_base;
      ours.base_hdr_headroom = {1, 2};
      ours.alternate_hdr_headroom = {7, 2};
      google.baseHdrHeadroomN = 1;
      google.baseHdrHeadroomD = 2;
      google.alternateHdrHeadroomN = 7;
      google.alternateHdrHeadroomD = 2;
      for (uint8_t c = 0; c < 3; ++c) {
        const int n = count == 1 ? 0 : c;
        ours.channels[c].gain_map_min = {-1 - n, 3};
        ours.channels[c].gain_map_max = {5 + n, 2};
        ours.channels[c].gamma = {3, 2};
        ours.channels[c].base_offset = {-1, 64};
        ours.channels[c].alternate_offset = {1, 32};
        google.gainMapMinN[c] = -1 - n;
        google.gainMapMinD[c] = 3;
        google.gainMapMaxN[c] = 5 + n;
        google.gainMapMaxD[c] = 2;
        google.gainMapGammaN[c] = 3;
        google.gainMapGammaD[c] = 2;
        google.baseOffsetN[c] = -1;
        google.baseOffsetD[c] = 64;
        google.alternateOffsetN[c] = 1;
        google.alternateOffsetD[c] = 32;
      }
      auto bytes = serialize_gain_map_metadata(ours);
      REQUIRE(bytes);
      ultrahdr::uhdr_gainmap_metadata_frac decoded;
      REQUIRE(ultrahdr::uhdr_gainmap_metadata_frac::decodeGainmapMetadata(*bytes, &decoded).error_code == UHDR_CODEC_OK);
      REQUIRE(decoded.useBaseColorSpace == use_base);
      REQUIRE(decoded.baseHdrHeadroomN == 1);
      REQUIRE(decoded.alternateHdrHeadroomN == 7);
      for (uint8_t c = 0; c < count; ++c) {
        REQUIRE(decoded.gainMapMinN[c] == google.gainMapMinN[c]);
        REQUIRE(decoded.gainMapMaxN[c] == google.gainMapMaxN[c]);
        REQUIRE(decoded.baseOffsetN[c] == -1);
        REQUIRE(decoded.alternateOffsetD[c] == 32);
      }
      std::vector<uint8_t> google_bytes;
      REQUIRE(ultrahdr::uhdr_gainmap_metadata_frac::encodeGainmapMetadata(&google, google_bytes).error_code == UHDR_CODEC_OK);
      REQUIRE(google_bytes == *bytes);
      auto parsed = parse_gain_map_metadata(google_bytes);
      REQUIRE(parsed.status == GainMapMetadataParseStatus::parsed);
      REQUIRE(parsed.metadata->channel_count == count);
      REQUIRE(*serialize_gain_map_metadata(*parsed.metadata) == google_bytes);
    }
  }
}

TEST_CASE("ISO reconstructed HDR pixels agree with upstream libultrahdr")
{
  const bool mono = GENERATE(false, true);
  const bool use_base = GENERATE(false, true);
  const uint8_t count = GENERATE(uint8_t{1}, uint8_t{3});
  const uint16_t primaries = GENERATE(uint16_t{1}, uint16_t{9}, uint16_t{12});
  const float display_boost = GENERATE(1.0f, 2.0f, 4.0f);
  INFO("mono=" << mono << " channels=" << int(count) << " base space=" << use_base
              << " primaries=" << primaries << " display boost=" << display_boost);
  constexpr uint32_t width = 6, height = 4;
  const std::array<heif_channel, 3> channels{heif_channel_R, heif_channel_G, heif_channel_B};
  std::vector<uint8_t> packed_base(width * height * 4, 255), packed_gain(width * height * (mono ? 1 : 3));
  auto base = std::make_shared<HeifPixelImage>();
  auto gain = std::make_shared<HeifPixelImage>();
  base->create(width, height, heif_colorspace_RGB, heif_chroma_444);
  gain->create(width, height, mono ? heif_colorspace_monochrome : heif_colorspace_RGB,
                mono ? heif_chroma_monochrome : heif_chroma_444);
  nclx_profile baseline;
  baseline.m_colour_primaries = 1;
  baseline.m_transfer_characteristics = 13;
  baseline.m_matrix_coefficients = 0;
  base->set_color_profile_nclx(baseline);
  nclx_profile gain_profile;
  gain_profile.m_matrix_coefficients = 0;
  gain->set_color_profile_nclx(gain_profile);
  for (size_t c = 0; c < 3; ++c) {
    REQUIRE_FALSE(base->add_channel(channels[c], width, height, 8, nullptr));
    size_t stride = 0;
    auto* plane = base->get_channel_memory(channels[c], &stride);
    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        const auto value = static_cast<uint8_t>(48 + ((x * 37 + y * 23 + c * 61) % 180));
        plane[size_t(y) * stride + x] = packed_base[(size_t(y) * width + x) * 4 + c] = value;
      }
    }
  }
  for (size_t c = 0; c < (mono ? 1U : 3U); ++c) {
    const auto channel = mono ? heif_channel_Y : channels[c];
    REQUIRE_FALSE(gain->add_channel(channel, width, height, 8, nullptr));
    size_t stride = 0;
    auto* plane = gain->get_channel_memory(channel, &stride);
    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        // Equal-size rasters compare the gain operation without Google's IDW resampler.
        const auto value = static_cast<uint8_t>(40 + ((x * 19 + y * 47 + c * 53) % 160));
        plane[size_t(y) * stride + x] = packed_gain[(size_t(y) * width + x) * (mono ? 1 : 3) + c] = value;
      }
    }
  }
  GainMapMetadata metadata;
  metadata.channel_count = count;
  metadata.use_base_colour_space = use_base;
  metadata.base_hdr_headroom = {0, 1};
  metadata.alternate_hdr_headroom = {2, 1};
  for (size_t c = 0; c < 3; ++c) {
    auto& channel = metadata.channels[c];
    // Upstream applies one set of parameters to its monochrome gain path.
    const int32_t n = count == 1 || mono ? 0 : static_cast<int32_t>(c);
    channel.gain_map_min = {-1, 2};
    channel.gain_map_max = {4 + n, 2};
    channel.gamma = {3, 2};
    channel.base_offset = {1, 64};
    channel.alternate_offset = {1, 32};
  }
  auto bytes = serialize_gain_map_metadata(metadata);
  REQUIRE(bytes);
  ultrahdr::uhdr_gainmap_metadata_frac fractions;
  REQUIRE(ultrahdr::uhdr_gainmap_metadata_frac::decodeGainmapMetadata(*bytes, &fractions).error_code == UHDR_CODEC_OK);
  ultrahdr::uhdr_gainmap_metadata_ext_t google_metadata(ultrahdr::kJpegrVersion);
  REQUIRE(ultrahdr::uhdr_gainmap_metadata_frac::gainmapMetadataFractionToFloat(&fractions, &google_metadata).error_code ==
          UHDR_CODEC_OK);
  const auto gamut = primaries == 1 ? UHDR_CG_BT_709 : primaries == 9 ? UHDR_CG_BT_2100 : UHDR_CG_DISPLAY_P3;
  // The pinned upstream's isPixelFormatRgb omits RGB888; use its RGBA path.
  uhdr_raw_image_t google_base{UHDR_IMG_FMT_32bppRGBA8888, UHDR_CG_BT_709, UHDR_CT_SRGB,
                              UHDR_CR_FULL_RANGE, width, height, {packed_base.data(), nullptr, nullptr}, {width, 0, 0}};
  uhdr_raw_image_t google_gain{mono ? UHDR_IMG_FMT_8bppYCbCr400 : UHDR_IMG_FMT_24bppRGB888, gamut, UHDR_CT_UNSPECIFIED,
                              UHDR_CR_FULL_RANGE, width, height, {packed_gain.data(), nullptr, nullptr}, {width, 0, 0}};
  std::vector<uint16_t> google_pixels(width * height * 4);
  uhdr_raw_image_t google_output{UHDR_IMG_FMT_64bppRGBAHalfFloat, gamut, UHDR_CT_LINEAR,
                                UHDR_CR_FULL_RANGE, width, height, {google_pixels.data(), nullptr, nullptr}, {width, 0, 0}};
  ultrahdr::UltraHdr decoder;
  const auto error = decoder.applyGainMap(&google_base, &google_gain, &google_metadata, UHDR_CT_LINEAR,
                                          UHDR_IMG_FMT_64bppRGBAHalfFloat, display_boost, &google_output);
  INFO(error.detail);
  REQUIRE(error.error_code == UHDR_CODEC_OK);
  nclx_profile alternate = baseline;
  alternate.m_colour_primaries = primaries;
  alternate.m_transfer_characteristics = 16;
  auto* options = heif_decoding_options_alloc();
  REQUIRE(options);
  auto output = reconstruct_tone_map(base, gain, metadata, GainMapColour(alternate), *options, nullptr,
                                      std::nullopt, std::log2(display_boost));
  heif_decoding_options_free(options);
  REQUIRE(output);
  for (size_t c = 0; c < 3; ++c) {
    size_t stride = 0;
    const auto* plane = (*output)->get_channel_memory<uint16_t>(channels[c], &stride);
    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        const double pq = plane[size_t(y) * stride / sizeof(uint16_t) + x] / 65535.0;
        const uint16_t half = google_pixels[(size_t(y) * width + x) * 4 + c];
        const int exponent = (half >> 10) & 31;
        REQUIRE(exponent != 31);
        double independent = std::ldexp((half & 1023) + (exponent ? 1024 : 0), exponent ? exponent - 25 : -24);
        if (half & 0x8000) { independent = -independent; }
        auto linear = gain_map_decode_transfer(pq, 16);
        REQUIRE(linear);
        INFO("x=" << x << " y=" << y << " c=" << c);
        // Upstream rounds its 10-bit sRGB/gain LUT indices and then writes half.
        // Allow 0.15% of reference white plus 0.3% of the reconstructed value.
        REQUIRE(*linear == Catch::Approx(independent).margin(0.0015 + 0.003 * std::abs(independent)));
      }
    }
  }
}
