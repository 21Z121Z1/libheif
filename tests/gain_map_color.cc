/*
  libheif ICC colour bridge tests for ISO 21496-1 tone-map reconstruction

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
#include "gain_map_color.h"
#include "gain_map_reconstruction.h"
#include "libheif/heif_components.h"
#include "libheif/heif_experimental.h"
#include "libheif/heif_items.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#if LIBHEIF_TEST_LCMS
#include <lcms2.h>
#endif

namespace {

uint32_t signature(const char* text)
{
  return (static_cast<uint32_t>(static_cast<uint8_t>(text[0])) << 24) |
         (static_cast<uint32_t>(static_cast<uint8_t>(text[1])) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(text[2])) << 8) |
         static_cast<uint32_t>(static_cast<uint8_t>(text[3]));
}

void put_u16(std::vector<uint8_t>& data, uint16_t value)
{
  data.push_back(static_cast<uint8_t>(value >> 8));
  data.push_back(static_cast<uint8_t>(value));
}

void put_u32(std::vector<uint8_t>& data, uint32_t value)
{
  data.push_back(static_cast<uint8_t>(value >> 24));
  data.push_back(static_cast<uint8_t>(value >> 16));
  data.push_back(static_cast<uint8_t>(value >> 8));
  data.push_back(static_cast<uint8_t>(value));
}

void set_u32(std::vector<uint8_t>& data, size_t offset, uint32_t value)
{
  REQUIRE(offset + 4 <= data.size());
  data[offset] = static_cast<uint8_t>(value >> 24);
  data[offset + 1] = static_cast<uint8_t>(value >> 16);
  data[offset + 2] = static_cast<uint8_t>(value >> 8);
  data[offset + 3] = static_cast<uint8_t>(value);
}

void put_fixed(std::vector<uint8_t>& data, double value)
{
  const int64_t fixed = std::llround(value * 65536.0);
  put_u32(data, static_cast<uint32_t>(fixed));
}

std::vector<uint8_t> xyz_tag(const std::array<double, 3>& xyz)
{
  std::vector<uint8_t> data;
  put_u32(data, signature("XYZ "));
  put_u32(data, 0);
  for (double value : xyz) {
    put_fixed(data, value);
  }
  return data;
}

std::vector<uint8_t> chad_tag(const std::array<std::array<double, 3>, 3>& matrix)
{
  std::vector<uint8_t> data;
  put_u32(data, signature("sf32"));
  put_u32(data, 0);
  for (const auto& row : matrix) {
    for (double value : row) {
      put_fixed(data, value);
    }
  }
  return data;
}

std::vector<uint8_t> srgb_trc_tag()
{
  std::vector<uint8_t> data;
  put_u32(data, signature("para"));
  put_u32(data, 0);
  put_u16(data, 3);
  put_u16(data, 0);
  for (double value : {2.4, 1.0 / 1.055, 0.055 / 1.055,
                       1.0 / 12.92, 0.04045}) {
    put_fixed(data, value);
  }
  return data;
}

std::vector<uint8_t> sampled_trc_tag(const std::vector<uint16_t>& samples)
{
  std::vector<uint8_t> data;
  put_u32(data, signature("curv"));
  put_u32(data, 0);
  put_u32(data, static_cast<uint32_t>(samples.size()));
  for (uint16_t value : samples) { put_u16(data, value); }
  return data;
}

std::vector<uint8_t> cicp_tag(uint8_t primaries, uint8_t transfer,
                              uint8_t matrix, uint8_t full_range)
{
  std::vector<uint8_t> data;
  put_u32(data, signature("cicp"));
  put_u32(data, 0);
  data.push_back(primaries);
  data.push_back(transfer);
  data.push_back(matrix);
  data.push_back(full_range);
  return data;
}

struct TestTag
{
  uint32_t signature;
  std::vector<uint8_t> payload;
};

std::vector<uint8_t> make_icc(const std::vector<TestTag>& tags)
{
  std::vector<uint8_t> data(132 + tags.size() * 12, 0);
  set_u32(data, 12, signature("mntr"));
  set_u32(data, 16, signature("RGB "));
  set_u32(data, 20, signature("XYZ "));
  set_u32(data, 36, signature("acsp"));
  set_u32(data, 128, static_cast<uint32_t>(tags.size()));

  for (size_t i = 0; i < tags.size(); ++i) {
    while ((data.size() & 3U) != 0) {
      data.push_back(0);
    }
    const uint32_t offset = static_cast<uint32_t>(data.size());
    data.insert(data.end(), tags[i].payload.begin(), tags[i].payload.end());
    const size_t entry = 132 + i * 12;
    set_u32(data, entry, tags[i].signature);
    set_u32(data, entry + 4, offset);
    set_u32(data, entry + 8, static_cast<uint32_t>(tags[i].payload.size()));
  }
  set_u32(data, 0, static_cast<uint32_t>(data.size()));
  return data;
}

std::array<std::array<double, 3>, 3> multiply(
    const std::array<std::array<double, 3>, 3>& a,
    const std::array<std::array<double, 3>, 3>& b)
{
  std::array<std::array<double, 3>, 3> result{};
  for (size_t y = 0; y < 3; ++y) {
    for (size_t x = 0; x < 3; ++x) {
      for (size_t k = 0; k < 3; ++k) {
        result[y][x] += a[y][k] * b[k][x];
      }
    }
  }
  return result;
}

}  // namespace

TEST_CASE("Gray ICC shapers convert through PCS into the alternate RGB application space")
{
  const bool lab = GENERATE(false, true);
  auto bytes = make_icc({{signature("kTRC"), sampled_trc_tag({512})}});
  set_u32(bytes, 16, signature("GRAY"));
  set_u32(bytes, 20, signature(lab ? "Lab " : "XYZ "));
  const auto profile = std::make_shared<color_profile_raw>(signature("prof"), bytes);
  auto gray = GainMapColour::from_icc(profile);
  REQUIRE(gray);
  REQUIRE_FALSE(gray->has_application_primaries());
  REQUIRE(gray->icc_profile() == profile);
  nclx_profile nclx;
  nclx.set_colour_primaries(9);
  nclx.set_transfer_characteristics(8);
  const GainMapColour alternate(nclx);
  auto matrix = gray->matrix_to(alternate);
  REQUIRE(matrix);
  for (double sample : {0.0, 0.1, 0.5, 1.0}) {
    auto pcs = gray->decode({sample, sample, sample});
    REQUIRE(pcs);
    // ICC.1:2022 F.2: kTRC gives relative Y or normalized L*. Invert
    // neutral Lab independently before applying the known PCS white.
    const double connection = sample * sample;
    const double luminance = !lab ? connection : connection <= 0.08 ?
        connection * 2700 / 24389 : std::pow((100 * connection + 16) / 116, 3);
    const std::array<double, 3> white{0.9642, 1, 0.8249};
    const auto rgb = gain_map_transform(*matrix, *pcs);
    for (size_t c = 0; c < 3; ++c) {
      REQUIRE((*pcs)[c] == Catch::Approx(luminance * white[c]).margin(1e-12));
      REQUIRE(rgb[c] == Catch::Approx(luminance).margin(1e-10));
    }
  }
  REQUIRE_FALSE(gray->decode({0.2, 0.3, 0.2}));
  REQUIRE_FALSE(gray->decode({NAN, NAN, NAN}));
  auto encoded_gray = gray->encode({0.9642 * 0.25, 0.25, 0.8249 * 0.25});
  REQUIRE(encoded_gray);
  const double encoded_connection = lab ? (116 * std::cbrt(0.25) - 16) / 100 : 0.25;
  for (double value : *encoded_gray) { REQUIRE(value == Catch::Approx(std::sqrt(encoded_connection)).margin(1e-12)); }
  auto base = std::make_shared<HeifPixelImage>();
  base->create(1, 1, heif_colorspace_monochrome, heif_chroma_monochrome);
  REQUIRE_FALSE(base->add_channel(heif_channel_Y, 1, 1, 8, nullptr));
  base->fill_channel(heif_channel_Y, 127);
  auto gain = std::make_shared<HeifPixelImage>();
  gain->create(1, 1, heif_colorspace_monochrome, heif_chroma_monochrome);
  REQUIRE_FALSE(gain->add_channel(heif_channel_Y, 1, 1, 8, nullptr));
  gain->fill_channel(heif_channel_Y, 255);
  GainMapMetadata metadata;
  metadata.use_base_colour_space = false;
  metadata.channel_count = 3;
  for (size_t c = 0; c < 3; ++c) {
    metadata.channels[c].gain_map_min = {static_cast<int32_t>(c), 1};
    metadata.channels[c].gain_map_max = {static_cast<int32_t>(c), 1};
  }
  auto* options = heif_decoding_options_alloc();
  REQUIRE(options);
  auto result = reconstruct_tone_map(base, gain, metadata, alternate, *options, nullptr, *gray,
                                    std::nullopt, true);
  REQUIRE(result);
  const double connection = std::pow(127.0 / 255, 2);
  const double luminance = lab ? std::pow((connection * 100 + 16) / 116, 3) : connection;
  for (size_t c = 0; c < 3; ++c) {
    const auto channel = static_cast<heif_channel>(heif_channel_R + c);
    REQUIRE((*result)->get_channel_memory<float>(channel, nullptr)[0] ==
            Catch::Approx(luminance * std::exp2(double(c))).margin(1e-6));
  }
  metadata.use_base_colour_space = true;
  REQUIRE_FALSE(reconstruct_tone_map(base, gain, metadata, alternate, *options, nullptr, *gray));
  heif_decoding_options_free(options);
  auto missing = make_icc({});
  set_u32(missing, 16, signature("GRAY"));
  REQUIRE_FALSE(GainMapColour::from_icc(std::make_shared<color_profile_raw>(signature("prof"), missing)));
  auto bad = make_icc({{signature("kTRC"), sampled_trc_tag({0, 32768, 16384, 65535})}});
  set_u32(bad, 16, signature("GRAY"));
  REQUIRE_FALSE(GainMapColour::from_icc(std::make_shared<color_profile_raw>(signature("prof"), bad)));
}

TEST_CASE("Gray ICC alternates retain their actual mono encoding and opacity")
{
  const bool lab = GENERATE(false, true);
  const bool floating = GENERATE(false, true);
  const bool premultiplied = GENERATE(false, true);
  const double target = GENERATE(0.0, 1.0, 2.0);
  auto bytes = make_icc({{signature("kTRC"), sampled_trc_tag({512})}});
  set_u32(bytes, 16, signature("GRAY"));
  set_u32(bytes, 20, signature(lab ? "Lab " : "XYZ "));
  auto gray = GainMapColour::from_icc(std::make_shared<color_profile_raw>(signature("prof"), bytes));
  REQUIRE(gray);
  auto base = std::make_shared<HeifPixelImage>();
  base->create(2, 2, heif_colorspace_RGB, heif_chroma_444);
  nclx_profile nclx;
  nclx.set_colour_primaries(9);
  nclx.set_transfer_characteristics(8);
  nclx.set_matrix_coefficients(0);
  nclx.set_full_range_flag(true);
  base->set_color_profile_nclx(nclx);
  for (auto channel : {heif_channel_R, heif_channel_G, heif_channel_B, heif_channel_Alpha}) {
    REQUIRE_FALSE(base->add_channel(channel, 2, 2, 32, nullptr, heif_component_datatype_floating_point));
    size_t stride = 0;
    auto* plane = base->get_channel_memory<float>(channel, &stride);
    const float value = channel == heif_channel_Alpha || !premultiplied ? 0.5f : 0.25f;
    for (size_t y = 0; y < 2; ++y) { std::fill_n(plane + y * stride / sizeof(float), 2, value); }
  }
  base->set_premultiplied_alpha(premultiplied);
  auto gain = std::make_shared<HeifPixelImage>();
  gain->create(1, 1, heif_colorspace_monochrome, heif_chroma_monochrome);
  REQUIRE_FALSE(gain->add_channel(heif_channel_Y, 1, 1, 8, nullptr));
  gain->get_channel_memory(heif_channel_Y, nullptr)[0] = 255;
  GainMapMetadata metadata;
  metadata.use_base_colour_space = true;
  metadata.alternate_hdr_headroom = {2, 1};
  metadata.channels[0].gain_map_min = {2, 1};
  metadata.channels[0].gain_map_max = {2, 1};
  auto* options = heif_decoding_options_alloc();
  REQUIRE(options);
  auto result = reconstruct_tone_map(base, gain, metadata, *gray, *options, nullptr,
                                    std::nullopt, target, floating);
  REQUIRE(result);
  REQUIRE((*result)->get_colorspace() == heif_colorspace_monochrome);
  REQUIRE((*result)->get_chroma_format() == heif_chroma_monochrome);
  REQUIRE((*result)->get_color_profile_icc()->get_data() == bytes);
  REQUIRE((*result)->is_premultiplied_alpha() == premultiplied);
  const double luminance = 0.5 * std::exp2(target);
  const double connection = lab ? (116 * std::cbrt(luminance) - 16) / 100 : luminance;
  const double encoded = std::sqrt(connection);
  const double stored = encoded * (premultiplied ? 0.5 : 1);
  const double expected = floating ? stored : std::clamp(stored, 0.0, 1.0);
  size_t stride = 0;
  const auto* plane = (*result)->get_channel_memory(heif_channel_Y, &stride);
  for (size_t y = 0; y < 2; ++y) {
    for (size_t x = 0; x < 2; ++x) {
      const auto* row = plane + y * stride;
      const double actual = floating ? reinterpret_cast<const float*>(row)[x] :
                                      reinterpret_cast<const uint16_t*>(row)[x] / 65535.0;
      REQUIRE(actual == Catch::Approx(expected).margin(floating ? 1e-6 : 1.0 / 65535));
    }
  }
  metadata.use_base_colour_space = false;
  REQUIRE_FALSE(reconstruct_tone_map(base, gain, metadata, *gray, *options, nullptr));
  heif_decoding_options_free(options);
}

TEST_CASE("Public NCLX APIs preserve the defined H.273 matrix code points")
{
  const uint16_t matrix = GENERATE(uint16_t{15}, uint16_t{16}, uint16_t{17});
  auto* profile = heif_nclx_color_profile_alloc();
  REQUIRE(profile);
  const auto set = heif_nclx_color_profile_set_matrix_coefficients(profile, matrix);
  CHECK(set.code == heif_error_Ok);
  CHECK(static_cast<uint16_t>(profile->matrix_coefficients) == matrix);
  heif_nclx_color_profile_free(profile);
  nclx_profile stored;
  stored.set_colour_primaries(9);
  stored.set_transfer_characteristics(16);
  stored.set_matrix_coefficients(matrix);
  profile = nullptr;
  const auto get = stored.get_nclx_color_profile(&profile);
  CHECK_FALSE(get);
  if (profile) { CHECK(static_cast<uint16_t>(profile->matrix_coefficients) == matrix); }
  heif_nclx_color_profile_free(profile);
}

TEST_CASE("Public NCLX matrix setters keep reserved code points unsupported")
{
  const uint16_t matrix = GENERATE(uint16_t{3}, uint16_t{18}, uint16_t{255}, uint16_t{65535});
  auto* profile = heif_nclx_color_profile_alloc();
  REQUIRE(profile);
  const auto error = heif_nclx_color_profile_set_matrix_coefficients(profile, matrix);
  CHECK(error.code == heif_error_Invalid_input);
  CHECK(error.subcode == heif_suberror_Unknown_NCLX_matrix_coefficients);
  CHECK(profile->matrix_coefficients == heif_matrix_coefficients_unspecified);
  heif_nclx_color_profile_free(profile);
}

TEST_CASE("ICC CICP tag maps Display-P3 PQ directly")
{
  const auto bytes = make_icc({
      {signature("cicp"), cicp_tag(12, 16, 0, 1)}
  });
  const auto profile = std::make_shared<color_profile_raw>(signature("prof"), bytes);
  auto result = GainMapColour::from_icc(profile);
  REQUIRE(result);
  CHECK(result->raster_profile().m_colour_primaries == 12);
  CHECK(result->raster_profile().m_transfer_characteristics == 16);
  CHECK(result->raster_profile().m_matrix_coefficients == 0);
  CHECK(result->raster_profile().m_full_range_flag);
  CHECK(result->icc_profile() == profile);
}

TEST_CASE("ICC CICP rejects profile classes forbidden by ICC 9.2.17")
{
  const auto profile_class = GENERATE("prtr", "spac", "link", "abst", "nmcl");
  INFO(profile_class);
  auto bytes = make_icc({{signature("cicp"), cicp_tag(12, 16, 0, 1)}});
  set_u32(bytes, 12, signature(profile_class));
  const auto profile = std::make_shared<color_profile_raw>(signature("prof"), bytes);
  auto colour = GainMapColour::from_icc(profile);
  REQUIRE_FALSE(colour);
  CHECK(colour.error().error_code == heif_error_Invalid_input);
}

TEST_CASE("ICC CICP XYZ encoding retains the declared ST 428 colour transform")
{
  const auto profile_class = GENERATE("mntr", "scnr");
  auto bytes = make_icc({{signature("cicp"), cicp_tag(10, 17, 0, 1)}});
  set_u32(bytes, 12, signature(profile_class));
  set_u32(bytes, 16, signature("XYZ "));
  const auto profile = std::make_shared<color_profile_raw>(signature("prof"), bytes);
  auto colour = GainMapColour::from_icc(profile);
  REQUIRE(colour);
  CHECK(colour->raster_profile().m_colour_primaries == 10);
  CHECK(colour->raster_profile().m_transfer_characteristics == 17);
  CHECK(colour->raster_profile().m_matrix_coefficients == 0);
  CHECK(colour->has_application_primaries());
  CHECK(colour->icc_profile() == profile);
  const GainMapRGB signal{0.2, 0.4, 0.6};
  auto linear = colour->decode(signal);
  REQUIRE(linear);
  for (size_t c = 0; c < 3; ++c) {
    // H.273 Table 3: ST 428 decodes to absolute XYZ luminance, then
    // ISO reference white is normalized to 203 cd/m2.
    CHECK((*linear)[c] == Catch::Approx(std::pow(signal[c], 2.6) * 52.37 / 203).margin(1e-12));
  }
  auto encoded = colour->encode(*linear);
  REQUIRE(encoded);
  for (size_t c = 0; c < 3; ++c) {
    CHECK((*encoded)[c] == Catch::Approx(signal[c]).margin(1e-12));
  }
}

TEST_CASE("ICC CICP validates the profile data space and its matrix")
{
  const auto data_space = GENERATE("RGB ", "XYZ ", "YCbr", "GRAY", "CMYK", "Lab ");
  const bool rgb_or_xyz = signature(data_space) == signature("RGB ") ||
                          signature(data_space) == signature("XYZ ");
  auto bytes = make_icc({{signature("cicp"), cicp_tag(1, 13, rgb_or_xyz ? 1 : 0, 1)}});
  set_u32(bytes, 16, signature(data_space));
  const auto profile = std::make_shared<color_profile_raw>(signature("prof"), bytes);
  auto colour = GainMapColour::from_icc(profile);
  REQUIRE_FALSE(colour);
  CHECK(colour.error().error_code == heif_error_Invalid_input);
}

TEST_CASE("YCbCr ICC CICP reconstructs through declared RGB colourimetry")
{
  const int matrix = GENERATE(0, 9, 10, 12, 13, 14, 15);
  const int bits = GENERATE(8, 10, 12, 16);
  const bool missing_storage = GENERATE(false, true);
  const bool use_base = GENERATE(false, true);
  const bool icc_alternate = GENERATE(false, true);
  const double headroom = GENERATE(0.0, 1.0, 2.0);
  auto bytes = make_icc({{signature("cicp"), cicp_tag(9, 16, static_cast<uint8_t>(matrix), 0)}});
  if (matrix != 0) { set_u32(bytes, 16, signature("YCbr")); }
  const auto profile = std::make_shared<color_profile_raw>(signature("prof"), bytes);
  auto baseline = GainMapColour::from_icc(profile);
  REQUIRE(baseline);
  CHECK(baseline->icc_profile() == profile);
  auto base = std::make_shared<HeifPixelImage>();
  base->create(2, 2, matrix == 0 ? heif_colorspace_RGB : heif_colorspace_YCbCr, heif_chroma_444);
  const auto channels = matrix == 0 ? std::array{heif_channel_R, heif_channel_G, heif_channel_B} :
                                     std::array{heif_channel_Y, heif_channel_Cb, heif_channel_Cr};
  const auto codewords = matrix == 0 || matrix == 9 ? std::array<uint8_t, 3>{128, 96, 160} :
                                                    std::array<uint8_t, 3>{128, 128, 128};
  for (size_t c = 0; c < 3; ++c) {
    REQUIRE_FALSE(base->add_channel(channels[c], 2, 2, bits, nullptr));
    size_t stride = 0;
    auto* plane = base->get_channel_memory(channels[c], &stride);
    for (size_t y = 0; y < 2; ++y) {
      if (bits == 8) { std::fill_n(plane + y * stride, 2, codewords[c]); }
      else {
        std::fill_n(reinterpret_cast<uint16_t*>(plane + y * stride), 2,
                    static_cast<uint16_t>(codewords[c] << (bits - 8)));
      }
    }
  }
  nclx_profile storage = nclx_profile::undefined();
  storage.set_matrix_coefficients(static_cast<uint16_t>(matrix));
  storage.set_full_range_flag(false);
  if (!missing_storage) { base->set_color_profile_nclx(storage); }
  base->set_color_profile_icc(profile);
  auto gain = std::make_shared<HeifPixelImage>();
  gain->create(1, 1, heif_colorspace_monochrome, heif_chroma_monochrome);
  REQUIRE_FALSE(gain->add_channel(heif_channel_Y, 1, 1, 8, nullptr));
  size_t stride = 0;
  gain->get_channel_memory(heif_channel_Y, &stride)[0] = 128;
  GainMapMetadata metadata;
  metadata.channel_count = 3;
  metadata.use_base_colour_space = use_base;
  metadata.alternate_hdr_headroom = {2, 1};
  for (size_t c = 0; c < 3; ++c) {
    metadata.channels[c].gain_map_max = {static_cast<int32_t>(c + 1), 1};
    metadata.channels[c].gamma = {3, 2};
    metadata.channels[c].base_offset = {1, 64};
    metadata.channels[c].alternate_offset = {1, 128};
  }
  nclx_profile linear;
  linear.set_colour_primaries(9);
  linear.set_transfer_characteristics(8);
  linear.set_matrix_coefficients(0);
  linear.set_full_range_flag(true);
  const GainMapColour alternate = icc_alternate ? *baseline : GainMapColour(linear);
  const auto options = std::unique_ptr<heif_decoding_options, decltype(&heif_decoding_options_free)>(
      heif_decoding_options_alloc(), heif_decoding_options_free);
  REQUIRE(options);
  auto result = reconstruct_tone_map(base, gain, metadata, alternate, *options,
                                     nullptr, *baseline, headroom, true);
  REQUIRE(result);
  const double y = (128.0 - 16) / 219;
  const double cb = (double(codewords[1]) - 128) / 224;
  const double cr = (double(codewords[2]) - 128) / 224;
  const double red = y + 2 * (1 - 0.2627) * cr;
  const double blue = y + 2 * (1 - 0.0593) * cb;
  const GainMapRGB rgb = matrix == 0 ? GainMapRGB{y, (96.0 - 16) / 219, (160.0 - 16) / 219} :
      GainMapRGB{red, (y - 0.2627 * red - 0.0593 * blue) / (1 - 0.2627 - 0.0593), blue};
  for (size_t c = 0; c < 3; ++c) {
    const double p = std::pow(rgb[c], 32.0 / 2523);
    const double decoded = std::pow(std::max(p - 3424.0 / 4096, 0.0) /
        (2413.0 / 128 - 2392.0 / 128 * p), 16384.0 / 2610) * 10000 / 203;
    const double log_gain = static_cast<double>(c + 1) * std::pow(128.0 / 255, 2.0 / 3);
    double expected = (decoded + 1.0 / 64) * std::exp2(headroom / 2 * log_gain) - 1.0 / 128;
    if (icc_alternate) {
      const double q = std::pow(expected * 203 / 10000, 2610.0 / 16384);
      expected = std::pow((3424.0 / 4096 + 2413.0 / 128 * q) /
          (1 + 2392.0 / 128 * q), 2523.0 / 32);
    }
    const auto* plane = (*result)->get_channel_memory<float>(static_cast<heif_channel>(heif_channel_R + c), &stride);
    REQUIRE(plane);
    CHECK(plane[0] == Catch::Approx(expected).margin(2e-6));
  }
  CHECK((*result)->get_color_profile_nclx().m_colour_primaries == 9);
  CHECK((*result)->get_color_profile_nclx().m_transfer_characteristics == (icc_alternate ? 16 : 8));
  CHECK((*result)->get_color_profile_nclx().m_matrix_coefficients == 0);
  CHECK((*result)->get_color_profile_nclx().get_full_range_flag());
  // The item's ICC bytes describe YCbCr narrow range, while reconstructed
  // samples are normalized RGB. Its original profile stays on the item.
  CHECK_FALSE((*result)->get_color_profile_icc());
}

TEST_CASE("Nominal diffuse white sets the HDR reference normalization")
{
  nclx_profile pq;
  pq.set_colour_primaries(1);
  pq.set_transfer_characteristics(16);
  const GainMapColour colour(pq, 1000000); // 100 cd/m2, so 100 cd/m2 is unity.
  auto linear = colour.decode({0.508078421517399, 0.508078421517399, 0.508078421517399});
  REQUIRE(linear);
  auto encoded = colour.encode({1, 1, 1});
  REQUIRE(encoded);
  for (size_t c = 0; c < 3; ++c) {
    REQUIRE((*linear)[c] == Catch::Approx(1).margin(1e-10));
    REQUIRE((*encoded)[c] == Catch::Approx(0.508078421517399).margin(1e-10));
  }
  const auto transfer = GENERATE(uint16_t{13}, uint16_t{16}, uint16_t{17}, uint16_t{18});
  pq.set_transfer_characteristics(transfer);
  const GainMapRGB signal{0.25, 0.5, 0.75};
  auto original = GainMapColour(pq).decode(signal);
  auto explicit_default = GainMapColour(pq, 2030000).decode(signal);
  auto changed = GainMapColour(pq, 1000000).decode(signal);
  REQUIRE(original);
  REQUIRE(explicit_default);
  REQUIRE(changed);
  auto round_trip = GainMapColour(pq, 1000000).encode(*changed);
  REQUIRE(round_trip);
  for (size_t c = 0; c < 3; ++c) {
    REQUIRE((*original)[c] == (*explicit_default)[c]);
    REQUIRE((*changed)[c] == Catch::Approx((*original)[c] * (transfer == 13 ? 1 : 2.03)).margin(1e-10));
    REQUIRE((*round_trip)[c] == Catch::Approx(signal[c]).margin(1e-10));
  }
}

TEST_CASE("ICC matrix-shaper Display-P3 retains its quantized colourants and TRC")
{
  const std::array<std::array<double, 3>, 3> p3{{
      {0.4865709486, 0.2656676932, 0.1982172852},
      {0.2289745641, 0.6917385218, 0.0792869141},
      {0.0, 0.0451133819, 1.0439443689}}};
  const std::array<std::array<double, 3>, 3> d65_to_d50{{
      {1.04788208, 0.02291870, -0.05020142},
      {0.02958679, 0.99047852, -0.01705933},
      {-0.00923157, 0.01507568, 0.75167847}}};
  const auto adapted = multiply(d65_to_d50, p3);
  const auto trc = srgb_trc_tag();

  const auto bytes = make_icc({
      {signature("rXYZ"), xyz_tag({adapted[0][0], adapted[1][0], adapted[2][0]})},
      {signature("gXYZ"), xyz_tag({adapted[0][1], adapted[1][1], adapted[2][1]})},
      {signature("bXYZ"), xyz_tag({adapted[0][2], adapted[1][2], adapted[2][2]})},
      {signature("chad"), chad_tag(d65_to_d50)},
      {signature("rTRC"), trc},
      {signature("gTRC"), trc},
      {signature("bTRC"), trc}
  });

  const auto profile = std::make_shared<color_profile_raw>(signature("prof"), bytes);
  auto colour = GainMapColour::from_icc(profile);
  REQUIRE(colour);
  CHECK(colour->icc_profile() == profile);
  CHECK(colour->has_application_primaries());
  CHECK(colour->raster_profile().m_colour_primaries == 2);
  CHECK(colour->raster_profile().m_transfer_characteristics == 2);
  CHECK(colour->raster_profile().m_matrix_coefficients == 0);
  CHECK(colour->raster_profile().m_full_range_flag);

  const auto quantize = [](double value) { return std::round(value * 65536) / 65536; };
  const GainMapRGB signal{0.01, 0.25, 0.75};
  auto linear = colour->decode(signal);
  REQUIRE(linear);
  for (size_t c = 0; c < 3; c++) {
    const double expected = signal[c] >= quantize(0.04045) ?
        std::pow(quantize(1.0 / 1.055) * signal[c] + quantize(0.055 / 1.055), quantize(2.4)) :
        quantize(1.0 / 12.92) * signal[c];
    CHECK((*linear)[c] == Catch::Approx(expected).margin(1e-12));
  }
  auto encoded = colour->encode(*linear);
  REQUIRE(encoded);
  for (size_t c = 0; c < 3; c++) {
    CHECK((*encoded)[c] == Catch::Approx(signal[c]).margin(1e-12));
  }

  // Gray uses D50 PCS coordinates, exposing the stored colourant matrix here.
  auto gray_bytes = make_icc({{signature("kTRC"), sampled_trc_tag({256})}});
  set_u32(gray_bytes, 16, signature("GRAY"));
  auto gray = GainMapColour::from_icc(std::make_shared<color_profile_raw>(signature("prof"), gray_bytes));
  REQUIRE(gray);
  auto to_pcs = colour->matrix_to(*gray);
  REQUIRE(to_pcs);
  for (size_t row = 0; row < 3; row++) {
    for (size_t c = 0; c < 3; c++) {
      CHECK((*to_pcs)[row][c] == Catch::Approx(quantize(adapted[row][c])).margin(1e-12));
    }
  }
}

TEST_CASE("ICC tag bounds are checked before colour interpretation")
{
  auto bytes = make_icc({
      {signature("cicp"), cicp_tag(12, 16, 0, 1)}
  });
  set_u32(bytes, 136, 0xFFFFFF00U);
  const auto profile = std::make_shared<color_profile_raw>(signature("prof"), bytes);
  auto result = GainMapColour::from_icc(profile);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().error_code == heif_error_Invalid_input);
}

TEST_CASE("Arbitrary ICC LUT profile is not guessed as CICP")
{
  std::vector<uint8_t> lut(32, 0);
  set_u32(lut, 0, signature("mAB "));
  const auto bytes = make_icc({
      {signature("A2B0"), lut}
  });
  const auto profile = std::make_shared<color_profile_raw>(signature("prof"), bytes);
  auto result = GainMapColour::from_icc(profile);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().error_code == heif_error_Unsupported_feature);
  REQUIRE(result.error().sub_error_code ==
          heif_suberror_Unsupported_color_conversion);
}

namespace {
std::shared_ptr<const color_profile_raw> matrix_profile(const std::array<std::vector<uint8_t>, 3>& curves)
{
  // Deliberately not one of the supported CICP RGB matrices. Its colourants
  // sum to the ICC D50 PCS white (up to the stored s15Fixed16 precision).
  return std::make_shared<color_profile_raw>(signature("prof"), make_icc({
      {signature("rXYZ"), xyz_tag({0.5, 0.25, 0.02})},
      {signature("gXYZ"), xyz_tag({0.3, 0.65, 0.15})},
      {signature("bXYZ"), xyz_tag({0.1642, 0.1, 0.6549})},
      {signature("rTRC"), curves[0]},
      {signature("gTRC"), curves[1]},
      {signature("bTRC"), curves[2]}
  }));
}
}  // namespace

TEST_CASE("ICC reconstruction retains custom primaries and unequal channel TRCs")
{
  const auto profile = matrix_profile({sampled_trc_tag({256}), sampled_trc_tag({512}), sampled_trc_tag({768})});
  auto colour = GainMapColour::from_icc(profile);
  REQUIRE(colour);
  auto decoded = colour->decode({0.5, 0.5, 0.5});
  REQUIRE(decoded);
  REQUIRE((*decoded)[0] == 0.5);
  REQUIRE((*decoded)[1] == 0.25);
  REQUIRE((*decoded)[2] == 0.125);
  auto encoded = colour->encode(*decoded);
  REQUIRE(encoded);
  for (double value : *encoded) { REQUIRE(value == Catch::Approx(0.5).margin(1e-9)); }
  REQUIRE(colour->raster_profile().m_colour_primaries == 2);
  REQUIRE(colour->raster_profile().m_transfer_characteristics == 2);
  REQUIRE(colour->icc_profile() == profile);

  auto base = std::make_shared<HeifPixelImage>();
  base->create(1, 1, heif_colorspace_RGB, heif_chroma_444);
  for (auto channel : {heif_channel_R, heif_channel_G, heif_channel_B}) {
    REQUIRE_FALSE(base->add_channel(channel, 1, 1, 16, nullptr));
    base->fill_channel(channel, 32768);
  }
  auto gain = std::make_shared<HeifPixelImage>();
  gain->create(1, 1, heif_colorspace_monochrome, heif_chroma_monochrome);
  REQUIRE_FALSE(gain->add_channel(heif_channel_Y, 1, 1, 8, nullptr));
  gain->fill_channel(heif_channel_Y, 255);
  GainMapMetadata metadata;
  metadata.channels[0].gain_map_min = {1, 1};
  metadata.channels[0].gain_map_max = {1, 1};
  metadata.channels[0].base_offset = {1, 8};
  metadata.channels[0].alternate_offset = {1, 16};
  const auto alternate_profile = matrix_profile({sampled_trc_tag({512}), sampled_trc_tag({512}), sampled_trc_tag({512})});
  auto alternate = GainMapColour::from_icc(alternate_profile);
  REQUIRE(alternate);
  const bool use_base = GENERATE(false, true);
  metadata.use_base_colour_space = use_base;
  auto* options = heif_decoding_options_alloc();
  REQUIRE(options);
  auto output = reconstruct_tone_map(base, gain, metadata, *alternate, *options, nullptr, *colour);
  REQUIRE(output);
  const std::array<heif_channel, 3> channels{heif_channel_R, heif_channel_G, heif_channel_B};
  for (size_t c = 0; c < 3; ++c) {
    const double linear = (std::pow(32768.0 / 65535, static_cast<double>(c + 1)) + 0.125) * 2 - 0.0625;
    const auto sample = (*output)->get_channel_memory<uint16_t>(channels[c], nullptr)[0];
    REQUIRE(sample == Catch::Approx(std::sqrt(std::min(linear, 1.0)) * 65535).margin(1));
  }
  REQUIRE((*output)->get_color_profile_icc() == alternate_profile);
  heif_color_profile_nclx requested{};
  requested.color_primaries = heif_color_primaries_ITU_R_BT_2020_2_and_2100_0;
  requested.transfer_characteristics = heif_transfer_characteristic_linear;
  requested.matrix_coefficients = heif_matrix_coefficients_RGB_GBR;
  requested.full_range_flag = 1;
  auto converted = convert_tone_map_colour(*output, requested, *options, nullptr);
  REQUIRE(converted);
  REQUIRE_FALSE((*converted)->get_color_profile_icc());
  REQUIRE((*output)->get_color_profile_icc() == alternate_profile);
  REQUIRE((*converted)->get_color_profile_nclx().m_transfer_characteristics == 8);
  heif_decoding_options_free(options);
}

TEST_CASE("Sampled ICC curves use interpolation and normative plateau inverses")
{
  const auto curve = sampled_trc_tag({0, 8192, 8192, 65535, 65535});
  auto colour = GainMapColour::from_icc(matrix_profile({curve, curve, curve}));
  REQUIRE(colour);
  auto value = colour->decode({0.625, 0.5, 0.875});
  REQUIRE(value);
  REQUIRE((*value)[0] == Catch::Approx((8192.0 + 65535) / 2 / 65535).margin(1e-12));
  REQUIRE((*value)[1] == 8192.0 / 65535);
  REQUIRE((*value)[2] == 1);
  auto inverse = colour->encode({8192.0 / 65535, 1, 0});
  REQUIRE(inverse);
  REQUIRE((*inverse)[0] == Catch::Approx(0.5).margin(1e-8));
  REQUIRE((*inverse)[1] == Catch::Approx(0.75).margin(1e-8));
  REQUIRE((*inverse)[2] == Catch::Approx(0).margin(1e-8));
  const auto invalid = sampled_trc_tag({0, 50000, 10000, 65535});
  REQUIRE_FALSE(GainMapColour::from_icc(matrix_profile({invalid, invalid, invalid})));
}

TEST_CASE("Apple Rec.709 ICC branch overlap retains exact forward values")
{
  // Parameters from Core Image's public itur_709 producer, stored as
  // s15Fixed16. ICC WP35 permits a negative join caused by rounding.
  constexpr std::array<double, 5> p{0x238e4 / 65536.0, 0xe8f0 / 65536.0,
      0x1710 / 65536.0, 0x38e4 / 65536.0, 0x14bc / 65536.0};
  std::vector<uint8_t> curve;
  put_u32(curve, signature("para"));
  put_u32(curve, 0);
  put_u16(curve, 3);
  put_u16(curve, 0);
  for (double value : p) { put_fixed(curve, value); }
  const size_t channel = GENERATE(size_t{0}, size_t{1}, size_t{2});
  std::array<std::vector<uint8_t>, 3> curves;
  for (auto& trc : curves) { trc = sampled_trc_tag({}); }
  curves[channel] = curve;
  auto colour = GainMapColour::from_icc(matrix_profile(curves));
  REQUIRE(colour);
  const double left = p[3] * p[4], right = std::pow(p[1] * p[4] + p[2], p[0]);
  REQUIRE(left > right);
  for (double sample : {0.0, 0.04, std::nextafter(p[4], 0.0), p[4], 0.5, 1.0}) {
    auto decoded = colour->decode({sample, sample, sample});
    REQUIRE(decoded);
    const double expected = sample < p[4] ? p[3] * sample : std::pow(p[1] * sample + p[2], p[0]);
    REQUIRE((*decoded)[channel] == Catch::Approx(expected).margin(1e-12));
  }
  const double overlap = (left + right) / 2;
  auto encoded = colour->encode({overlap, overlap, overlap});
  REQUIRE(encoded);
  // Two exact inverses exist here. Keep the existing lower-branch choice.
  REQUIRE((*encoded)[channel] == Catch::Approx(overlap / p[3]).margin(1e-12));
  REQUIRE((*encoded)[channel] < p[4]);
  auto decoded = colour->decode(*encoded);
  REQUIRE(decoded);
  for (double value : *decoded) { REQUIRE(value == Catch::Approx(overlap).margin(1e-12)); }
}


TEST_CASE("ICC gamma and parametric equations preserve extended relative HDR")
{
  const int kind = GENERATE(-2, -1, 0, 1, 2, 3, 4);
  std::vector<uint8_t> curve;
  if (kind < 0) {
    curve = sampled_trc_tag(kind == -2 ? std::vector<uint16_t>{} : std::vector<uint16_t>{512});
  }
  else {
    put_u32(curve, signature("para"));
    put_u32(curve, 0);
    put_u16(curve, static_cast<uint16_t>(kind));
    put_u16(curve, 0);
    const std::array<double, 7> p{2, 1, 0, kind == 2 ? 0.125 : 0.25, 0.25, 0.125, 0.125};
    const std::array<size_t, 5> counts{1, 3, 4, 5, 7};
    for (size_t i = 0; i < counts[static_cast<size_t>(kind)]; ++i) { put_fixed(curve, p[i]); }
  }
  auto colour = GainMapColour::from_icc(matrix_profile({curve, curve, curve}));
  REQUIRE(colour);
  const GainMapRGB signal{1.25, 2, 4};
  auto linear = colour->decode(signal);
  REQUIRE(linear);
  for (size_t c = 0; c < 3; ++c) {
    const double expected = kind == -2 ? signal[c] : signal[c] * signal[c] + ((kind == 2 || kind == 4) ? 0.125 : 0);
    REQUIRE((*linear)[c] == Catch::Approx(expected).margin(1e-12));
  }
  auto encoded = colour->encode(*linear);
  REQUIRE(encoded);
  for (size_t c = 0; c < 3; ++c) { REQUIRE((*encoded)[c] == Catch::Approx(signal[c]).margin(1e-12)); }
}

TEST_CASE("ICC storage NCLX does not clip relative RGB before its actual transfer")
{
  const bool full_range = GENERATE(false, true);
  const auto curve = sampled_trc_tag({512});
  const auto profile = matrix_profile({curve, curve, curve});
  auto colour = GainMapColour::from_icc(profile);
  REQUIRE(colour);
  auto base = std::make_shared<HeifPixelImage>();
  base->create(1, 1, heif_colorspace_YCbCr, heif_chroma_444);
  const std::array<heif_channel, 3> input{heif_channel_Y, heif_channel_Cb, heif_channel_Cr};
  const std::array<uint8_t, 3> codes{200, 168, 188};
  for (size_t c = 0; c < 3; ++c) {
    REQUIRE_FALSE(base->add_channel(input[c], 1, 1, 8, nullptr));
    base->get_channel_memory(input[c], nullptr)[0] = codes[c];
  }
  nclx_profile storage;
  storage.set_colour_primaries(2);
  storage.set_transfer_characteristics(2);
  storage.set_matrix_coefficients(6);
  storage.set_full_range_flag(full_range);
  base->set_color_profile_nclx(storage);
  base->set_color_profile_icc(profile);
  auto gain = std::make_shared<HeifPixelImage>();
  gain->create(1, 1, heif_colorspace_monochrome, heif_chroma_monochrome);
  REQUIRE_FALSE(gain->add_channel(heif_channel_Y, 1, 1, 8, nullptr));
  gain->fill_channel(heif_channel_Y, 255);
  GainMapMetadata metadata;
  metadata.channels[0].gain_map_min = {1, 1};
  metadata.channels[0].gain_map_max = {1, 1};
  metadata.channels[0].base_offset = {1, 8};
  metadata.channels[0].alternate_offset = {1, 16};
  auto* options = heif_decoding_options_alloc();
  REQUIRE(options);
  auto output = reconstruct_tone_map(base, gain, metadata, *colour, *options, nullptr,
                                     *colour, std::nullopt, true);
  REQUIRE(output);
  const double y = full_range ? 200.0 / 255 : 184.0 / 219;
  const double cb = 40.0 / (full_range ? 255 : 224);
  const double cr = 60.0 / (full_range ? 255 : 224);
  const double r = y + 1.402 * cr, b = y + 1.772 * cb;
  const GainMapRGB signal{r, (y - 0.299 * r - 0.114 * b) / 0.587, b};
  const std::array<heif_channel, 3> channels{heif_channel_R, heif_channel_G, heif_channel_B};
  for (size_t c = 0; c < 3; ++c) {
    // Independent gamma-2 EOTF, ISO gain/offset equation, inverse gamma-2.
    const double expected = std::sqrt((signal[c] * signal[c] + 0.125) * 2 - 0.0625);
    REQUIRE((*output)->get_channel_memory<float>(channels[c], nullptr)[0] ==
            Catch::Approx(expected).margin(1e-6));
  }
  REQUIRE(base->get_channel_memory(heif_channel_Cr, nullptr)[0] == 188);
  heif_decoding_options_free(options);
}

#if LIBHEIF_TEST_LCMS
namespace {
std::shared_ptr<const color_profile_raw> cmyk_lut_profile(bool lab)
{
  using Profile = std::unique_ptr<void, decltype(&cmsCloseProfile)>;
  using Curve = std::unique_ptr<cmsToneCurve, decltype(&cmsFreeToneCurve)>;
  using Pipeline = std::unique_ptr<cmsPipeline, decltype(&cmsPipelineFree)>;
  Profile profile(cmsCreateProfilePlaceholder(nullptr), cmsCloseProfile);
  REQUIRE(profile);
  cmsSetProfileVersion(profile.get(), 4.3);
  cmsSetDeviceClass(profile.get(), cmsSigInputClass);
  cmsSetColorSpace(profile.get(), cmsSigCmykData);
  cmsSetPCS(profile.get(), lab ? cmsSigLabData : cmsSigXYZData);
  REQUIRE(cmsWriteTag(profile.get(), cmsSigMediaWhitePointTag, cmsD50_XYZ()));
  const auto text_tag = [&](cmsTagSignature tag, const char* text) {
    std::unique_ptr<cmsMLU, decltype(&cmsMLUfree)> value(cmsMLUalloc(nullptr, 1), cmsMLUfree);
    REQUIRE(value);
    REQUIRE(cmsMLUsetASCII(value.get(), "en", "US", text));
    REQUIRE(cmsWriteTag(profile.get(), tag, value.get()));
  };
  text_tag(cmsSigProfileDescriptionTag, "libheif synthetic CMYK input fixture");
  text_tag(cmsSigCopyrightTag, "Copyright 2026 libheif contributors");
  Curve identity(cmsBuildGamma(nullptr, 1), cmsFreeToneCurve);
  REQUIRE(identity);
  cmsToneCurve* curves[4] = {identity.get(), identity.get(), identity.get(), identity.get()};
  Pipeline pipeline(cmsPipelineAlloc(nullptr, 4, 3), cmsPipelineFree);
  REQUIRE(pipeline);
  REQUIRE(cmsPipelineInsertStage(pipeline.get(), cmsAT_END, cmsStageAllocToneCurves(nullptr, 4, curves)));
  auto* clut = cmsStageAllocCLut16bit(nullptr, 3, 4, 3, nullptr);
  REQUIRE(clut);
  REQUIRE(cmsStageSampleCLut16bit(clut,
      [](const cmsUInt16Number in[], cmsUInt16Number out[], void* context) -> int {
        const double black = in[3] / 65535.0;
        const double r = (1 - in[0] / 65535.0) * (1 - black);
        const double g = (1 - in[1] / 65535.0) * (1 - black);
        const double b = (1 - in[2] / 65535.0) * (1 - black);
        // A deliberately asymmetric, D50-adapted RGB basis. The four-channel
        // transform cannot be reproduced by dropping K or relabeling CMY as RGB.
        cmsCIEXYZ xyz{0.4360747 * r + 0.3850649 * g + 0.1430804 * b,
                      0.2225045 * r + 0.7168786 * g + 0.0606169 * b,
                      0.0139322 * r + 0.0971045 * g + 0.7141733 * b};
        if (*static_cast<bool*>(context)) {
          cmsCIELab value{};
          cmsXYZ2Lab(cmsD50_XYZ(), &value, &xyz);
          cmsFloat2LabEncoded(out, &value);
        }
        else {
          const double values[3] = {xyz.X, xyz.Y, xyz.Z};
          for (size_t c = 0; c < 3; ++c) {
            out[c] = static_cast<uint16_t>(std::round(values[c] * 32768));
          }
        }
        return 1;
      }, &lab, 0));
  REQUIRE(cmsPipelineInsertStage(pipeline.get(), cmsAT_END, clut));
  REQUIRE(cmsPipelineInsertStage(pipeline.get(), cmsAT_END, cmsStageAllocToneCurves(nullptr, 3, curves)));
  REQUIRE(cmsWriteTag(profile.get(), cmsSigAToB0Tag, pipeline.get()));
  REQUIRE(cmsWriteTag(profile.get(), cmsSigAToB1Tag, pipeline.get()));
  cmsUInt32Number size = 0;
  REQUIRE(cmsSaveProfileToMem(profile.get(), nullptr, &size));
  std::vector<uint8_t> bytes(size);
  REQUIRE(cmsSaveProfileToMem(profile.get(), bytes.data(), &size));
  return std::make_shared<color_profile_raw>(signature("prof"), bytes);
}
}  // namespace

TEST_CASE("CMYK ICC baselines reconstruct in the declared alternate RGB application space")
{
  const bool lab = GENERATE(false, true);
  const int bits = GENERATE(8, 16, 32, 64);
  const bool floating = GENERATE(false, true);
  if (floating && bits < 32) { return; }
  const auto profile = cmyk_lut_profile(lab);
  auto colour = GainMapColour::from_icc(profile);
#if LIBHEIF_HAVE_LCMS2
  const bool premultiplied = GENERATE(false, true);
  REQUIRE(colour);
  REQUIRE_FALSE(colour->has_application_primaries());
  REQUIRE_FALSE(colour->decode({0.1, 0.2, 0.3}));
  REQUIRE_FALSE(colour->encode({0.1, 0.2, 0.3}));
  REQUIRE_FALSE(colour->decode_cmyk({0.1, NAN, 0.3, 0.4}));
  nclx_profile nclx;
  nclx.set_colour_primaries(9);
  nclx.set_transfer_characteristics(8);
  nclx.set_matrix_coefficients(0);
  nclx.set_full_range_flag(true);
  const GainMapColour alternate(nclx);
  using Profile = std::unique_ptr<void, decltype(&cmsCloseProfile)>;
  using Transform = std::unique_ptr<void, decltype(&cmsDeleteTransform)>;
  Profile input(cmsOpenProfileFromMem(profile->get_data().data(),
      static_cast<cmsUInt32Number>(profile->get_data().size())), cmsCloseProfile);
  Profile xyz(cmsCreateXYZProfile(), cmsCloseProfile);
  REQUIRE(input);
  REQUIRE(xyz);
  Transform reference(cmsCreateTransform(input.get(), TYPE_CMYK_DBL, xyz.get(), TYPE_XYZ_DBL,
      INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOOPTIMIZE | cmsFLAGS_NOCACHE), cmsDeleteTransform);
  REQUIRE(reference);
  auto matrix = colour->matrix_to(alternate);
  REQUIRE(matrix);
  auto base = std::make_shared<HeifPixelImage>();
  base->create(3, 2, heif_colorspace_custom, heif_chroma_planar);
  base->set_color_profile_icc(profile);
  base->set_premultiplied_alpha(premultiplied);
  REQUIRE_FALSE(base->add_channel(heif_channel_Alpha, 3, 2, 32, nullptr, heif_component_datatype_floating_point));
  size_t alpha_stride = 0;
  auto* alpha = base->get_channel_memory<float>(heif_channel_Alpha, &alpha_stride);
  for (size_t y = 0; y < 2; ++y) { std::fill_n(alpha + y * alpha_stride / sizeof(float), 3, 0.5f); }
  const std::array<uint16_t, 4> types{heif_cmpd_component_type_cyan, heif_cmpd_component_type_magenta,
                                    heif_cmpd_component_type_yellow, heif_cmpd_component_type_key_black};
  // Reorder the storage planes so component types, rather than position/ID,
  // determine CMYK order. Each component has a different value.
  const std::array<double, 4> samples{0, 0.5, 1, 0.5};
  std::array<double, 4> actual{};
  for (size_t c : {3u, 1u, 0u, 2u}) {
    const auto datatype = floating ? heif_component_datatype_floating_point : heif_component_datatype_unsigned_integer;
    auto id = base->add_component(3, 2, types[c], datatype, bits, nullptr);
    REQUIRE(id);
    size_t stride = 0;
    auto* plane = base->get_component(*id, &stride);
    const double fraction = samples[c] * (premultiplied ? 0.5 : 1);
    const double value = floating ? fraction : std::round(fraction * (std::ldexp(1.0, bits) - 1));
    actual[c] = (floating ? value : value / (std::ldexp(1.0, bits) - 1)) * (premultiplied ? 2 : 1);
    for (size_t y = 0; y < 2; ++y) {
      auto* row = plane + y * stride;
      for (size_t x = 0; x < 3; ++x) {
        if (floating && bits == 32) { reinterpret_cast<float*>(row)[x] = static_cast<float>(value); }
        else if (floating) { reinterpret_cast<double*>(row)[x] = value; }
        else if (bits == 8) { row[x] = static_cast<uint8_t>(value); }
        else if (bits == 16) { reinterpret_cast<uint16_t*>(row)[x] = static_cast<uint16_t>(value); }
        else if (bits == 32) { reinterpret_cast<uint32_t*>(row)[x] = static_cast<uint32_t>(value); }
        else { reinterpret_cast<uint64_t*>(row)[x] = fraction == 1 ? UINT64_MAX : static_cast<uint64_t>(value); }
      }
    }
  }
  auto gain = std::make_shared<HeifPixelImage>();
  gain->create(1, 1, heif_colorspace_monochrome, heif_chroma_monochrome);
  REQUIRE_FALSE(gain->add_channel(heif_channel_Y, 1, 1, 8, nullptr));
  gain->get_channel_memory(heif_channel_Y, nullptr)[0] = 255;
  gain->set_color_profile_nclx(nclx);
  GainMapMetadata metadata;
  metadata.use_base_colour_space = false;
  metadata.channel_count = 3;
  metadata.alternate_hdr_headroom = {2, 1};
  for (size_t c = 0; c < 3; ++c) {
    metadata.channels[c].gain_map_min = {static_cast<int32_t>(c), 2};
    metadata.channels[c].gain_map_max = metadata.channels[c].gain_map_min;
  }
  auto* options = heif_decoding_options_alloc();
  auto result = reconstruct_tone_map(base, gain, metadata, alternate, *options, nullptr, *colour,
                                     std::nullopt, true);
  REQUIRE(result);
  REQUIRE((*result)->is_premultiplied_alpha() == premultiplied);
  REQUIRE((*result)->get_channel_memory<float>(heif_channel_Alpha, nullptr)[0] == 0.5f);
  for (auto& value : actual) { value *= 100; } // Little CMS floating CMYK is in percent.
  GainMapRGB pcs{};
  cmsDoTransform(reference.get(), actual.data(), pcs.data(), 1);
  const auto rgb = gain_map_transform(*matrix, pcs);
  const std::array<heif_channel, 3> channels{heif_channel_R, heif_channel_G, heif_channel_B};
  for (size_t c = 0; c < 3; ++c) {
    size_t stride = 0;
    const auto* out = (*result)->get_channel_memory<float>(channels[c], &stride);
    for (size_t y = 0; y < 2; ++y) {
      for (size_t x = 0; x < 3; ++x) {
        REQUIRE(out[y * stride / sizeof(float) + x] ==
            Catch::Approx(rgb[c] * std::exp2(double(c) / 2) * (premultiplied ? 0.5 : 1)).margin(1e-5));
      }
    }
  }
  metadata.use_base_colour_space = true;
  REQUIRE_FALSE(reconstruct_tone_map(base, gain, metadata, alternate, *options, nullptr, *colour));
  metadata.use_base_colour_space = false;
  // Each malformed layout must fail before sample access; none may silently
  // drop K, guess the component order or read beyond a short plane.
  for (int defect = 0; defect < 6; ++defect) {
    auto invalid = std::make_shared<HeifPixelImage>();
    invalid->create(3, 2, heif_colorspace_custom, heif_chroma_planar);
    const size_t count = defect == 0 ? 3 : defect == 1 || defect == 5 ? 5 : 4;
    for (size_t c = 0; c < count; ++c) {
      const uint16_t type = c == 4 ? defect == 5 ? heif_cmpd_component_type_depth : types[0] : types[c];
      const auto datatype = defect == 3 && c == 2 ? heif_component_datatype_signed_integer :
                            defect == 4 && c == 1 ? heif_component_datatype_complex_number :
                                                   heif_component_datatype_unsigned_integer;
      REQUIRE(invalid->add_component(defect == 2 && c == 2 ? 2 : 3, 2, type, datatype, 32, nullptr));
    }
    REQUIRE_FALSE(reconstruct_tone_map(invalid, gain, metadata, alternate, *options, nullptr, *colour));
  }
  heif_decoding_options_free(options);
#else
  REQUIRE_FALSE(colour);
  REQUIRE(colour.error().error_code == heif_error_Unsupported_feature);
#endif
}

#if HEIF_ENABLE_EXPERIMENTAL_FEATURES
TEST_CASE("Serialized CMYK ICC baselines match independent RGB colour management")
{
  const bool lab = GENERATE(false, true);
  const int bits = GENERATE(8, 16);
  const double target = GENERATE(0.0, 1.0, 2.0);
  const auto profile = cmyk_lut_profile(lab);
  auto* ctx = heif_context_alloc();
  REQUIRE(ctx);
  heif_encoder* encoder = nullptr;
  const auto available = heif_context_get_encoder_for_format(ctx, heif_compression_uncompressed, &encoder);
  if (available.code != heif_error_Ok) {
    heif_context_free(ctx);
    SKIP("Uncompressed encoder unavailable");
  }
  heif_image* pixels = nullptr;
  REQUIRE(heif_image_create(2, 2, heif_colorspace_custom, heif_chroma_planar, &pixels).code == heif_error_Ok);
  const std::array<double, 4> samples{0.1, 0.3, 0.5, 0.2};
  const double max = std::ldexp(1.0, bits) - 1;
  std::array<double, 4> actual{};
  for (size_t c : {2u, 0u, 3u, 1u}) {
    uint32_t id = 0;
    REQUIRE(heif_image_add_component(pixels, 2, 2, static_cast<uint16_t>(heif_cmpd_component_type_cyan + c),
        heif_component_datatype_unsigned_integer, bits, &id).code == heif_error_Ok);
    size_t stride = 0;
    auto* plane = heif_image_get_component(pixels, id, &stride);
    const auto code = static_cast<uint16_t>(std::round(samples[c] * max));
    actual[c] = code * 100 / max;
    for (size_t y = 0; y < 2; ++y) {
      if (bits == 8) { std::fill_n(plane + y * stride, 2, static_cast<uint8_t>(code)); }
      else { std::fill_n(reinterpret_cast<uint16_t*>(plane + y * stride), 2, code); }
    }
  }
  const auto& icc = profile->get_data();
  REQUIRE(heif_image_set_raw_color_profile(pixels, "prof", icc.data(), icc.size()).code == heif_error_Ok);
  heif_image_handle* base = nullptr;
  auto error = heif_context_encode_image(ctx, pixels, encoder, nullptr, &base);
  INFO(error.message);
  REQUIRE(error.code == heif_error_Ok);
  heif_image_release(pixels);
  REQUIRE(heif_image_create(1, 1, heif_colorspace_monochrome, heif_chroma_monochrome, &pixels).code == heif_error_Ok);
  REQUIRE(heif_image_add_plane(pixels, heif_channel_Y, 1, 1, 8).code == heif_error_Ok);
  size_t stride = 0;
  heif_image_get_plane2(pixels, heif_channel_Y, &stride)[0] = 255;
  heif_image_handle* gain = nullptr;
  REQUIRE(heif_context_encode_gain_map_image(ctx, pixels, encoder, nullptr, nullptr, &gain).code == heif_error_Ok);
  heif_image_release(pixels);
  heif_color_profile_nclx alternate{};
  alternate.version = 1;
  alternate.color_primaries = heif_color_primaries_ITU_R_BT_2020_2_and_2100_0;
  alternate.transfer_characteristics = heif_transfer_characteristic_linear;
  alternate.matrix_coefficients = heif_matrix_coefficients_RGB_GBR;
  alternate.full_range_flag = 1;
  auto* options = heif_tone_map_options_alloc();
  REQUIRE(options);
  options->alternate_nclx = &alternate;
  heif_gain_map_metadata metadata{};
  metadata.struct_version = 1;
  metadata.channel_count = 3;
  metadata.base_hdr_headroom = {0, 1};
  metadata.alternate_hdr_headroom = {2, 1};
  for (size_t c = 0; c < 3; ++c) {
    metadata.channels[c].gain_map_min = {static_cast<int32_t>(c), 2};
    metadata.channels[c].gain_map_max = metadata.channels[c].gain_map_min;
    metadata.channels[c].gamma = {1, 1};
    metadata.channels[c].base_offset = {0, 1};
    metadata.channels[c].alternate_offset = {0, 1};
  }
  heif_image_handle* tmap = nullptr;
  REQUIRE(heif_context_add_tone_map_derived_image(ctx, base, gain, &metadata, options, &tmap).code == heif_error_Ok);
  std::vector<uint8_t> bytes;
  heif_writer writer{};
  writer.writer_api_version = 1;
  writer.write = [](heif_context*, const void* data, size_t size, void* context) -> heif_error {
    auto& out = *static_cast<std::vector<uint8_t>*>(context);
    const auto* first = static_cast<const uint8_t*>(data);
    out.insert(out.end(), first, first + size);
    return {heif_error_Ok, heif_suberror_Unspecified, nullptr};
  };
  REQUIRE(heif_context_write(ctx, &writer, &bytes).code == heif_error_Ok);
  auto* reopened = heif_context_alloc();
  REQUIRE(reopened);
  REQUIRE(heif_context_read_from_memory_without_copy(reopened, bytes.data(), bytes.size(), nullptr).code == heif_error_Ok);
  heif_image_handle* read_tmap = nullptr;
  REQUIRE(heif_context_get_image_handle(reopened, heif_image_handle_get_item_id(tmap), &read_tmap).code == heif_error_Ok);
  heif_image_handle* read_base = nullptr;
  REQUIRE(heif_image_handle_get_tone_map_base_image_handle(read_tmap, &read_base).code == heif_error_Ok);
  std::vector<uint8_t> read_icc(heif_image_handle_get_raw_color_profile_size(read_base));
  REQUIRE(read_icc.size() == icc.size());
  REQUIRE(heif_image_handle_get_raw_color_profile(read_base, read_icc.data()).code == heif_error_Ok);
  REQUIRE(read_icc == icc);
  pixels = nullptr;
  error = heif_decode_tone_map_image_float32(read_tmap, &pixels, nullptr, target, heif_gain_map_resampling_phase_co_sited);
#if LIBHEIF_HAVE_LCMS2
  INFO(error.message);
  REQUIRE(error.code == heif_error_Ok);
  using Profile = std::unique_ptr<void, decltype(&cmsCloseProfile)>;
  using Curve = std::unique_ptr<cmsToneCurve, decltype(&cmsFreeToneCurve)>;
  using Transform = std::unique_ptr<void, decltype(&cmsDeleteTransform)>;
  Profile input(cmsOpenProfileFromMem(icc.data(), static_cast<cmsUInt32Number>(icc.size())), cmsCloseProfile);
  Curve identity(cmsBuildGamma(nullptr, 1), cmsFreeToneCurve);
  const cmsCIExyY white{0.3127, 0.3290, 1};
  const cmsCIExyYTRIPLE primaries{{0.708, 0.292, 1}, {0.170, 0.797, 1}, {0.131, 0.046, 1}};
  cmsToneCurve* curves[3] = {identity.get(), identity.get(), identity.get()};
  Profile output(cmsCreateRGBProfile(&white, &primaries, curves), cmsCloseProfile);
  Transform cmm(cmsCreateTransform(input.get(), TYPE_CMYK_DBL, output.get(), TYPE_RGB_DBL,
      INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOOPTIMIZE | cmsFLAGS_NOCACHE), cmsDeleteTransform);
  REQUIRE(cmm);
  GainMapRGB reference{};
  cmsDoTransform(cmm.get(), actual.data(), reference.data(), 1);
  for (size_t c = 0; c < 3; ++c) {
    const auto* plane = reinterpret_cast<const float*>(heif_image_get_plane_readonly2(pixels,
        static_cast<heif_channel>(heif_channel_R + c), &stride));
    REQUIRE(plane);
    REQUIRE(plane[0] == Catch::Approx(reference[c] * std::exp2(double(c) * target / 4)).margin(0.00015));
  }
#else
  REQUIRE(error.code == heif_error_Unsupported_feature);
  REQUIRE_FALSE(pixels);
#endif
  heif_image_release(pixels);
  heif_image_handle_release(read_base);
  heif_image_handle_release(read_tmap);
  heif_context_free(reopened);
  heif_image_handle_release(tmap);
  heif_image_handle_release(gain);
  heif_image_handle_release(base);
  heif_tone_map_options_free(options);
  heif_encoder_release(encoder);
  heif_context_free(ctx);
}
#endif

#if HEIF_ENABLE_EXPERIMENTAL_FEATURES
TEST_CASE("Gray ICC alternate files preserve mono samples and colour manage requested RGB")
{
  const bool lab = GENERATE(false, true);
  const bool pixi = GENERATE(false, true);
  const double target = GENERATE(0.0, 1.0, 2.0);
  const int requested = GENERATE(0, 1, 2); // Default, BT.2020, unspecified RGB fields.
  using Profile = std::unique_ptr<void, decltype(&cmsCloseProfile)>;
  using Curve = std::unique_ptr<cmsToneCurve, decltype(&cmsFreeToneCurve)>;
  using Transform = std::unique_ptr<void, decltype(&cmsDeleteTransform)>;
  Curve gamma(cmsBuildGamma(nullptr, 2), cmsFreeToneCurve);
  Curve identity(cmsBuildGamma(nullptr, 1), cmsFreeToneCurve);
  REQUIRE(gamma);
  REQUIRE(identity);
  Profile gray(cmsCreateGrayProfile(cmsD50_xyY(), gamma.get()), cmsCloseProfile);
  REQUIRE(gray);
  cmsSetProfileVersion(gray.get(), 4.4);
  if (lab) { cmsSetPCS(gray.get(), cmsSigLabData); }
  cmsUInt32Number size = 0;
  REQUIRE(cmsSaveProfileToMem(gray.get(), nullptr, &size));
  std::vector<uint8_t> icc(size);
  REQUIRE(cmsSaveProfileToMem(gray.get(), icc.data(), &size));
  auto* ctx = heif_context_alloc();
  REQUIRE(ctx);
  heif_encoder* encoder = nullptr;
  const auto available = heif_context_get_encoder_for_format(ctx, heif_compression_uncompressed, &encoder);
  if (available.code != heif_error_Ok) {
    heif_context_free(ctx);
    SKIP("Uncompressed encoder unavailable");
  }
  heif_color_profile_nclx base_profile{};
  base_profile.version = 1;
  base_profile.color_primaries = heif_color_primaries_ITU_R_BT_2020_2_and_2100_0;
  base_profile.transfer_characteristics = heif_transfer_characteristic_linear;
  base_profile.matrix_coefficients = heif_matrix_coefficients_RGB_GBR;
  base_profile.full_range_flag = 1;
  heif_image* image = nullptr;
  REQUIRE(heif_image_create(2, 2, heif_colorspace_RGB, heif_chroma_444, &image).code == heif_error_Ok);
  const std::array<heif_channel, 3> channels{heif_channel_R, heif_channel_G, heif_channel_B};
  for (size_t c = 0; c < 3; ++c) {
    REQUIRE(heif_image_add_plane(image, channels[c], 2, 2, 8).code == heif_error_Ok);
    size_t stride = 0;
    auto* plane = heif_image_get_plane2(image, channels[c], &stride);
    for (size_t y = 0; y < 2; ++y) { std::fill_n(plane + y * stride, 2, static_cast<uint8_t>(64 + c * 32)); }
  }
  REQUIRE(heif_image_set_nclx_color_profile(image, &base_profile).code == heif_error_Ok);
  heif_image_handle* base = nullptr;
  auto* encoding = heif_encoding_options_alloc();
  REQUIRE(encoding);
  encoding->output_nclx_profile = &base_profile;
  REQUIRE(heif_context_encode_image(ctx, image, encoder, encoding, &base).code == heif_error_Ok);
  heif_encoding_options_free(encoding);
  heif_image_release(image);
  REQUIRE(heif_image_create(1, 1, heif_colorspace_monochrome, heif_chroma_monochrome, &image).code == heif_error_Ok);
  REQUIRE(heif_image_add_plane(image, heif_channel_Y, 1, 1, 8).code == heif_error_Ok);
  size_t stride = 0;
  heif_image_get_plane2(image, heif_channel_Y, &stride)[0] = 255;
  heif_image_handle* gain = nullptr;
  REQUIRE(heif_context_encode_gain_map_image(ctx, image, encoder, nullptr, nullptr, &gain).code == heif_error_Ok);
  heif_image_release(image);
  heif_gain_map_metadata metadata{};
  metadata.struct_version = 1;
  metadata.channel_count = 3;
  metadata.use_base_colour_space = 1;
  metadata.base_hdr_headroom = {0, 1};
  metadata.alternate_hdr_headroom = {2, 1};
  for (size_t c = 0; c < 3; ++c) {
    metadata.channels[c].gain_map_min = {static_cast<int32_t>(c), 2};
    metadata.channels[c].gain_map_max = metadata.channels[c].gain_map_min;
    metadata.channels[c].gamma = {1, 1};
    metadata.channels[c].base_offset = {0, 1};
    metadata.channels[c].alternate_offset = {0, 1};
  }
  auto* options = heif_tone_map_options_alloc();
  REQUIRE(options);
  options->alternate_icc_type = heif_color_profile_type_prof;
  options->alternate_icc = icc.data();
  options->alternate_icc_size = icc.size();
  options->pixi_num_channels = 3;
  for (auto& depth : options->pixi_bits_per_channel) { depth = 12; }
  const auto count = heif_context_get_number_of_items(ctx);
  heif_image_handle* tmap = nullptr;
  const auto invalid_hint = heif_context_add_tone_map_derived_image(ctx, base, gain, &metadata, options, &tmap);
  INFO(invalid_hint.message);
  REQUIRE(invalid_hint.code == heif_error_Usage_error);
  REQUIRE_FALSE(tmap);
  REQUIRE(heif_context_get_number_of_items(ctx) == count);
  options->pixi_num_channels = pixi ? 1 : 0;
  REQUIRE(heif_context_add_tone_map_derived_image(ctx, base, gain, &metadata, options, &tmap).code == heif_error_Ok);
  heif_image_handle* nested = nullptr;
  if (!lab && pixi && requested == 0) {
    auto nested_metadata = metadata;
    nested_metadata.use_base_colour_space = 0;
    nested_metadata.channel_count = 1;
    nested_metadata.channels[0].gain_map_min = {1, 1};
    nested_metadata.channels[0].gain_map_max = {1, 1};
    auto* nested_options = heif_tone_map_options_alloc();
    REQUIRE(nested_options);
    nested_options->alternate_nclx = &base_profile;
    nested_options->create_altr_group = 0;
    REQUIRE(heif_context_add_tone_map_derived_image(ctx, tmap, gain, &nested_metadata,
        nested_options, &nested).code == heif_error_Ok);
    heif_tone_map_options_free(nested_options);
  }
  std::vector<uint8_t> bytes;
  heif_writer writer{};
  writer.writer_api_version = 1;
  writer.write = [](heif_context*, const void* data, size_t length, void* context) -> heif_error {
    auto& out = *static_cast<std::vector<uint8_t>*>(context);
    const auto* first = static_cast<const uint8_t*>(data);
    out.insert(out.end(), first, first + length);
    return {heif_error_Ok, heif_suberror_Unspecified, nullptr};
  };
  REQUIRE(heif_context_write(ctx, &writer, &bytes).code == heif_error_Ok);
  auto* reopened = heif_context_alloc();
  REQUIRE(reopened);
  REQUIRE(heif_context_read_from_memory_without_copy(reopened, bytes.data(), bytes.size(), nullptr).code == heif_error_Ok);
  heif_image_handle* handle = nullptr;
  REQUIRE(heif_context_get_image_handle(reopened, heif_image_handle_get_item_id(tmap), &handle).code == heif_error_Ok);
  heif_colorspace colours;
  heif_chroma chroma;
  REQUIRE(heif_image_handle_get_preferred_decoding_colorspace(handle, &colours, &chroma).code == heif_error_Ok);
  REQUIRE(colours == heif_colorspace_monochrome);
  REQUIRE(chroma == heif_chroma_monochrome);
  REQUIRE(heif_image_handle_get_luma_bits_per_pixel(handle) == -1);
  const auto decoded_mono = heif_decode_tone_map_image(handle, &image, heif_colorspace_monochrome,
      heif_chroma_monochrome, nullptr, target);
  INFO(decoded_mono.message);
  REQUIRE(decoded_mono.code == heif_error_Ok);
  REQUIRE(heif_image_get_bits_per_pixel_range(image, heif_channel_Y) == 16);
  std::vector<uint8_t> decoded_icc(heif_image_get_raw_color_profile_size(image));
  REQUIRE(decoded_icc.size() == icc.size());
  REQUIRE(heif_image_get_raw_color_profile(image, decoded_icc.data()).code == heif_error_Ok);
  REQUIRE(decoded_icc == icc);
  const cmsCIExyY white{0.3127, 0.3290, 1};
  const cmsCIExyYTRIPLE bt2020{{0.708, 0.292, 1}, {0.170, 0.797, 1}, {0.131, 0.046, 1}};
  const cmsCIExyYTRIPLE bt709{{0.64, 0.33, 1}, {0.30, 0.60, 1}, {0.15, 0.06, 1}};
  cmsToneCurve* curves[3] = {identity.get(), identity.get(), identity.get()};
  Profile source(cmsCreateRGBProfile(&white, &bt2020, curves), cmsCloseProfile);
  Profile output(cmsCreateRGBProfile(&white, requested == 1 ? &bt2020 : &bt709, curves), cmsCloseProfile);
  REQUIRE(source);
  REQUIRE(output);
  constexpr auto flags = cmsFLAGS_NOOPTIMIZE | cmsFLAGS_NOCACHE;
  Transform forward(cmsCreateTransform(source.get(), TYPE_RGB_DBL, gray.get(), TYPE_GRAY_DBL,
      INTENT_RELATIVE_COLORIMETRIC, flags), cmsDeleteTransform);
  Transform reverse(cmsCreateTransform(gray.get(), TYPE_GRAY_DBL, output.get(), TYPE_RGB_DBL,
      INTENT_RELATIVE_COLORIMETRIC, flags), cmsDeleteTransform);
  REQUIRE(forward);
  REQUIRE(reverse);
  GainMapRGB applied{};
  for (size_t c = 0; c < 3; ++c) { applied[c] = (64.0 + double(c) * 32) / 255 * std::exp2(double(c) * target / 4); }
  double encoded_gray = 0;
  cmsDoTransform(forward.get(), applied.data(), &encoded_gray, 1);
  const auto* mono = reinterpret_cast<const uint16_t*>(heif_image_get_plane_readonly2(image, heif_channel_Y, &stride));
  REQUIRE(mono);
  REQUIRE(mono[0] / 65535.0 == Catch::Approx(encoded_gray).margin(0.00015));
  heif_image_release(image);
  auto requested_profile = base_profile;
  if (requested == 2) {
    requested_profile.color_primaries = heif_color_primaries_unspecified;
    requested_profile.transfer_characteristics = heif_transfer_characteristic_unspecified;
  }
  auto* decoding = heif_decoding_options_alloc();
  REQUIRE(decoding);
  if (requested) { decoding->output_image_nclx_profile = &requested_profile; }
  const auto decoded = heif_decode_tone_map_image_float32(handle, &image, decoding, target,
      heif_gain_map_resampling_phase_co_sited);
  INFO(decoded.message);
  REQUIRE(decoded.code == heif_error_Ok);
  REQUIRE(heif_image_get_raw_color_profile_size(image) == 0);
  heif_color_profile_nclx* nclx = nullptr;
  REQUIRE(heif_image_get_nclx_color_profile(image, &nclx).code == heif_error_Ok);
  REQUIRE(nclx->color_primaries == (requested == 1 ? heif_color_primaries_ITU_R_BT_2020_2_and_2100_0 : heif_color_primaries_ITU_R_BT_709_5));
  REQUIRE(nclx->transfer_characteristics == heif_transfer_characteristic_linear);
  REQUIRE(nclx->matrix_coefficients == heif_matrix_coefficients_RGB_GBR);
  REQUIRE(nclx->full_range_flag);
  heif_nclx_color_profile_free(nclx);
  GainMapRGB reference{};
  cmsDoTransform(reverse.get(), &encoded_gray, reference.data(), 1);
  for (size_t c = 0; c < 3; ++c) {
    const auto* plane = reinterpret_cast<const float*>(heif_image_get_plane_readonly2(image, channels[c], &stride));
    REQUIRE(plane);
    REQUIRE(plane[0] == Catch::Approx(reference[c]).margin(0.00015));
  }
  heif_image_release(image);
  heif_decoding_options_free(decoding);
  if (nested) {
    heif_image_handle* read_nested = nullptr;
    REQUIRE(heif_context_get_image_handle(reopened, heif_image_handle_get_item_id(nested), &read_nested).code == heif_error_Ok);
    REQUIRE(heif_decode_tone_map_image_float32(read_nested, &image, nullptr, target,
        heif_gain_map_resampling_phase_co_sited).code == heif_error_Ok);
    GainMapRGB full{};
    for (size_t c = 0; c < 3; ++c) { full[c] = (64.0 + double(c) * 32) / 255 * std::exp2(double(c) / 2); }
    double full_gray = 0;
    cmsDoTransform(forward.get(), full.data(), &full_gray, 1);
    Transform to_base(cmsCreateTransform(gray.get(), TYPE_GRAY_DBL, source.get(), TYPE_RGB_DBL,
        INTENT_RELATIVE_COLORIMETRIC, flags), cmsDeleteTransform);
    REQUIRE(to_base);
    cmsDoTransform(to_base.get(), &full_gray, reference.data(), 1);
    for (size_t c = 0; c < 3; ++c) {
      const auto* plane = reinterpret_cast<const float*>(heif_image_get_plane_readonly2(image, channels[c], &stride));
      REQUIRE(plane);
      REQUIRE(plane[0] == Catch::Approx(reference[c] * std::exp2(target / 2)).margin(0.0003));
    }
    heif_image_release(image);
    heif_image_handle_release(read_nested);
    // An unknown minimum must ignore the gray alternate and return RGB base
    // pixels, even though the item still declares a gray ICC and mono PIXI.
    uint8_t* payload = nullptr;
    size_t payload_size = 0;
    REQUIRE(heif_item_get_item_data(reopened, heif_image_handle_get_item_id(handle), nullptr,
        &payload, &payload_size).code == heif_error_Ok);
    REQUIRE(payload_size > 5);
    const auto found = std::search(bytes.begin(), bytes.end(), payload, payload + payload_size);
    REQUIRE(found != bytes.end());
    const size_t offset = static_cast<size_t>(found - bytes.begin());
    heif_release_item_data(reopened, &payload);
    auto future = bytes;
    future[offset + 2] = 1; // Unknown minimum=1, uninterpreted writer=0.
    auto* fallback_context = heif_context_alloc();
    REQUIRE(fallback_context);
    REQUIRE(heif_context_read_from_memory_without_copy(fallback_context, future.data(), future.size(), nullptr).code == heif_error_Ok);
    heif_image_handle* fallback = nullptr;
    REQUIRE(heif_context_get_image_handle(fallback_context, heif_image_handle_get_item_id(handle), &fallback).code == heif_error_Ok);
    REQUIRE(heif_image_handle_get_gain_map_metadata_status(fallback) == heif_gain_map_metadata_status_unsupported_minimum_version);
    REQUIRE(heif_decode_tone_map_image_float32(fallback, &image, nullptr, target,
        heif_gain_map_resampling_phase_co_sited).code == heif_error_Ok);
    REQUIRE(heif_image_get_nclx_color_profile(image, &nclx).code == heif_error_Ok);
    REQUIRE(nclx->color_primaries == base_profile.color_primaries);
    REQUIRE(nclx->transfer_characteristics == base_profile.transfer_characteristics);
    heif_nclx_color_profile_free(nclx);
    for (size_t c = 0; c < 3; ++c) {
      const auto* plane = reinterpret_cast<const float*>(heif_image_get_plane_readonly2(image, channels[c], &stride));
      REQUIRE(plane);
      REQUIRE(plane[0] == Catch::Approx((64.0 + double(c) * 32) / 255).margin(1e-7));
    }
    heif_image_release(image);
    heif_image_handle_release(fallback);
    heif_context_free(fallback_context);
    heif_image_handle_release(nested);
  }
  heif_image_handle_release(handle);
  heif_context_free(reopened);
  heif_image_handle_release(tmap);
  heif_image_handle_release(gain);
  heif_image_handle_release(base);
  heif_tone_map_options_free(options);
  heif_encoder_release(encoder);
  heif_context_free(ctx);
}
#endif

TEST_CASE("Serialized gray ICC XYZ and Lab shapers match independent Little CMS")
{
  const bool lab = GENERATE(false, true);
  using Profile = std::unique_ptr<void, decltype(&cmsCloseProfile)>;
  using Curve = std::unique_ptr<cmsToneCurve, decltype(&cmsFreeToneCurve)>;
  using Transform = std::unique_ptr<void, decltype(&cmsDeleteTransform)>;
  Curve gamma(cmsBuildGamma(nullptr, 2), cmsFreeToneCurve);
  Curve identity(cmsBuildGamma(nullptr, 1), cmsFreeToneCurve);
  REQUIRE(gamma);
  REQUIRE(identity);
  Profile profile(cmsCreateGrayProfile(cmsD50_xyY(), gamma.get()), cmsCloseProfile);
  REQUIRE(profile);
  cmsSetProfileVersion(profile.get(), 4.4);
  if (lab) { cmsSetPCS(profile.get(), cmsSigLabData); }
  cmsUInt32Number size = 0;
  REQUIRE(cmsSaveProfileToMem(profile.get(), nullptr, &size));
  std::vector<uint8_t> bytes(size);
  REQUIRE(cmsSaveProfileToMem(profile.get(), bytes.data(), &size));
  auto gray = GainMapColour::from_icc(std::make_shared<color_profile_raw>(signature("prof"), bytes));
  REQUIRE(gray);
  const cmsCIExyY white{0.3127, 0.3290, 1};
  const cmsCIExyYTRIPLE rgb{{0.708, 0.292, 1}, {0.170, 0.797, 1}, {0.131, 0.046, 1}};
  cmsToneCurve* curves[3] = {identity.get(), identity.get(), identity.get()};
  Profile output(cmsCreateRGBProfile(&white, &rgb, curves), cmsCloseProfile);
  REQUIRE(output);
  Transform cmm(cmsCreateTransform(profile.get(), TYPE_GRAY_DBL, output.get(), TYPE_RGB_DBL,
      INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOOPTIMIZE | cmsFLAGS_NOCACHE), cmsDeleteTransform);
  REQUIRE(cmm);
  nclx_profile nclx;
  nclx.set_colour_primaries(9);
  nclx.set_transfer_characteristics(8);
  auto matrix = gray->matrix_to(GainMapColour(nclx));
  REQUIRE(matrix);
  for (double sample : {0.0, 0.01, 0.1, 0.5, 0.9, 1.0}) {
    GainMapRGB reference{};
    cmsDoTransform(cmm.get(), &sample, reference.data(), 1);
    auto pcs = gray->decode({sample, sample, sample});
    REQUIRE(pcs);
    const auto actual = gain_map_transform(*matrix, *pcs);
    for (size_t c = 0; c < 3; ++c) {
      REQUIRE(actual[c] == Catch::Approx(reference[c]).margin(0.0001));
    }
  }
}

TEST_CASE("Gray ICC LUTs override shapers in the available transform directions")
{
  const bool output_direction = GENERATE(false, true);
  using Profile = std::unique_ptr<void, decltype(&cmsCloseProfile)>;
  using Curve = std::unique_ptr<cmsToneCurve, decltype(&cmsFreeToneCurve)>;
  using Pipeline = std::unique_ptr<cmsPipeline, decltype(&cmsPipelineFree)>;
  Curve gamma(cmsBuildGamma(nullptr, 2), cmsFreeToneCurve);
  Curve identity(cmsBuildGamma(nullptr, 1), cmsFreeToneCurve);
  REQUIRE(gamma);
  REQUIRE(identity);
  Profile profile(cmsCreateGrayProfile(cmsD50_xyY(), gamma.get()), cmsCloseProfile);
  REQUIRE(profile);
  cmsSetProfileVersion(profile.get(), 4.3);
  Pipeline pipeline(cmsPipelineAlloc(nullptr, 1, 3), cmsPipelineFree);
  REQUIRE(pipeline);
  cmsToneCurve* curves[3] = {identity.get(), identity.get(), identity.get()};
  REQUIRE(cmsPipelineInsertStage(pipeline.get(), cmsAT_END, cmsStageAllocToneCurves(nullptr, 1, curves)));
  auto* clut = cmsStageAllocCLut16bit(nullptr, 33, 1, 3, nullptr);
  REQUIRE(clut);
  REQUIRE(cmsStageSampleCLut16bit(clut,
      [](const cmsUInt16Number in[], cmsUInt16Number out[], void*) -> int {
        const std::array<double, 3> white{0.9642, 1, 0.8249};
        // XYZ PCS LUT values encode 1.0 as 32768, not 65535. This input
        // transform is deliberately linear rather than the coexisting gamma 2.
        for (size_t c = 0; c < 3; ++c) {
          out[c] = static_cast<uint16_t>(std::round(in[0] * white[c] * 32768 / 65535));
        }
        return 1;
      }, nullptr, 0));
  REQUIRE(cmsPipelineInsertStage(pipeline.get(), cmsAT_END, clut));
  REQUIRE(cmsPipelineInsertStage(pipeline.get(), cmsAT_END, cmsStageAllocToneCurves(nullptr, 3, curves)));
  REQUIRE(cmsWriteTag(profile.get(), cmsSigAToB1Tag, pipeline.get()));
  if (output_direction) {
    Pipeline output(cmsPipelineAlloc(nullptr, 3, 1), cmsPipelineFree);
    REQUIRE(output);
    REQUIRE(cmsPipelineInsertStage(output.get(), cmsAT_END, cmsStageAllocToneCurves(nullptr, 3, curves)));
    auto* inverse = cmsStageAllocCLut16bit(nullptr, 9, 3, 1, nullptr);
    REQUIRE(inverse);
    REQUIRE(cmsStageSampleCLut16bit(inverse,
        [](const cmsUInt16Number in[], cmsUInt16Number out[], void*) -> int {
          // The output table deliberately differs from the gamma-2 shaper.
          const double y = std::clamp(in[1] / 32768.0, 0.0, 1.0);
          out[0] = static_cast<uint16_t>(std::round(std::cbrt(y) * 65535));
          return 1;
        }, nullptr, 0));
    REQUIRE(cmsPipelineInsertStage(output.get(), cmsAT_END, inverse));
    REQUIRE(cmsPipelineInsertStage(output.get(), cmsAT_END, cmsStageAllocToneCurves(nullptr, 1, curves)));
    REQUIRE(cmsWriteTag(profile.get(), cmsSigBToA1Tag, output.get()));
  }
  cmsUInt32Number size = 0;
  REQUIRE(cmsSaveProfileToMem(profile.get(), nullptr, &size));
  std::vector<uint8_t> bytes(size);
  REQUIRE(cmsSaveProfileToMem(profile.get(), bytes.data(), &size));
  auto gray = GainMapColour::from_icc(std::make_shared<color_profile_raw>(signature("prof"), bytes));
#if LIBHEIF_HAVE_LCMS2
  REQUIRE(gray);
  REQUIRE_FALSE(gray->has_application_primaries());
  nclx_profile nclx;
  nclx.set_colour_primaries(1);
  nclx.set_transfer_characteristics(8);
  auto matrix = gray->matrix_to(GainMapColour(nclx));
  REQUIRE(matrix);
  for (double sample : {0.0, 0.25, 0.5, 1.0}) {
    auto pcs = gray->decode({sample, sample, sample});
    REQUIRE(pcs);
    const auto actual = gain_map_transform(*matrix, *pcs);
    for (double value : actual) { REQUIRE(value == Catch::Approx(sample).margin(0.0001)); }
  }
  if (output_direction) {
    using Transform = std::unique_ptr<void, decltype(&cmsDeleteTransform)>;
    Profile xyz(cmsCreateXYZProfile(), cmsCloseProfile);
    REQUIRE(xyz);
    Transform independent(cmsCreateTransform(xyz.get(), TYPE_XYZ_DBL, profile.get(), TYPE_GRAY_DBL,
        INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOOPTIMIZE | cmsFLAGS_NOCACHE), cmsDeleteTransform);
    REQUIRE(independent);
    for (double y : {0.05, 0.25, 0.5, 0.9}) {
      const GainMapRGB pcs{0.9642 * y, y, 0.8249 * y};
      double expected = 0;
      cmsDoTransform(independent.get(), pcs.data(), &expected, 1);
      auto actual = gray->encode(pcs);
      REQUIRE(actual);
      for (double value : *actual) { REQUIRE(value == Catch::Approx(expected).margin(0.00003)); }
    }
    auto actual = gray->encode({0.9642 * 0.25, 0.25, 0.8249 * 0.25});
    REQUIRE(actual);
    REQUIRE(std::abs((*actual)[0] - 0.5) > 0.1); // Must not use the coexisting shaper.
  }
  else {
    // ICC.1:2022 8.10.2(d) uses the required gray shaper when the output
    // direction has no LUT. The input direction still uses its explicit LUT.
    auto actual = gray->encode({0.9642 * 0.25, 0.25, 0.8249 * 0.25});
    REQUIRE(actual);
    for (double value : *actual) { REQUIRE(value == Catch::Approx(0.5).margin(0.0001)); }
  }
#else
  REQUIRE_FALSE(gray);
  REQUIRE(gray.error().error_code == heif_error_Unsupported_feature);
#endif
}

namespace {
std::shared_ptr<const color_profile_raw> rgb_lut_profile(double version, double gamma,
                                                       bool colourants = true, bool encode = true,
                                                       cmsProfileClassSignature profile_class = cmsSigDisplayClass,
                                                       bool chromaticity = false)
{
  using Profile = std::unique_ptr<void, decltype(&cmsCloseProfile)>;
  using Curve = std::unique_ptr<cmsToneCurve, decltype(&cmsFreeToneCurve)>;
  using Pipeline = std::unique_ptr<cmsPipeline, decltype(&cmsPipelineFree)>;
  Curve identity(cmsBuildGamma(nullptr, 1), cmsFreeToneCurve);
  Curve forward(cmsBuildGamma(nullptr, gamma), cmsFreeToneCurve);
  Curve backward(cmsBuildGamma(nullptr, 1 / gamma), cmsFreeToneCurve);
  REQUIRE(identity);
  REQUIRE(forward);
  REQUIRE(backward);
  cmsToneCurve* curves[3] = {identity.get(), identity.get(), identity.get()};
  const cmsCIExyY white{0.3127, 0.3290, 1};
  const cmsCIExyYTRIPLE primaries{{0.640, 0.330, 1}, {0.300, 0.600, 1}, {0.150, 0.060, 1}};
  Profile profile(cmsCreateRGBProfile(&white, &primaries, curves), cmsCloseProfile);
  REQUIRE(profile);
  cmsSetProfileVersion(profile.get(), version);
  cmsSetDeviceClass(profile.get(), profile_class);
  // Chromaticity fixtures retain the native RGB basis; other fixtures use a
  // D50-scaled XYZ basis with independently known application-space values.
  // Leave identity TRCs present so selecting them would miss the LUT's gamma.
  const std::array<double, 3> white_xyz{0.9642, 1, 0.8249};
  const std::array<cmsTagSignature, 3> tags{cmsSigRedColorantTag, cmsSigGreenColorantTag, cmsSigBlueColorantTag};
  std::array<double, 9> to_pcs{}, from_pcs{};
  constexpr double pcs_scale = 65535.0 / 32768;
  for (size_t c = 0; c < 3; ++c) {
    cmsCIEXYZ xyz{};
    if (chromaticity) { xyz = *static_cast<cmsCIEXYZ*>(cmsReadTag(profile.get(), tags[c])); }
    else {
      if (c == 0) { xyz.X = white_xyz[c]; }
      if (c == 1) { xyz.Y = white_xyz[c]; }
      if (c == 2) { xyz.Z = white_xyz[c]; }
      REQUIRE(cmsWriteTag(profile.get(), tags[c], &xyz));
    }
    to_pcs[c] = xyz.X / pcs_scale;
    to_pcs[3 + c] = xyz.Y / pcs_scale;
    to_pcs[6 + c] = xyz.Z / pcs_scale;
  }
  const auto& m = to_pcs;
  const double det = m[0] * (m[4] * m[8] - m[5] * m[7]) -
                     m[1] * (m[3] * m[8] - m[5] * m[6]) +
                     m[2] * (m[3] * m[7] - m[4] * m[6]);
  REQUIRE(std::abs(det) > 1e-12);
  for (size_t row = 0; row < 3; ++row) {
    for (size_t c = 0; c < 3; ++c) {
      from_pcs[row * 3 + c] =
          (m[((c + 1) % 3) * 3 + (row + 1) % 3] * m[((c + 2) % 3) * 3 + (row + 2) % 3] -
           m[((c + 1) % 3) * 3 + (row + 2) % 3] * m[((c + 2) % 3) * 3 + (row + 1) % 3]) / det;
    }
  }
  for (bool inverse : {false, true}) {
    if (inverse && !encode) { continue; }
    Pipeline pipeline(cmsPipelineAlloc(nullptr, 3, 3), cmsPipelineFree);
    REQUIRE(pipeline);
    cmsToneCurve* input_curves[3] = {inverse ? identity.get() : forward.get(),
                                    inverse ? identity.get() : forward.get(),
                                    inverse ? identity.get() : forward.get()};
    cmsToneCurve* output_curves[3] = {inverse ? backward.get() : identity.get(),
                                     inverse ? backward.get() : identity.get(),
                                     inverse ? backward.get() : identity.get()};
    REQUIRE(cmsPipelineInsertStage(pipeline.get(), cmsAT_END, cmsStageAllocToneCurves(nullptr, 3, input_curves)));
    if (version < 4) {
      auto* clut = cmsStageAllocCLut16bit(nullptr, 33, 3, 3, nullptr);
      REQUIRE(clut);
      auto* matrix = inverse ? from_pcs.data() : to_pcs.data();
      REQUIRE(cmsStageSampleCLut16bit(clut,
          [](const cmsUInt16Number in[], cmsUInt16Number out[], void* cargo) -> int {
            const auto* matrix = static_cast<const double*>(cargo);
            for (size_t c = 0; c < 3; ++c) {
              const double value = in[0] * matrix[c * 3] + in[1] * matrix[c * 3 + 1] + in[2] * matrix[c * 3 + 2];
              out[c] = static_cast<uint16_t>(std::round(std::clamp(value, 0.0, 65535.0)));
            }
            return 1;
          }, matrix, 0));
      REQUIRE(cmsPipelineInsertStage(pipeline.get(), cmsAT_END, clut));
    }
    else {
      REQUIRE(cmsPipelineInsertStage(pipeline.get(), cmsAT_END,
          cmsStageAllocMatrix(nullptr, 3, 3, inverse ? from_pcs.data() : to_pcs.data(), nullptr)));
    }
    REQUIRE(cmsPipelineInsertStage(pipeline.get(), cmsAT_END, cmsStageAllocToneCurves(nullptr, 3, output_curves)));
    REQUIRE(cmsWriteTag(profile.get(), inverse ? cmsSigBToA1Tag : cmsSigAToB1Tag, pipeline.get()));
    if (profile_class == cmsSigColorSpaceClass) {
      // ICC.1:2022 8.7 requires both perceptual directions for ColorSpace profiles.
      REQUIRE(cmsWriteTag(profile.get(), inverse ? cmsSigBToA0Tag : cmsSigAToB0Tag, pipeline.get()));
    }
  }
  if (!colourants) {
    for (auto tag : tags) { REQUIRE(cmsWriteTag(profile.get(), tag, nullptr)); }
    if (!chromaticity) { REQUIRE(cmsWriteTag(profile.get(), cmsSigChromaticityTag, nullptr)); }
  }
  cmsUInt32Number size = 0;
  REQUIRE(cmsSaveProfileToMem(profile.get(), nullptr, &size));
  std::vector<uint8_t> bytes(size);
  REQUIRE(cmsSaveProfileToMem(profile.get(), bytes.data(), &size));
  return std::make_shared<color_profile_raw>(signature("prof"), bytes);
}
}  // namespace

#if LIBHEIF_HAVE_LCMS2
TEST_CASE("ICC hybrid profiles use directional matrix TRC fallback")
{
  const bool input_lut = GENERATE(false, true);
  const auto original = rgb_lut_profile(4.3, 2, true, true, cmsSigDisplayClass, true);
  using Profile = std::unique_ptr<void, decltype(&cmsCloseProfile)>;
  Profile profile(cmsOpenProfileFromMem(original->get_data().data(),
      static_cast<cmsUInt32Number>(original->get_data().size())), cmsCloseProfile);
  REQUIRE(profile);
  REQUIRE(cmsWriteTag(profile.get(), input_lut ? cmsSigBToA1Tag : cmsSigAToB1Tag, nullptr));
  cmsUInt32Number size = 0;
  REQUIRE(cmsSaveProfileToMem(profile.get(), nullptr, &size));
  std::vector<uint8_t> bytes(size);
  REQUIRE(cmsSaveProfileToMem(profile.get(), bytes.data(), &size));
  auto colour = GainMapColour::from_icc(std::make_shared<color_profile_raw>(signature("prof"), bytes));
#if LIBHEIF_HAVE_LCMS2
  using Curve = std::unique_ptr<cmsToneCurve, decltype(&cmsFreeToneCurve)>;
  using Transform = std::unique_ptr<void, decltype(&cmsDeleteTransform)>;
  REQUIRE(colour);
  Curve identity(cmsBuildGamma(nullptr, 1), cmsFreeToneCurve);
  REQUIRE(identity);
  const cmsCIExyY white{0.3127, 0.3290, 1};
  const cmsCIExyYTRIPLE primaries{{0.64, 0.33, 1}, {0.30, 0.60, 1}, {0.15, 0.06, 1}};
  cmsToneCurve* curves[3] = {identity.get(), identity.get(), identity.get()};
  Profile linear(cmsCreateRGBProfile(&white, &primaries, curves), cmsCloseProfile);
  REQUIRE(linear);
  constexpr auto flags = cmsFLAGS_NOOPTIMIZE | cmsFLAGS_NOCACHE;
  Transform forward(cmsCreateTransform(profile.get(), TYPE_RGB_DBL, linear.get(), TYPE_RGB_DBL,
      INTENT_RELATIVE_COLORIMETRIC, flags), cmsDeleteTransform);
  Transform reverse(cmsCreateTransform(linear.get(), TYPE_RGB_DBL, profile.get(), TYPE_RGB_DBL,
      INTENT_RELATIVE_COLORIMETRIC, flags), cmsDeleteTransform);
  REQUIRE(forward);
  REQUIRE(reverse);
  const GainMapRGB signal{0.25, 0.5, 0.75};
  const GainMapRGB values{0.0625, 0.25, 0.5625};
  GainMapRGB reference{};
  cmsDoTransform(forward.get(), signal.data(), reference.data(), 1);
  auto decoded = colour->decode(signal);
  REQUIRE(decoded);
  for (size_t c = 0; c < 3; ++c) {
    REQUIRE((*decoded)[c] == Catch::Approx(reference[c]).margin(0.0001));
    REQUIRE((*decoded)[c] == Catch::Approx(input_lut ? values[c] : signal[c]).margin(0.0001));
  }
  cmsDoTransform(reverse.get(), values.data(), reference.data(), 1);
  auto encoded = colour->encode(values);
  REQUIRE(encoded);
  for (size_t c = 0; c < 3; ++c) {
    REQUIRE((*encoded)[c] == Catch::Approx(reference[c]).margin(0.0001));
    REQUIRE((*encoded)[c] == Catch::Approx(input_lut ? values[c] : signal[c]).margin(0.0001));
  }
#else
  REQUIRE_FALSE(colour);
  REQUIRE(colour.error().error_code == heif_error_Unsupported_feature);
#endif
}

TEST_CASE("ICC relative colourimetry ignores unrelated intent LUT tags")
{
  using Profile = std::unique_ptr<void, decltype(&cmsCloseProfile)>;
  const auto original = rgb_lut_profile(4.3, 2, true, false, cmsSigDisplayClass, true);
  Profile profile(cmsOpenProfileFromMem(original->get_data().data(),
      static_cast<cmsUInt32Number>(original->get_data().size())), cmsCloseProfile);
  REQUIRE(profile);
  REQUIRE(cmsWriteTag(profile.get(), cmsSigAToB2Tag, cmsReadTag(profile.get(), cmsSigAToB1Tag)));
  REQUIRE(cmsWriteTag(profile.get(), cmsSigAToB1Tag, nullptr));
  cmsUInt32Number size = 0;
  REQUIRE(cmsSaveProfileToMem(profile.get(), nullptr, &size));
  std::vector<uint8_t> bytes(size);
  REQUIRE(cmsSaveProfileToMem(profile.get(), bytes.data(), &size));
  auto colour = GainMapColour::from_icc(std::make_shared<color_profile_raw>(signature("prof"), bytes));
  REQUIRE(colour); // The current relative-colourimetric directions need no CMM.
  const GainMapRGB signal{0.25, 0.5, 0.75};
  auto decoded = colour->decode(signal);
  auto encoded = colour->encode(signal);
  REQUIRE(decoded);
  REQUIRE(encoded);
  for (size_t c = 0; c < 3; ++c) {
    REQUIRE((*decoded)[c] == Catch::Approx(signal[c]).margin(1e-12));
    REQUIRE((*encoded)[c] == Catch::Approx(signal[c]).margin(1e-12));
  }
}

TEST_CASE("RGB ICC LUT chromaticity defines application primaries without XYZ colourants")
{
  const auto profile_class = GENERATE(cmsSigDisplayClass, cmsSigColorSpaceClass);
  const bool use_base = GENERATE(false, true);
  auto baseline = GainMapColour::from_icc(rgb_lut_profile(4.3, 2, false, true, profile_class, true));
  auto alternate = GainMapColour::from_icc(rgb_lut_profile(4.3, 1, false, true, profile_class, true));
  REQUIRE(baseline);
  REQUIRE(alternate);
  REQUIRE(baseline->has_application_primaries());
  REQUIRE(alternate->has_application_primaries());
  auto linear = baseline->decode({0.25, 0.5, 0.75});
  auto signal = baseline->encode({0.0625, 0.25, 0.5625});
  REQUIRE(linear);
  REQUIRE(signal);
  for (size_t c = 0; c < 3; ++c) {
    const double expected = static_cast<double>(c + 1) * 0.25;
    REQUIRE((*linear)[c] == Catch::Approx(expected * expected).margin(0.0001));
    REQUIRE((*signal)[c] == Catch::Approx(expected).margin(0.0001));
  }
  auto base = std::make_shared<HeifPixelImage>();
  base->create(1, 1, heif_colorspace_RGB, heif_chroma_444);
  GainMapMetadata metadata;
  metadata.channel_count = 3;
  metadata.use_base_colour_space = use_base;
  for (size_t c = 0; c < 3; ++c) {
    const auto channel = static_cast<heif_channel>(heif_channel_R + c);
    REQUIRE_FALSE(base->add_channel(channel, 1, 1, 16, nullptr));
    base->fill_channel(channel, 16384);
    metadata.channels[c].gain_map_min = {static_cast<int32_t>(c), 1};
    metadata.channels[c].gain_map_max = {static_cast<int32_t>(c), 1};
    metadata.channels[c].base_offset = {1, 8};
    metadata.channels[c].alternate_offset = {1, 16};
  }
  auto gain = std::make_shared<HeifPixelImage>();
  gain->create(1, 1, heif_colorspace_monochrome, heif_chroma_monochrome);
  REQUIRE_FALSE(gain->add_channel(heif_channel_Y, 1, 1, 8, nullptr));
  gain->fill_channel(heif_channel_Y, 255);
  auto* options = heif_decoding_options_alloc();
  REQUIRE(options);
  auto result = reconstruct_tone_map(base, gain, metadata, *alternate, *options, nullptr, *baseline);
  REQUIRE(result);
  for (size_t c = 0; c < 3; ++c) {
    const double expected = (std::pow(16384.0 / 65535, 2) + 0.125) * std::exp2(double(c)) - 0.0625;
    const auto channel = static_cast<heif_channel>(heif_channel_R + c);
    REQUIRE((*result)->get_channel_memory<uint16_t>(channel, nullptr)[0] ==
            Catch::Approx(std::round(expected * 65535)).margin(8));
  }
  heif_decoding_options_free(options);
}

TEST_CASE("ICC chromaticity needs a valid explicit adaptation and nonsingular basis")
{
  const int variant = GENERATE(0, 1, 2, 3, 4, 5, 6);
  using Profile = std::unique_ptr<void, decltype(&cmsCloseProfile)>;
  const auto source = rgb_lut_profile(4.3, 2, false, true, cmsSigDisplayClass, true);
  Profile profile(cmsOpenProfileFromMem(source->get_data().data(),
                  static_cast<cmsUInt32Number>(source->get_data().size())), cmsCloseProfile);
  REQUIRE(profile);
  std::vector<uint8_t> chrm(36);
  REQUIRE(cmsReadRawTag(profile.get(), cmsSigChromaticityTag, chrm.data(), 36) == 36);
  if (variant == 0) { chrm.resize(20); }
  if (variant == 1) { set_u32(chrm, 0, signature("XYZ ")); }
  if (variant == 2) { chrm[9] = 2; }
  if (variant == 3) { chrm[11] = 7; }
  if (variant == 4) {
    for (size_t i = 0; i < 8; ++i) { chrm[20 + i] = chrm[28 + i] = chrm[12 + i]; }
  }
  REQUIRE(cmsWriteRawTag(profile.get(), cmsSigChromaticityTag, chrm.data(),
                          static_cast<cmsUInt32Number>(chrm.size())));
  if (variant == 5) {
    std::vector<uint8_t> chad(44);
    set_u32(chad, 0, signature("sf32"));
    REQUIRE(cmsWriteRawTag(profile.get(), cmsSigChromaticAdaptationTag, chad.data(), 44));
  }
  if (variant == 6) { REQUIRE(cmsWriteTag(profile.get(), cmsSigChromaticAdaptationTag, nullptr)); }
  cmsUInt32Number size = 0;
  REQUIRE(cmsSaveProfileToMem(profile.get(), nullptr, &size));
  std::vector<uint8_t> bytes(size);
  REQUIRE(cmsSaveProfileToMem(profile.get(), bytes.data(), &size));
  auto colour = GainMapColour::from_icc(std::make_shared<color_profile_raw>(signature("prof"), bytes));
  if (variant == 6) {
    REQUIRE(colour);
    REQUIRE_FALSE(colour->has_application_primaries());
    REQUIRE(colour->decode({0.25, 0.5, 0.75}));
  }
  else {
    REQUIRE_FALSE(colour);
    REQUIRE(colour.error().error_code ==
        (variant < 3 ? heif_error_Invalid_input : heif_error_Unsupported_feature));
  }
}

TEST_CASE("ICC LUT PCS conversions need primaries only on the selected application space")
{
  const double version = GENERATE(2.1, 4.3);
  const auto profile_class = GENERATE(cmsSigDisplayClass, cmsSigColorSpaceClass);
  const bool missing_base = GENERATE(false, true);
  const auto profile = rgb_lut_profile(version, 2, true, true, profile_class);
  // Keep the real LUTs, omitting only the three optional RGB colourant tags.
  auto pcs = GainMapColour::from_icc(rgb_lut_profile(version, 2, false, true, profile_class));
  auto rgb = GainMapColour::from_icc(profile);
  INFO((pcs ? "PCS transform supported" : pcs.error().message));
  REQUIRE(pcs);
  REQUIRE(rgb);
  REQUIRE_FALSE(pcs->has_application_primaries());
  REQUIRE(rgb->has_application_primaries());
  auto base = std::make_shared<HeifPixelImage>();
  base->create(1, 1, heif_colorspace_RGB, heif_chroma_444);
  for (auto channel : {heif_channel_R, heif_channel_G, heif_channel_B}) {
    REQUIRE_FALSE(base->add_channel(channel, 1, 1, 16, nullptr));
    base->fill_channel(channel, 16384);
  }
  auto gain = std::make_shared<HeifPixelImage>();
  gain->create(1, 1, heif_colorspace_monochrome, heif_chroma_monochrome);
  REQUIRE_FALSE(gain->add_channel(heif_channel_Y, 1, 1, 8, nullptr));
  gain->fill_channel(heif_channel_Y, 255);
  GainMapMetadata metadata;
  metadata.channel_count = 3;
  metadata.use_base_colour_space = !missing_base;
  for (size_t c = 0; c < 3; ++c) {
    metadata.channels[c].gain_map_min = {static_cast<int32_t>(c), 1};
    metadata.channels[c].gain_map_max = {static_cast<int32_t>(c), 1};
    metadata.channels[c].base_offset = {1, 8};
    metadata.channels[c].alternate_offset = {1, 16};
  }
  const auto& baseline = missing_base ? *pcs : *rgb;
  const auto& alternate = missing_base ? *rgb : *pcs;
  auto* options = heif_decoding_options_alloc();
  REQUIRE(options);
  auto result = reconstruct_tone_map(base, gain, metadata, alternate, *options, nullptr, baseline);
  REQUIRE(result);
  for (size_t c = 0; c < 3; ++c) {
    const double linear = (std::pow(16384.0 / 65535, 2) + 0.125) * std::exp2(double(c)) - 0.0625;
    const auto channel = static_cast<heif_channel>(heif_channel_R + c);
    REQUIRE((*result)->get_channel_memory<uint16_t>(channel, nullptr)[0] ==
            Catch::Approx(std::round(std::sqrt(linear) * 65535)).margin(10));
  }
  metadata.use_base_colour_space = missing_base;
  REQUIRE_FALSE(reconstruct_tone_map(base, gain, metadata, alternate, *options, nullptr, baseline));
  heif_decoding_options_free(options);
  // Decode-only input LUTs must not fall back to their coexisting identity TRCs
  // when an unavailable reverse transform is requested.
  auto input_only = GainMapColour::from_icc(rgb_lut_profile(version, 2, false, false));
  REQUIRE(input_only);
  REQUIRE(input_only->decode({0.25, 0.5, 0.75}));
  REQUIRE_FALSE(input_only->encode({0.0625, 0.25, 0.5625}));
}

TEST_CASE("RGB ICC LUTs retain their application primaries and take precedence over shaper tags")
{
  const double version = GENERATE(2.1, 4.3);
  const auto profile_class = GENERATE(cmsSigDisplayClass, cmsSigColorSpaceClass);
  const auto profile = rgb_lut_profile(version, 2, true, true, profile_class);
  auto colour = GainMapColour::from_icc(profile);
  REQUIRE(colour);
  auto linear = colour->decode({0.25, 0.5, 0.75});
  REQUIRE(linear);
  auto signal = colour->encode({0.0625, 0.25, 0.5625});
  REQUIRE(signal);
  for (size_t c = 0; c < 3; ++c) {
    const double expected = static_cast<double>(c + 1) * 0.25;
    REQUIRE((*linear)[c] == Catch::Approx(expected * expected).margin(0.0001));
    REQUIRE((*signal)[c] == Catch::Approx(expected).margin(0.0001));
  }
  REQUIRE(colour->icc_profile() == profile);
  REQUIRE_FALSE(colour->decode({INFINITY, 0, 0}));
  REQUIRE_FALSE(colour->encode({NAN, 0, 0}));
  auto alternate = GainMapColour::from_icc(rgb_lut_profile(version, 1, true, true, profile_class));
  REQUIRE(alternate);
  const bool use_base = GENERATE(false, true);
  GainMapMetadata metadata;
  metadata.use_base_colour_space = use_base;
  metadata.channel_count = 3;
  auto base = std::make_shared<HeifPixelImage>();
  base->create(1, 1, heif_colorspace_RGB, heif_chroma_444);
  for (size_t c = 0; c < 3; ++c) {
    const auto channel = static_cast<heif_channel>(heif_channel_R + c);
    REQUIRE_FALSE(base->add_channel(channel, 1, 1, 16, nullptr));
    base->fill_channel(channel, 16384);
    metadata.channels[c].gain_map_min = {static_cast<int32_t>(c), 1};
    metadata.channels[c].gain_map_max = {static_cast<int32_t>(c), 1};
    metadata.channels[c].base_offset = {1, 8};
    metadata.channels[c].alternate_offset = {1, 16};
  }
  auto gain = std::make_shared<HeifPixelImage>();
  gain->create(1, 1, heif_colorspace_monochrome, heif_chroma_monochrome);
  REQUIRE_FALSE(gain->add_channel(heif_channel_Y, 1, 1, 8, nullptr));
  gain->fill_channel(heif_channel_Y, 255);
  auto* options = heif_decoding_options_alloc();
  REQUIRE(options);
  auto result = reconstruct_tone_map(base, gain, metadata, *alternate, *options, nullptr, *colour);
  REQUIRE(result);
  for (size_t c = 0; c < 3; ++c) {
    const double expected = (std::pow(16384.0 / 65535, 2) + 0.125) * std::exp2(double(c)) - 0.0625;
    const auto channel = static_cast<heif_channel>(heif_channel_R + c);
    REQUIRE((*result)->get_channel_memory<uint16_t>(channel, nullptr)[0] ==
            Catch::Approx(std::round(expected * 65535)).margin(8));
  }
  REQUIRE((*result)->get_color_profile_icc() == alternate->icc_profile());
  heif_decoding_options_free(options);
  // A LUT without RGB colourants cannot define ISO's application primaries.
  auto invalid_bytes = profile->get_data();
  for (size_t offset = 132; offset + 12 <= invalid_bytes.size(); offset += 12) {
    if (invalid_bytes[offset] == 'r' && invalid_bytes[offset + 1] == 'X' &&
        invalid_bytes[offset + 2] == 'Y' && invalid_bytes[offset + 3] == 'Z') {
      set_u32(invalid_bytes, offset, signature("none"));
      break;
    }
  }
  REQUIRE_FALSE(GainMapColour::from_icc(std::make_shared<color_profile_raw>(signature("prof"), invalid_bytes)));
}
#else
TEST_CASE("RGB ICC LUTs return unsupported when the optional CMM is disabled")
{
  const auto profile_class = GENERATE(cmsSigDisplayClass, cmsSigColorSpaceClass);
  const bool chromaticity = GENERATE(false, true);
  auto colour = GainMapColour::from_icc(rgb_lut_profile(4.3, 2, !chromaticity, true, profile_class, chromaticity));
  REQUIRE_FALSE(colour);
  REQUIRE(colour.error().error_code == heif_error_Unsupported_feature);
}
#endif

TEST_CASE("Defined CICP RGB primaries match independent Little CMS with their native white")
{
  struct ColourDefinition {
    uint16_t primaries;
    cmsCIExyY white;
    cmsCIExyYTRIPLE rgb;
  };
  const auto definition = GENERATE(
      ColourDefinition{1, {0.3127, 0.3290, 1}, {{0.640, 0.330, 1}, {0.300, 0.600, 1}, {0.150, 0.060, 1}}},
      ColourDefinition{4, {0.310, 0.316, 1}, {{0.670, 0.330, 1}, {0.210, 0.710, 1}, {0.140, 0.080, 1}}},
      ColourDefinition{5, {0.3127, 0.3290, 1}, {{0.640, 0.330, 1}, {0.290, 0.600, 1}, {0.150, 0.060, 1}}},
      ColourDefinition{6, {0.3127, 0.3290, 1}, {{0.630, 0.340, 1}, {0.310, 0.595, 1}, {0.155, 0.070, 1}}},
      ColourDefinition{7, {0.3127, 0.3290, 1}, {{0.630, 0.340, 1}, {0.310, 0.595, 1}, {0.155, 0.070, 1}}},
      ColourDefinition{8, {0.310, 0.316, 1}, {{0.681, 0.319, 1}, {0.243, 0.692, 1}, {0.145, 0.049, 1}}},
      ColourDefinition{9, {0.3127, 0.3290, 1}, {{0.708, 0.292, 1}, {0.170, 0.797, 1}, {0.131, 0.046, 1}}},
      ColourDefinition{11, {0.314, 0.351, 1}, {{0.680, 0.320, 1}, {0.265, 0.690, 1}, {0.150, 0.060, 1}}},
      ColourDefinition{12, {0.3127, 0.3290, 1}, {{0.680, 0.320, 1}, {0.265, 0.690, 1}, {0.150, 0.060, 1}}},
      ColourDefinition{22, {0.3127, 0.3290, 1}, {{0.630, 0.340, 1}, {0.295, 0.605, 1}, {0.155, 0.077, 1}}});
  using Profile = std::unique_ptr<void, decltype(&cmsCloseProfile)>;
  using Curve = std::unique_ptr<cmsToneCurve, decltype(&cmsFreeToneCurve)>;
  using Transform = std::unique_ptr<void, decltype(&cmsDeleteTransform)>;
  Curve identity(cmsBuildGamma(nullptr, 1), cmsFreeToneCurve);
  REQUIRE(identity);
  cmsToneCurve* curves[3] = {identity.get(), identity.get(), identity.get()};
  const cmsCIExyY target_white{0.3127, 0.3290, 1};
  const cmsCIExyYTRIPLE target_rgb{{0.640, 0.330, 1}, {0.300, 0.600, 1}, {0.150, 0.060, 1}};
  Profile source(cmsCreateRGBProfile(&definition.white, &definition.rgb, curves), cmsCloseProfile);
  Profile target(cmsCreateRGBProfile(&target_white, &target_rgb, curves), cmsCloseProfile);
  REQUIRE(source);
  REQUIRE(target);
  Transform cmm(cmsCreateTransform(source.get(), TYPE_RGB_DBL, target.get(), TYPE_RGB_DBL,
                                   INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOOPTIMIZE | cmsFLAGS_NOCACHE),
                  cmsDeleteTransform);
  REQUIRE(cmm);
  auto matrix = gain_map_primaries_matrix(definition.primaries, 1);
  REQUIRE(matrix);
  for (const GainMapRGB signal : {GainMapRGB{0, 0, 0}, GainMapRGB{1, 1, 1},
                                  GainMapRGB{0.2, 0.6, 0.8}, GainMapRGB{0.9, 0.7, 0.1},
                                  GainMapRGB{1, 0, 0}, GainMapRGB{0.5, 0.5, 0.5}}) {
    GainMapRGB reference{};
    cmsDoTransform(cmm.get(), signal.data(), reference.data(), 1);
    const auto actual = gain_map_transform(*matrix, signal);
    for (size_t c = 0; c < 3; ++c) {
      REQUIRE(actual[c] == Catch::Approx(reference[c]).margin(0.0001));
    }
  }
}

TEST_CASE("Exact ICC matrix TRC conversions match independent Little CMS in both directions")
{
  const bool srgb = GENERATE(false, true);
  const uint16_t target_code = GENERATE(uint16_t{9}, uint16_t{4}, uint16_t{11});
  using Profile = std::unique_ptr<void, decltype(&cmsCloseProfile)>;
  using Curve = std::unique_ptr<cmsToneCurve, decltype(&cmsFreeToneCurve)>;
  using Transform = std::unique_ptr<void, decltype(&cmsDeleteTransform)>;
  const cmsCIExyY white{0.3127, 0.3290, 1};
  const cmsCIExyYTRIPLE source_primaries{{0.64, 0.33, 1}, {0.21, 0.71, 1}, {0.15, 0.06, 1}};
  cmsCIExyY target_white = white;
  cmsCIExyYTRIPLE target_primaries{{0.708, 0.292, 1}, {0.170, 0.797, 1}, {0.131, 0.046, 1}};
  if (target_code == 4) {
    target_white = {0.310, 0.316, 1};
    target_primaries = {{0.67, 0.33, 1}, {0.21, 0.71, 1}, {0.14, 0.08, 1}};
  }
  else if (target_code == 11) {
    target_white = {0.314, 0.351, 1};
    target_primaries = {{0.680, 0.320, 1}, {0.265, 0.690, 1}, {0.150, 0.060, 1}};
  }
  Curve red(cmsBuildGamma(nullptr, 1.8), cmsFreeToneCurve);
  Curve green(cmsBuildGamma(nullptr, 2.0), cmsFreeToneCurve);
  Curve blue(cmsBuildGamma(nullptr, 2.4), cmsFreeToneCurve);
  REQUIRE(red);
  REQUIRE(green);
  REQUIRE(blue);
  cmsToneCurve* source_curves[3] = {red.get(), green.get(), blue.get()};
  Profile original(srgb ? cmsCreate_sRGBProfile() : cmsCreateRGBProfile(&white, &source_primaries, source_curves),
                    cmsCloseProfile);
  REQUIRE(original);
  cmsUInt32Number size = 0;
  REQUIRE(cmsSaveProfileToMem(original.get(), nullptr, &size));
  std::vector<uint8_t> bytes(size);
  REQUIRE(cmsSaveProfileToMem(original.get(), bytes.data(), &size));
  // Both implementations read exactly the serialized colourants/curve values.
  Profile source(cmsOpenProfileFromMem(bytes.data(), size), cmsCloseProfile);
  REQUIRE(source);
  Curve identity(cmsBuildGamma(nullptr, 1), cmsFreeToneCurve);
  REQUIRE(identity);
  cmsToneCurve* target_curves[3] = {identity.get(), identity.get(), identity.get()};
  Profile target(cmsCreateRGBProfile(&target_white, &target_primaries, target_curves), cmsCloseProfile);
  REQUIRE(target);
  Transform forward(cmsCreateTransform(source.get(), TYPE_RGB_DBL, target.get(), TYPE_RGB_DBL,
                                       INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOOPTIMIZE | cmsFLAGS_NOCACHE), cmsDeleteTransform);
  Transform backward(cmsCreateTransform(target.get(), TYPE_RGB_DBL, source.get(), TYPE_RGB_DBL,
                                        INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOOPTIMIZE | cmsFLAGS_NOCACHE), cmsDeleteTransform);
  REQUIRE(forward);
  REQUIRE(backward);
  auto colour = GainMapColour::from_icc(std::make_shared<color_profile_raw>(signature("prof"), bytes));
  REQUIRE(colour);
  nclx_profile target_nclx;
  target_nclx.set_colour_primaries(target_code);
  target_nclx.set_transfer_characteristics(8);
  const GainMapColour target_colour(target_nclx);
  auto to_target = colour->matrix_to(target_colour);
  auto from_target = target_colour.matrix_to(*colour);
  REQUIRE(to_target);
  REQUIRE(from_target);
  for (const GainMapRGB signal : {GainMapRGB{0, 0, 0}, GainMapRGB{1, 1, 1},
                                  GainMapRGB{0.2, 0.6, 0.8}, GainMapRGB{0.9, 0.7, 0.1},
                                  GainMapRGB{1, 0, 0}, GainMapRGB{0.5, 0.5, 0.5}}) {
    GainMapRGB reference{};
    cmsDoTransform(forward.get(), signal.data(), reference.data(), 1);
    auto decoded = colour->decode(signal);
    REQUIRE(decoded);
    const auto actual = gain_map_transform(*to_target, *decoded);
    GainMapRGB reverse_reference{};
    cmsDoTransform(backward.get(), reference.data(), reverse_reference.data(), 1);
    auto encoded = colour->encode(gain_map_transform(*from_target, reference));
    REQUIRE(encoded);
    for (size_t c = 0; c < 3; ++c) {
      // ICC's finite matrix encoding and CMM floating-point precision account
      // for small differences in the D65-to-D50 adaptation.
      REQUIRE(actual[c] == Catch::Approx(reference[c]).margin(0.0001));
      REQUIRE((*encoded)[c] == Catch::Approx(reverse_reference[c]).margin(0.0001));
    }
  }
}
#endif
