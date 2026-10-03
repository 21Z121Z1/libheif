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

TEST_CASE("ICC CICP tag maps Display-P3 PQ directly")
{
  const auto bytes = make_icc({
      {signature("cicp"), cicp_tag(12, 16, 0, 1)}
  });
  const color_profile_raw profile(signature("prof"), bytes);
  auto result = gain_map_nclx_from_icc(profile);
  REQUIRE(result);
  REQUIRE(result->m_colour_primaries == 12);
  REQUIRE(result->m_transfer_characteristics == 16);
  REQUIRE(result->m_matrix_coefficients == 0);
  REQUIRE(result->m_full_range_flag);
}

TEST_CASE("ICC matrix-shaper Display-P3 with sRGB TRC maps to CICP")
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

  const color_profile_raw profile(signature("prof"), bytes);
  auto result = gain_map_nclx_from_icc(profile);
  REQUIRE(result);
  REQUIRE(result->m_colour_primaries == 12);
  REQUIRE(result->m_transfer_characteristics == 13);
  REQUIRE(result->m_matrix_coefficients == 0);
  REQUIRE(result->m_full_range_flag);
}

TEST_CASE("ICC tag bounds are checked before colour interpretation")
{
  auto bytes = make_icc({
      {signature("cicp"), cicp_tag(12, 16, 0, 1)}
  });
  set_u32(bytes, 136, 0xFFFFFF00U);
  const color_profile_raw profile(signature("prof"), bytes);
  auto result = gain_map_nclx_from_icc(profile);
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
  const color_profile_raw profile(signature("prof"), bytes);
  auto result = gain_map_nclx_from_icc(profile);
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
  REQUIRE_FALSE(gain_map_nclx_from_icc(*profile));
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

#if LIBHEIF_TEST_LCMS
namespace {
std::shared_ptr<const color_profile_raw> rgb_lut_profile(double version, double gamma)
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
  // D50-scaled XYZ basis makes the intended LUT and application-space values
  // independently known. Deliberately leave the identity TRCs in the profile:
  // selecting those instead of the LUT would miss the gamma entirely.
  const std::array<double, 3> white_xyz{0.9642, 1, 0.8249};
  const std::array<cmsTagSignature, 3> tags{cmsSigRedColorantTag, cmsSigGreenColorantTag, cmsSigBlueColorantTag};
  std::array<double, 9> to_pcs{}, from_pcs{};
  constexpr double pcs_scale = 65535.0 / 32768;
  for (size_t c = 0; c < 3; ++c) {
    cmsCIEXYZ xyz{};
    if (c == 0) { xyz.X = white_xyz[c]; }
    if (c == 1) { xyz.Y = white_xyz[c]; }
    if (c == 2) { xyz.Z = white_xyz[c]; }
    REQUIRE(cmsWriteTag(profile.get(), tags[c], &xyz));
    to_pcs[c * 3 + c] = white_xyz[c] / pcs_scale;
    from_pcs[c * 3 + c] = pcs_scale / white_xyz[c];
  }
  for (bool inverse : {false, true}) {
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
            const auto* diagonal = static_cast<const double*>(cargo);
            for (size_t c = 0; c < 3; ++c) {
              out[c] = static_cast<uint16_t>(std::round(std::clamp(in[c] * diagonal[c * 3 + c], 0.0, 65535.0)));
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
  }
  cmsUInt32Number size = 0;
  REQUIRE(cmsSaveProfileToMem(profile.get(), nullptr, &size));
  std::vector<uint8_t> bytes(size);
  REQUIRE(cmsSaveProfileToMem(profile.get(), bytes.data(), &size));
  return std::make_shared<color_profile_raw>(signature("prof"), bytes);
}
}  // namespace

#if LIBHEIF_HAVE_LCMS2
TEST_CASE("RGB ICC LUTs retain their application primaries and take precedence over shaper tags")
{
  const double version = GENERATE(2.1, 4.3);
  const auto profile = rgb_lut_profile(version, 2);
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
  auto alternate = GainMapColour::from_icc(rgb_lut_profile(version, 1));
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
  auto colour = GainMapColour::from_icc(rgb_lut_profile(4.3, 2));
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
