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

#ifndef LIBHEIF_GAIN_MAP_COLOR_H
#define LIBHEIF_GAIN_MAP_COLOR_H

#include "error.h"
#include <array>
#include <cstdint>

using GainMapRGB = std::array<double, 3>;
using GainMapMatrix = std::array<GainMapRGB, 3>;

// A conservative NCLX subset. PQ uses BT.2408 HDR reference white (203 cd/m2).
// ICC and HLG require a separate rendering/viewing configuration.
bool gain_map_supports_transfer(uint16_t transfer);
Result<double> gain_map_decode_transfer(double value, uint16_t transfer);
Result<double> gain_map_encode_transfer(double value, uint16_t transfer);
Result<GainMapMatrix> gain_map_primaries_matrix(uint16_t source, uint16_t target);
GainMapRGB gain_map_transform(const GainMapMatrix& matrix, const GainMapRGB& value);

#endif
