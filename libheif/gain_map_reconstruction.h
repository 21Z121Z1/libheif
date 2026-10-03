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

#ifndef LIBHEIF_GAIN_MAP_RECONSTRUCTION_H
#define LIBHEIF_GAIN_MAP_RECONSTRUCTION_H

#include <memory>
#include <optional>

#include "gain_map_metadata.h"
#include "gain_map_color.h"
#include "image/pixelimage.h"

Result<std::shared_ptr<HeifPixelImage>> reconstruct_tone_map(
    const std::shared_ptr<HeifPixelImage>& base,
    const std::shared_ptr<HeifPixelImage>& gain,
    const GainMapMetadata& metadata,
    const GainMapColour& alternate_colour,
    const heif_decoding_options& options,
    const heif_security_limits* limits,
    const std::optional<GainMapColour>& baseline_colour_override = std::nullopt,
    std::optional<double> target_headroom = std::nullopt,
    bool defer_quantization = false);

// Quantize only at the public output boundary, after nested gains and colour
// conversion. Internal tmap samples may exceed a relative encoding's unit range.
Result<std::shared_ptr<HeifPixelImage>> finish_tone_map_output(
    const std::shared_ptr<HeifPixelImage>& image,
    const heif_decoding_options& options,
    const heif_security_limits* limits, bool floating = false);

// Root output conversion, after all nested ISO gain-map operations complete.
Result<std::shared_ptr<HeifPixelImage>> convert_tone_map_colour(
    const std::shared_ptr<HeifPixelImage>& image,
    const heif_color_profile_nclx& requested,
    const heif_decoding_options& options,
    const heif_security_limits* limits,
    bool floating = false);

#endif
