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

#ifndef LIBHEIF_GAIN_MAP_MATH_H
#define LIBHEIF_GAIN_MAP_MATH_H

#include "gain_map_metadata.h"

// ISO 21496-1:2025 equations (1)-(3). Headroom values are log2 ratios.
// Callers validate the metadata once before invoking the pixel operations.
double gain_map_full_weight(const GainMapMetadata& metadata);
Result<double> gain_map_target_weight(const GainMapMetadata& metadata, double target_headroom);
Result<double> gain_map_unnormalize(double normalized, const GainMapChannel& channel);
Result<double> gain_map_apply(double baseline, double log2_gain,
                              const GainMapChannel& channel, double weight);

#endif
