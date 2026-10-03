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
#include <cstring>
#include <memory>

namespace {
Error unsupported(const char* message)
{
  return {heif_error_Unsupported_feature, heif_suberror_Unsupported_color_conversion, message};
}

Result<std::shared_ptr<HeifPixelImage>> decode_ycbcr(
    const std::shared_ptr<HeifPixelImage>& image,
    const heif_decoding_options& options, const heif_security_limits* limits)
{
  auto profile = image->get_color_profile_nclx();
  const auto matrix = profile.m_matrix_coefficients;
  const bool constant_luminance = matrix == 10 || matrix == 13;
  const bool lms_matrix = matrix == 14 || matrix == 15;
  const bool code_matrix = matrix == 0 || matrix == 8 || matrix == 16 || matrix == 17;
  const bool linear_matrix = !code_matrix && !constant_luminance && !lms_matrix && matrix != 11;
  const bool extended = profile.m_transfer_characteristics == 11 || profile.m_transfer_characteristics == 12 ||
                        (profile.m_transfer_characteristics == 13 && matrix != 0);
  const int y_bits = image->get_bits_per_pixel(heif_channel_Y);
  const int c_bits = image->get_bits_per_pixel(heif_channel_Cb);
  const int cr_bits = image->get_bits_per_pixel(heif_channel_Cr);
  if (y_bits < 1 || c_bits < 1 || cr_bits < 1) {
    return unsupported("Tone-map matrix requires three colour components");
  }
  const int rgb_bits = y_bits - (matrix == 16 ? 2 : matrix == 17 ? 1 : 0);
  if (rgb_bits < 1 || (!profile.get_full_range_flag() &&
                      (rgb_bits < 8 || (!code_matrix && (c_bits < 8 || cr_bits < 8)))) ||
      (code_matrix && matrix != 0 && c_bits != cr_bits) ||
      (matrix == 8 && c_bits != y_bits && c_bits != y_bits + 1)) {
    return unsupported("Unsupported tone-map matrix bit depths");
  }
  const auto chroma = image->get_chroma_format();
  if (chroma != heif_chroma_444 &&
      options.color_conversion_options.only_use_preferred_chroma_algorithm &&
      options.color_conversion_options.preferred_chroma_upsampling_algorithm !=
          heif_chroma_upsampling_nearest_neighbor) {
    if (options.color_conversion_options.preferred_chroma_upsampling_algorithm != heif_chroma_upsampling_bilinear) {
      return unsupported("Unsupported mandatory tone-map chroma upsampling algorithm");
    }
  }
  // Constant-luminance chroma depends on the actual transfer, not just Kr/Kb.
  // Use the normalized matrix transfer before any display OOTF when deriving
  // the H.273 Eq.72-75 constants and solving its linear luminance equation.
  std::array<double, 6> cl{}; // Kr, Kb, NB, PB, NR, PR
  if (constant_luminance) {
    if (matrix == 13 && !get_colour_primaries(profile.m_colour_primaries).defined) {
      return unsupported("Constant-luminance raster has no defined primaries");
    }
    const auto weights = get_Kr_Kb(matrix, profile.m_colour_primaries);
    cl[0] = matrix == 10 ? 0.2627 : weights.Kr;
    cl[1] = matrix == 10 ? 0.0593 : weights.Kb;
    if (cl[0] < 0 || cl[1] < 0 || cl[0] + cl[1] >= 1) {
      return unsupported("Invalid constant-luminance primary weights");
    }
    const std::array<double, 4> fractions{1 - cl[1], cl[1], 1 - cl[0], cl[0]};
    for (size_t c = 0; c < fractions.size(); ++c) {
      auto encoded = gain_map_encode_matrix_signal(fractions[c], profile.m_transfer_characteristics);
      if (!encoded) { return encoded.error(); }
      cl[c + 2] = c % 2 == 0 ? *encoded : 1 - *encoded;
      if (!std::isfinite(cl[c + 2]) || cl[c + 2] <= 0) {
        return unsupported("Invalid constant-luminance transfer endpoint");
      }
    }
  }
  const auto linear_weights = linear_matrix ? get_Kr_Kb(matrix, profile.m_colour_primaries) : Kr_Kb{};
  const uint32_t width = image->get_width(), height = image->get_height();
  auto output = std::make_shared<HeifPixelImage>();
  output->create(width, height, heif_colorspace_RGB, heif_chroma_444);
  const std::array<heif_channel, 3> in_channels{heif_channel_Y, heif_channel_Cb, heif_channel_Cr};
  const std::array<heif_channel, 3> out_channels{heif_channel_R, heif_channel_G, heif_channel_B};
  std::array<const uint8_t*, 3> input{};
  std::array<double*, 3> out{};
  std::array<size_t, 3> in_stride{}, out_stride{};
  for (size_t c = 0; c < 3; ++c) {
    input[c] = image->get_channel_memory(in_channels[c], &in_stride[c]);
    if (auto error = output->add_channel(out_channels[c], width, height, 64, limits, heif_component_datatype_floating_point)) {
      return error;
    }
    out[c] = output->get_channel_memory<double>(out_channels[c], &out_stride[c]);
    out_stride[c] /= sizeof(double);
  }
  const double rgb_max = (1U << rgb_bits) - 1;
  const double rgb_offset = profile.get_full_range_flag() ? 0 : 16.0 * (1U << (rgb_bits - 8));
  const double rgb_scale = profile.get_full_range_flag() ? rgb_max : 219.0 * (1U << (rgb_bits - 8));
  const int32_t c_mid = 1U << (c_bits - 1);
  const int32_t cr_mid = 1U << (cr_bits - 1);
  const double c_scale = code_matrix ? 1 :
                        profile.get_full_range_flag() ? (1U << c_bits) - 1 :
                                                        224.0 * (1U << (c_bits - 8));
  const double cr_scale = code_matrix ? 1 :
                         profile.get_full_range_flag() ? (1U << cr_bits) - 1 :
                                                         224.0 * (1U << (cr_bits - 8));
  const bool bilinear = options.color_conversion_options.preferred_chroma_upsampling_algorithm ==
                        heif_chroma_upsampling_bilinear;
  auto raw_sample = [&](size_t c, uint32_t x, uint32_t y) -> double {
    const auto* row = input[c] + size_t(y) * in_stride[c];
    const int bits = c == 0 ? y_bits : c == 1 ? c_bits : cr_bits;
    return bits <= 8 ? row[x] : reinterpret_cast<const uint16_t*>(row)[x];
  };
  auto chroma_sample = [&](size_t c, uint32_t x, uint32_t y) -> int32_t {
    if (chroma == heif_chroma_444) { return static_cast<int32_t>(raw_sample(c, x, y)); }
    if (!bilinear) {
      return static_cast<int32_t>(raw_sample(c, x / 2, chroma == heif_chroma_420 ? y / 2 : y));
    }
    // Centred chroma, with edge extension and a single rounding at its own
    // coded depth. Cb and Cr may have different storage widths (H.273 5.4).
    const double sx = std::clamp((double(x) - 0.5) / 2, 0.0, double((width - 1) / 2));
    const double sy = chroma == heif_chroma_420 ?
        std::clamp((double(y) - 0.5) / 2, 0.0, double((height - 1) / 2)) : y;
    const auto x0 = static_cast<uint32_t>(sx), y0 = static_cast<uint32_t>(sy);
    const auto x1 = std::min(x0 + 1, (width - 1) / 2);
    const auto y1 = std::min(y0 + 1, chroma == heif_chroma_420 ? (height - 1) / 2 : height - 1);
    const double fx = sx - x0, fy = sy - y0;
    const double top = raw_sample(c, x0, y0) * (1 - fx) + raw_sample(c, x1, y0) * fx;
    const double bottom = raw_sample(c, x0, y1) * (1 - fx) + raw_sample(c, x1, y1) * fx;
    return static_cast<int32_t>(std::round(top * (1 - fy) + bottom * fy));
  };
  // Portable arithmetic right shift for negative, odd lifting differences.
  auto half_floor = [](int32_t value) { return value >= 0 ? value / 2 : -((1 - value) / 2); };
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const std::array<int32_t, 3> sample{static_cast<int32_t>(raw_sample(0, x, y)),
                                        chroma_sample(1, x, y), chroma_sample(2, x, y)};
      const int32_t cb = sample[1] - c_mid, cr = sample[2] - cr_mid;
      GainMapRGB signal{};
      if (!code_matrix) {
        // H.273 (2024) Eq.30-38 and 76-78, with independent Y/C depths.
        const double ey = (sample[0] - rgb_offset) / rgb_scale;
        if (linear_matrix) {
          const double kr = linear_weights.Kr, kb = linear_weights.Kb;
          const double r = ey + 2 * (1 - kr) * cr / cr_scale;
          const double b = ey + 2 * (1 - kb) * cb / c_scale;
          signal = {r, (ey - kr * r - kb * b) / (1 - kr - kb), b};
        }
        else if (lms_matrix) {
          auto rgb = gain_map_decode_lms_matrix({ey, cb / c_scale, cr / cr_scale}, matrix,
                                                profile.m_transfer_characteristics);
          if (!rgb) { return rgb.error(); }
          signal = *rgb;
        }
        else if (constant_luminance) {
          // Eq.66-75: restore R'/B', solve linear G from luminance, then
          // reapply the transfer. Applying an NCL matrix here is incorrect.
          const double r = ey + 2 * cr / cr_scale * cl[cr <= 0 ? 4 : 5];
          const double b = ey + 2 * cb / c_scale * cl[cb <= 0 ? 2 : 3];
          const auto transfer = profile.m_transfer_characteristics;
          auto linear_y = gain_map_decode_matrix_signal(ey, transfer);
          auto linear_r = gain_map_decode_matrix_signal(r, transfer);
          auto linear_b = gain_map_decode_matrix_signal(b, transfer);
          if (!linear_y) { return linear_y.error(); }
          if (!linear_r) { return linear_r.error(); }
          if (!linear_b) { return linear_b.error(); }
          auto g = gain_map_encode_matrix_signal(
              (*linear_y - cl[0] * *linear_r - cl[1] * *linear_b) / (1 - cl[0] - cl[1]), transfer);
          if (!g) { return g.error(); }
          signal = {r, *g, b};
        }
        else {
          signal = {2 * cr / cr_scale + 0.991902 * ey, ey,
                    (2 * cb / c_scale + ey) / 0.986566};
        }
      }
      else if (matrix == 0) {
        // Identity matrix stores G, B and R in Y, Cb and Cr (Eq.48-50).
        const GainMapRGB rgb{static_cast<double>(sample[2]), static_cast<double>(sample[0]),
                             static_cast<double>(sample[1])};
        const std::array<int, 3> depths{cr_bits, y_bits, c_bits};
        for (size_t c = 0; c < 3; ++c) {
          const double max = (1U << depths[c]) - 1;
          const double offset = profile.get_full_range_flag() ? 0 : std::ldexp(16.0, depths[c] - 8);
          const double scale = profile.get_full_range_flag() ? max : std::ldexp(219.0, depths[c] - 8);
          signal[c] = (std::clamp(rgb[c], 0.0, max) - offset) / scale;
        }
      }
      else {
        // Eq.54-57 (YCgCo) or Eq.62-65 (YCgCo-R, -Re and -Ro).
        const bool reversible = matrix != 8 || c_bits != y_bits;
        const int32_t t = sample[0] - (reversible ? half_floor(cb) : cb);
        const double g = (reversible ? t : sample[0]) + cb;
        const double b = t - (reversible ? half_floor(cr) : cr);
        // Eq.65 uses B after the Clip3 in Eq.64.
        const double r = reversible ? std::clamp(b, 0.0, rgb_max) + cr : t + cr;
        const GainMapRGB rgb{r, g, b};
        for (size_t c = 0; c < 3; ++c) {
          signal[c] = (std::clamp(rgb[c], 0.0, rgb_max) - rgb_offset) / rgb_scale;
        }
      }
      for (size_t c = 0; c < 3; ++c) {
        if (!std::isfinite(signal[c])) {
          return unsupported("Tone-map matrix produced a non-finite RGB sample");
        }
        // H.273 permits extended E'RGB for TC 11/12 and non-identity TC 13.
        // Keep its sign/precision until the ISO gain and colour conversion;
        // YCgCo's explicitly specified code-domain Clip3 remains above.
        out[c][size_t(y) * out_stride[c] + x] = extended ? signal[c] : std::clamp(signal[c], 0.0, 1.0);
      }
    }
  }
  if (image->has_channel(heif_channel_Alpha)) {
    if (auto error = output->copy_new_channel_from(image, heif_channel_Alpha, heif_channel_Alpha, limits)) {
      return error;
    }
  }
  output->set_premultiplied_alpha(image->is_premultiplied_alpha());
  profile.set_matrix_coefficients(0);
  profile.set_full_range_flag(true);
  output->set_color_profile_nclx(profile);
  return output;
}

bool is_direct_rgb(const HeifPixelImage& image)
{
  return (image.get_colorspace() == heif_colorspace_RGB &&
          (image.get_chroma_format() == heif_chroma_444 ||
           num_interleaved_components_per_plane(image.get_chroma_format()) > 1)) ||
         (image.get_colorspace() == heif_colorspace_monochrome && image.get_chroma_format() == heif_chroma_monochrome);
}

Result<std::shared_ptr<HeifPixelImage>> prepare_rgb(
    const std::shared_ptr<HeifPixelImage>& image,
    const heif_decoding_options& options, const heif_security_limits* limits)
{
  if (auto error = image->check_plane_layout()) { return error; }
  auto profile = image->get_color_profile_nclx();
  const bool direct = is_direct_rgb(*image);
  const auto chroma = image->get_chroma_format();
  const bool word_interleaved = chroma == heif_chroma_interleaved_RRGGBB_BE ||
                               chroma == heif_chroma_interleaved_RRGGBB_LE ||
                               chroma == heif_chroma_interleaved_RRGGBBAA_BE ||
                               chroma == heif_chroma_interleaved_RRGGBBAA_LE;
  for (auto channel : image->get_channel_set()) {
    const uint16_t bits = image->get_bits_per_pixel(channel);
    const auto type = image->get_datatype(channel);
    const bool integer = type == heif_component_datatype_unsigned_integer && bits >= 1 && bits <= (direct ? 64 : 16);
    const bool floating = direct && type == heif_component_datatype_floating_point && (bits == 32 || bits == 64);
    if (!integer && !floating) {
      return unsupported("Unsupported tone-map sample datatype or depth");
    }
    if (word_interleaved && (!integer || bits > 16)) {
      return unsupported("Tone-map RRGGBB layouts require unsigned 16-bit sample storage");
    }
    if (direct && channel != heif_channel_Alpha && !profile.get_full_range_flag() &&
        (floating || bits < 8)) {
      return unsupported("Unsupported tone-map RGB or monochrome limited-range samples");
    }
  }
  // ISO 21496-1 Formula (2) operates on the decoded baseline, which may
  // exceed reference white. Read RGB/mono directly, avoiding both
  // premature clipping and a 16-bit quantization before inverse gamma/gain.
  if (direct) { return image; }
  if (image->get_colorspace() == heif_colorspace_YCbCr) {
    const auto matrix = profile.m_matrix_coefficients;
    // Resolve both inputs without defaulting unspecified matrix/primaries.
    const bool explicit_nonlinear = matrix == 8 || (matrix >= 10 && matrix <= 17 && matrix != 12);
    const bool explicit_linear = matrix == 0 || matrix == 1 || matrix == 4 ||
                                 matrix == 5 || matrix == 6 || matrix == 7 || matrix == 9;
    const bool derived_linear = matrix == 12 && get_colour_primaries(profile.m_colour_primaries).defined;
    if (!explicit_linear && !derived_linear && !explicit_nonlinear) {
      return unsupported("Unsupported tone-map YCbCr matrix coefficients");
    }
    return decode_ycbcr(image, options, limits);
  }
  profile.set_matrix_coefficients(0);
  profile.set_full_range_flag(true);
  return convert_colorspace(image, heif_colorspace_RGB, heif_chroma_444,
                             profile, 16, options.color_conversion_options,
                             nullptr, limits);
}

struct SamplePlane {
  const uint8_t* data = nullptr;
  size_t stride = 0;
  size_t pixel_step = 0;
  int byte_order = 0; // Native, little-endian or big-endian 16-bit interleaved samples.
  int bits = 0;
  heif_component_datatype type = heif_component_datatype_undefined;
  double offset = 0;
  double scale = 1;

  SamplePlane() = default;

  SamplePlane(const HeifPixelImage& image, heif_channel channel, bool full_range, size_t component = 0)
  {
    const auto chroma = image.get_chroma_format();
    if (channel == heif_channel_Alpha && image.has_channel(heif_channel_interleaved) &&
        is_interleaved_with_alpha(chroma)) {
      channel = heif_channel_interleaved;
      component = 3;
    }
    if (!image.has_channel(channel)) { return; }
    data = image.get_channel_memory(channel, &stride);
    bits = image.get_bits_per_pixel(channel);
    type = image.get_datatype(channel);
    const size_t bytes = static_cast<size_t>(bytes_per_sample_for_bit_depth(bits));
    pixel_step = bytes;
    if (channel == heif_channel_interleaved) {
      data += component * bytes;
      pixel_step *= num_interleaved_components_per_plane(chroma);
      if (chroma == heif_chroma_interleaved_RRGGBB_LE || chroma == heif_chroma_interleaved_RRGGBBAA_LE) {
        byte_order = 1;
      }
      else if (chroma == heif_chroma_interleaved_RRGGBB_BE || chroma == heif_chroma_interleaved_RRGGBBAA_BE) {
        byte_order = 2;
      }
    }
    if (type == heif_component_datatype_unsigned_integer) {
      offset = full_range ? 0 : std::ldexp(16.0, bits - 8);
      scale = full_range ? std::ldexp(1.0, bits) - 1 : std::ldexp(219.0, bits - 8);
    }
  }

  template<class T> double read(uint32_t x, uint32_t y) const
  {
    T value;
    std::memcpy(&value, data + size_t(y) * stride + size_t(x) * pixel_step, sizeof(T));
    return static_cast<double>(value);
  }

  double sample(uint32_t x, uint32_t y) const
  {
    if (!data) { return 1; }
    if (byte_order) {
      const auto* p = data + size_t(y) * stride + size_t(x) * pixel_step;
      const uint16_t value = byte_order == 1 ? static_cast<uint16_t>(p[0] | (p[1] << 8)) :
                                              static_cast<uint16_t>((p[0] << 8) | p[1]);
      return (value - offset) / scale;
    }
    if (type == heif_component_datatype_floating_point) {
      return bits == 32 ? read<float>(x, y) : read<double>(x, y);
    }
    const double value = bits <= 8 ? read<uint8_t>(x, y) :
                         bits <= 16 ? read<uint16_t>(x, y) :
                         bits <= 32 ? read<uint32_t>(x, y) : read<uint64_t>(x, y);
    return (value - offset) / scale;
  }
};

struct RGBPlanes {
  std::array<SamplePlane, 3> planes;

  explicit RGBPlanes(const HeifPixelImage& image)
  {
    const bool mono = image.get_colorspace() == heif_colorspace_monochrome;
    const bool interleaved = image.has_channel(heif_channel_interleaved);
    const bool full_range = image.get_color_profile_nclx().get_full_range_flag();
    const std::array<heif_channel, 3> channels{heif_channel_R, heif_channel_G, heif_channel_B};
    for (size_t c = 0; c < 3; ++c) {
      planes[c] = SamplePlane(image, interleaved ? heif_channel_interleaved : mono ? heif_channel_Y : channels[c],
                              full_range, interleaved ? c : 0);
    }
  }

  double sample(size_t c, uint32_t x, uint32_t y) const { return planes[c].sample(x, y); }
};

Error copy_alpha(const std::shared_ptr<HeifPixelImage>& output,
                 const std::shared_ptr<HeifPixelImage>& input, const heif_security_limits* limits,
                 bool normalize_depth)
{
  if (!input->has_alpha()) { return Error::Ok; }
  if (input->get_datatype(heif_channel_Alpha) == heif_component_datatype_unsigned_integer &&
      (input->get_bits_per_pixel(heif_channel_Alpha) == 16 ||
       (input->get_bits_per_pixel(heif_channel_Alpha) < 16 && !normalize_depth))) {
    return output->copy_new_channel_from(input, heif_channel_Alpha, heif_channel_Alpha, limits);
  }
  // Canonical output is unsigned RGB16. Quantize typed opacity only here,
  // after its original value has been used for unpremultiplication and gain.
  const uint32_t width = input->get_width(), height = input->get_height();
  if (auto error = output->add_channel(heif_channel_Alpha, width, height, 16, limits)) { return error; }
  const SamplePlane alpha(*input, heif_channel_Alpha, true);
  size_t stride = 0;
  auto* plane = output->get_channel_memory<uint16_t>(heif_channel_Alpha, &stride);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      plane[size_t(y) * (stride / sizeof(uint16_t)) + x] = static_cast<uint16_t>(std::round(alpha.sample(x, y) * 65535));
    }
  }
  return Error::Ok;
}

Error validate_alpha(double alpha)
{
  if (!std::isfinite(alpha) || alpha < 0 || alpha > 1) {
    return {heif_error_Invalid_input, heif_suberror_Unspecified, "Tone-map alpha must be finite and between zero and one"};
  }
  return Error::Ok;
}
}  // namespace

Result<std::shared_ptr<HeifPixelImage>> reconstruct_tone_map(
    const std::shared_ptr<HeifPixelImage>& base,
    const std::shared_ptr<HeifPixelImage>& gain,
    const GainMapMetadata& metadata,
    const GainMapColour& alternate_colour,
    const heif_decoding_options& options,
    const heif_security_limits* limits,
    const std::optional<GainMapColour>& baseline_colour_override,
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
  // Codec matrix/range stays on the raster; the item colour description is
  // used only after conversion to RGB sample values.
  const auto baseline_colour = baseline_colour_override.value_or(
      GainMapColour(base->get_color_profile_nclx(), base->get_nominal_diffuse_white_luminance()));
  if (base->is_premultiplied_alpha() && !base->has_alpha()) {
    return Error{heif_error_Invalid_input, heif_suberror_Unspecified,
                 "Premultiplied tone-map baseline has no alpha channel"};
  }
  const auto& application = metadata.use_base_colour_space ? baseline_colour : alternate_colour;
  if (!application.has_application_primaries()) {
    return unsupported("Selected tone-map application space has no defined RGB primaries");
  }
  auto before = baseline_colour.matrix_to(application);
  auto after = application.matrix_to(alternate_colour);
  if (!before) { return before.error(); }
  if (!after) { return after.error(); }

  auto base_rgb = prepare_rgb(base, options, limits);
  auto gain_rgb = prepare_rgb(gain, options, limits);
  if (!base_rgb) { return base_rgb.error(); }
  if (!gain_rgb) { return gain_rgb.error(); }
  const RGBPlanes base_planes(**base_rgb);
  const RGBPlanes gain_planes(**gain_rgb);
  const SamplePlane alpha_plane(**base_rgb, heif_channel_Alpha, true);
  const bool premultiplied = base->is_premultiplied_alpha();
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
      const double opacity = alpha_plane.sample(x, y);
      if (auto error = validate_alpha(opacity)) { return error; }
      const double alpha = premultiplied ? opacity : 1.0;
      GainMapRGB signal{base_planes.sample(0, x, y), base_planes.sample(1, x, y),
                        base_planes.sample(2, x, y)};
      if (premultiplied) {
        // 'prem' describes the main image's sample values (HEIF 6.9.1).
        // Undo it before the nonlinear EOTF and ISO's linear gain operation.
        for (auto& value : signal) { value = alpha > 0 ? value / alpha : 0; }
      }
      auto decoded = baseline_colour.decode(signal);
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
      auto encoded = alternate_colour.encode(linear);
      if (!encoded) { return encoded.error(); }
      for (size_t c = 0; c < 3; ++c) {
        out[c][size_t(y) * strides[c] + x] = static_cast<uint16_t>(
            std::round(std::clamp((*encoded)[c], 0.0, 1.0) * alpha * 65535.0));
      }
    }
  }
  if (auto error = copy_alpha(output, *base_rgb, limits, base->get_colorspace() == heif_colorspace_RGB)) { return error; }
  output->set_premultiplied_alpha(premultiplied);
  output->set_color_profile_nclx(alternate_colour.raster_profile());
  output->set_color_profile_icc(alternate_colour.icc_profile());
  if (alternate_colour.nominal_diffuse_white()) {
    output->set_nominal_diffuse_white_luminance(alternate_colour.nominal_diffuse_white());
  }
  return output;
}

Result<std::shared_ptr<HeifPixelImage>> convert_tone_map_colour(
    const std::shared_ptr<HeifPixelImage>& image,
    const heif_color_profile_nclx& requested,
    const heif_decoding_options& options,
    const heif_security_limits* limits)
{
  if (!limits) { limits = &global_security_limits; }
  auto source = image->get_color_profile_nclx();
  GainMapColour source_colour(source, image->get_nominal_diffuse_white_luminance());
  if (image->get_color_profile_icc()) {
    auto resolved = GainMapColour::from_icc(image->get_color_profile_icc(),
                                          image->get_nominal_diffuse_white_luminance());
    if (!resolved) { return resolved.error(); }
    source_colour = *resolved;
  }
  const auto source_description = source_colour.raster_profile();
  auto target = source_description;
  if (requested.color_primaries != heif_color_primaries_unspecified) {
    target.set_colour_primaries(requested.color_primaries);
  }
  if (requested.transfer_characteristics != heif_transfer_characteristic_unspecified) {
    target.set_transfer_characteristics(requested.transfer_characteristics);
  }
  if (source_description.m_colour_primaries == target.m_colour_primaries &&
      source_description.m_transfer_characteristics == target.m_transfer_characteristics) {
    return image;
  }
  if (!gain_map_supports_transfer(target.m_transfer_characteristics)) {
    return unsupported("Unsupported tone-map requested-output transfer function");
  }
  const GainMapColour target_colour(target, image->get_nominal_diffuse_white_luminance());
  auto matrix = source_colour.matrix_to(target_colour);
  if (!matrix) { return matrix.error(); }
  if (image->is_premultiplied_alpha() && !image->has_alpha()) {
    return Error{heif_error_Invalid_input, heif_suberror_Unspecified,
                 "Premultiplied tone-map output has no alpha channel"};
  }
  auto rgb = prepare_rgb(image, options, limits);
  if (!rgb) { return rgb.error(); }
  const RGBPlanes input(**rgb);
  const SamplePlane alpha_plane(**rgb, heif_channel_Alpha, true);
  const bool premultiplied = image->is_premultiplied_alpha();
  const uint32_t width = image->get_width(), height = image->get_height();
  auto output = std::make_shared<HeifPixelImage>();
  output->create(width, height, heif_colorspace_RGB, heif_chroma_444);
  output->copy_metadata_from(*image);
  // The old ICC describes the original samples, not this requested encoding.
  output->set_color_profile_icc(nullptr);
  output->add_warnings(image->get_warnings());
  const std::array<heif_channel, 3> channels{heif_channel_R, heif_channel_G, heif_channel_B};
  std::array<uint16_t*, 3> out{};
  std::array<size_t, 3> strides{};
  for (size_t c = 0; c < 3; ++c) {
    if (auto error = output->add_channel(channels[c], width, height, 16, limits)) { return error; }
    out[c] = output->get_channel_memory<uint16_t>(channels[c], &strides[c]);
    strides[c] /= sizeof(uint16_t);
  }
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const double opacity = alpha_plane.sample(x, y);
      if (auto error = validate_alpha(opacity)) { return error; }
      const double alpha = premultiplied ? opacity : 1.0;
      GainMapRGB signal{input.sample(0, x, y), input.sample(1, x, y), input.sample(2, x, y)};
      if (premultiplied) {
        for (auto& value : signal) { value = alpha > 0 ? value / alpha : 0; }
      }
      auto linear = source_colour.decode(signal);
      if (!linear) { return linear.error(); }
      auto encoded = target_colour.encode(gain_map_transform(*matrix, *linear));
      if (!encoded) { return encoded.error(); }
      for (size_t c = 0; c < 3; ++c) {
        out[c][size_t(y) * strides[c] + x] = static_cast<uint16_t>(
            std::round(std::clamp((*encoded)[c], 0.0, 1.0) * alpha * 65535.0));
      }
    }
  }
  if (auto error = copy_alpha(output, *rgb, limits, image->get_colorspace() == heif_colorspace_RGB)) { return error; }
  target.set_matrix_coefficients(0);
  target.set_full_range_flag(true);
  output->set_color_profile_nclx(target);
  return output;
}
