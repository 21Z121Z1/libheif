/*
 * HEIF codec.
 * Copyright (c) 2026 libheif contributors
 *
 * This file is part of libheif.
 *
 * libheif is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * libheif is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libheif.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "gain_map_reconstruction.h"

#include "gain_map_color.h"
#include "gain_map_math.h"
#include "color-conversion/colorconversion.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

namespace {
Error unsupported(const char* message)
{
  return {heif_error_Unsupported_feature, heif_suberror_Unsupported_color_conversion, message};
}

Result<std::shared_ptr<HeifPixelImage>> to_rgb16(
    const std::shared_ptr<HeifPixelImage>& image,
    const heif_decoding_options& options, const heif_security_limits* limits, bool gain_samples)
{
  if (auto error = image->check_plane_layout()) {
    return error;
  }
  for (auto channel : image->get_channel_set()) {
    const uint16_t bits = image->get_bits_per_pixel(channel);
    if (image->get_datatype(channel) != heif_component_datatype_unsigned_integer ||
        bits > 16) {
      return unsupported("Tone-map inputs require unsigned samples of at most 16 bits");
    }

  }
  auto profile = image->get_color_profile_nclx();
  if (gain_samples && image->get_colorspace() == heif_colorspace_monochrome) {
    const uint32_t w = image->get_width(), h = image->get_height();
    const int bits = image->get_bits_per_pixel(heif_channel_Y);
    if (bits < 1 || bits > 16 || (!profile.get_full_range_flag() && bits < 8)) {
      return unsupported("Unsupported monochrome gain-map bit depth");
    }
    auto rgb = std::make_shared<HeifPixelImage>();
    rgb->create(w, h, heif_colorspace_RGB, heif_chroma_444);
    size_t input_stride = 0;
    const auto* input = image->get_channel_memory(heif_channel_Y, &input_stride);
    // ISO 21496-1 requires at least 8 bits per gain-map component.
    // The reader deliberately tolerates non-conforming lower-depth full-range
    // mono samples because their normalization is still unambiguous.
    const double offset = profile.get_full_range_flag() ? 0 : 16.0 * (1U << (bits - 8));
    const double scale = profile.get_full_range_flag() ? static_cast<double>((1U << bits) - 1) :
                                                       219.0 * (1U << (bits - 8));
    for (auto channel : {heif_channel_R, heif_channel_G, heif_channel_B}) {
      if (auto error = rgb->add_channel(channel, w, h, 16, limits)) {
        return error;
      }
      size_t stride = 0;
      auto* output = rgb->get_channel_memory<uint16_t>(channel, &stride);
      stride /= sizeof(uint16_t);
      for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
          const auto* row = input + size_t(y) * input_stride;
          const double sample = bits <= 8 ? row[x] : reinterpret_cast<const uint16_t*>(row)[x];
          output[size_t(y) * stride + x] = static_cast<uint16_t>(
              std::round(std::clamp((sample - offset) / scale, 0.0, 1.0) * 65535));
        }
      }
    }
    profile.set_matrix_coefficients(0);
    profile.set_full_range_flag(true);
    rgb->set_color_profile_nclx(profile);
    return rgb;
  }
  profile.set_matrix_coefficients(0);
  profile.set_full_range_flag(true);
  return convert_colorspace(image, heif_colorspace_RGB, heif_chroma_444,
                             profile, 16, options.color_conversion_options,
                             nullptr, limits);
}

struct RGBPlanes {
  std::array<const uint16_t*, 3> data{};
  std::array<size_t, 3> stride{};

  explicit RGBPlanes(const HeifPixelImage& image)
  {
    const std::array<heif_channel, 3> channels{heif_channel_R, heif_channel_G, heif_channel_B};
    for (size_t c = 0; c < 3; ++c) {
      data[c] = image.get_channel_memory<uint16_t>(channels[c], &stride[c]);
      stride[c] /= sizeof(uint16_t);
    }
  }

  double sample(size_t c, uint32_t x, uint32_t y) const
  {
    return data[c][size_t(y) * stride[c] + x] / 65535.0;
  }
};
}  // namespace

Result<std::shared_ptr<HeifPixelImage>> reconstruct_tone_map(
    const std::shared_ptr<HeifPixelImage>& base,
    const std::shared_ptr<HeifPixelImage>& gain,
    const GainMapMetadata& metadata,
    const nclx_profile& alternate,
    const heif_decoding_options& options,
    const heif_security_limits* limits,
    std::optional<nclx_profile> baseline_colour_override,
    std::optional<double> target_headroom)
{
  if (!limits) {
    limits = &global_security_limits;
  }
  if (auto error = validate_gain_map_metadata(metadata)) {
    return error;
  }
  double weight = gain_map_full_weight(metadata);
  if (target_headroom) {
    auto target_weight = gain_map_target_weight(metadata, *target_headroom);
    if (!target_weight) { return target_weight.error(); }
    weight = *target_weight;
  }
  if (!base || !gain || !base->get_width() || !base->get_height() ||
      !gain->get_width() || !gain->get_height()) {
    return Error{heif_error_Invalid_input, heif_suberror_Invalid_image_size,
                 "Tone-map inputs have no usable dimensions"};
  }
  if (auto error = check_for_valid_image_size(limits, base->get_width(), base->get_height())) {
    return error;
  }
  if (auto error = check_for_valid_image_size(limits, gain->get_width(), gain->get_height())) {
    return error;
  }
  auto baseline = base->get_color_profile_nclx();
  if (baseline_colour_override) {
    // The decoded raster still carries the codec's matrix/range signalling.
    // Item-level ICC/NCLX colourimetry supplies the RGB primaries and transfer
    // function used by ISO 21496 without overwriting those storage semantics.
    baseline.m_colour_primaries =
        baseline_colour_override->m_colour_primaries;
    baseline.m_transfer_characteristics =
        baseline_colour_override->m_transfer_characteristics;
  }
  if (!baseline.is_defined() || !alternate.is_defined()) {
    return unsupported("Tone-map reconstruction has no supported colour description");
  }
  if (!gain_map_supports_transfer(baseline.m_transfer_characteristics) ||
      !gain_map_supports_transfer(alternate.m_transfer_characteristics)) {
    return unsupported("Unsupported tone-map transfer function");
  }
  // Matrix interpretation cannot be guessed from unspecified primaries.
  const uint16_t gain_matrix = gain->get_color_profile_nclx().m_matrix_coefficients;
  if (gain->get_colorspace() == heif_colorspace_YCbCr &&
      gain_matrix != 0 && gain_matrix != 1 && gain_matrix != 5 &&
      gain_matrix != 6 && gain_matrix != 9) {
    return unsupported("Unsupported gain-map YCbCr matrix coefficients");
  }
  if (base->is_premultiplied_alpha()) {
    return unsupported("Premultiplied tone-map baseline requires unpremultiplication");
  }
  const uint16_t application = metadata.use_base_colour_space ?
                               baseline.m_colour_primaries : alternate.m_colour_primaries;
  auto before = gain_map_primaries_matrix(baseline.m_colour_primaries, application);
  auto after = gain_map_primaries_matrix(application, alternate.m_colour_primaries);
  if (!before) { return before.error(); }
  if (!after) { return after.error(); }

  auto base_rgb = to_rgb16(base, options, limits, false);
  auto gain_rgb = to_rgb16(gain, options, limits, true);
  if (!base_rgb) { return base_rgb.error(); }
  if (!gain_rgb) { return gain_rgb.error(); }
  if ((*base_rgb)->get_colorspace() != heif_colorspace_RGB ||
      (*gain_rgb)->get_colorspace() != heif_colorspace_RGB ||
      (*base_rgb)->get_chroma_format() != heif_chroma_444 ||
      (*gain_rgb)->get_chroma_format() != heif_chroma_444) {
    return unsupported("Tone-map RGB conversion did not produce planar RGB");
  }
  const RGBPlanes base_planes(**base_rgb);
  const RGBPlanes gain_planes(**gain_rgb);
  const uint32_t width = base->get_width(), height = base->get_height();
  const uint32_t gain_width = gain->get_width(), gain_height = gain->get_height();
  if (auto error = check_for_valid_image_size(limits, width, height)) {
    return error;
  }
  auto output = std::make_shared<HeifPixelImage>();
  output->create(width, height, heif_colorspace_RGB, heif_chroma_444);
  const std::array<heif_channel, 3> channels{heif_channel_R, heif_channel_G, heif_channel_B};
  std::array<uint16_t*, 3> out{};
  std::array<size_t, 3> strides{};
  for (size_t c = 0; c < 3; ++c) {
    if (auto error = output->add_channel(channels[c], width, height, 16, limits)) {
      return error;
    }
    out[c] = output->get_channel_memory<uint16_t>(channels[c], &strides[c]);
    strides[c] /= sizeof(uint16_t);
  }
  // Co-sited bilinear interpolation, with edge extension. Unnormalize each
  // of the four gain samples BEFORE interpolation; no full-resolution gain
  // buffer is allocated. All pixel buffers use HeifPixelImage accounting.
  for (uint32_t y = 0; y < height; ++y) {
    const double gy = static_cast<double>(y) * gain_height / height;
    const uint32_t y0 = std::min(static_cast<uint32_t>(gy), gain_height - 1);
    const uint32_t y1 = std::min(y0 + 1, gain_height - 1);
    const double fy = gy - y0;
    for (uint32_t x = 0; x < width; ++x) {
      const double gx = static_cast<double>(x) * gain_width / width;
      const uint32_t x0 = std::min(static_cast<uint32_t>(gx), gain_width - 1);
      const uint32_t x1 = std::min(x0 + 1, gain_width - 1);
      const double fx = gx - x0;
      auto decoded = gain_map_decode_rgb({base_planes.sample(0, x, y),
                                         base_planes.sample(1, x, y),
                                         base_planes.sample(2, x, y)}, baseline);
      if (!decoded) { return decoded.error(); }
      GainMapRGB linear = *decoded;
      linear = gain_map_transform(*before, linear);
      for (size_t c = 0; c < 3; ++c) {
        const auto& channel = metadata.channels[metadata.channel_count == 1 ? 0 : c];
        auto g00 = gain_map_unnormalize(gain_planes.sample(c, x0, y0), channel);
        auto g10 = gain_map_unnormalize(gain_planes.sample(c, x1, y0), channel);
        auto g01 = gain_map_unnormalize(gain_planes.sample(c, x0, y1), channel);
        auto g11 = gain_map_unnormalize(gain_planes.sample(c, x1, y1), channel);
        if (!g00) { return g00.error(); }
        if (!g10) { return g10.error(); }
        if (!g01) { return g01.error(); }
        if (!g11) { return g11.error(); }
        const double top = *g00 * (1 - fx) + *g10 * fx;
        const double bottom = *g01 * (1 - fx) + *g11 * fx;
        auto value = gain_map_apply(linear[c], top * (1 - fy) + bottom * fy, channel, weight);
        if (!value) { return value.error(); }
        linear[c] = *value;
      }
      linear = gain_map_transform(*after, linear);
      auto encoded = gain_map_encode_rgb(linear, alternate);
      if (!encoded) { return encoded.error(); }
      for (size_t c = 0; c < 3; ++c) {
        out[c][size_t(y) * strides[c] + x] = static_cast<uint16_t>(
            std::round(std::clamp((*encoded)[c], 0.0, 1.0) * 65535.0));
      }
    }
  }
  if ((*base_rgb)->has_channel(heif_channel_Alpha)) {
    if (auto error = output->copy_new_channel_from(*base_rgb, heif_channel_Alpha, heif_channel_Alpha, limits)) {
      return error;
    }
  }
  output->set_color_profile_nclx(alternate);
  return output;
}
