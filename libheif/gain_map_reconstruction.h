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
#include "image/pixelimage.h"

Result<std::shared_ptr<HeifPixelImage>> reconstruct_tone_map(
    const std::shared_ptr<HeifPixelImage>& base,
    const std::shared_ptr<HeifPixelImage>& gain,
    const GainMapMetadata& metadata,
    const nclx_profile& alternate,
    const heif_decoding_options& options,
    const heif_security_limits* limits,
    std::optional<nclx_profile> baseline_colour_override = std::nullopt,
    std::optional<double> target_headroom = std::nullopt);

#endif
