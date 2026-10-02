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
#include "nclx.h"

#include <array>
#include <cstdint>
#include <memory>

using GainMapRGB = std::array<double, 3>;
using GainMapMatrix = std::array<GainMapRGB, 3>;

// A conservative colour subset. PQ uses BT.2408 HDR reference white
// (203 cd/m2). ICC profiles are accepted only when they can be mapped
// losslessly enough to this CICP subset: first through ICC.1:2022 'cicp',
// then through a recognized RGB matrix/TRC profile.
bool gain_map_supports_transfer(uint16_t transfer);
Result<double> gain_map_decode_transfer(double value, uint16_t transfer);
Result<double> gain_map_encode_transfer(double value, uint16_t transfer);
// HLG is a luminance-dependent RGB transform. Use the BT.2100 reference
// display (1000 cd/m2, gamma 1.2, zero black) and HDR reference white 203.
// Scalar transfer helpers intentionally cannot interpret HLG.
Result<GainMapRGB> gain_map_decode_rgb(const GainMapRGB& value, const nclx_profile& profile);
Result<GainMapRGB> gain_map_encode_rgb(const GainMapRGB& value, const nclx_profile& profile);
Result<GainMapMatrix> gain_map_primaries_matrix(uint16_t source, uint16_t target);
GainMapRGB gain_map_transform(const GainMapMatrix& matrix, const GainMapRGB& value);

Result<nclx_profile> gain_map_nclx_from_icc(const color_profile_raw& profile);

struct GainMapIccColour;

// A linear RGB application space, independent of codec matrix/range storage.
// ICC matrix/TRC profiles retain their actual colourants and per-channel curves.
class GainMapColour
{
public:
  GainMapColour(const nclx_profile& profile) : m_nclx(profile) {}
  static Result<GainMapColour> from_icc(const std::shared_ptr<const color_profile_raw>& profile);
  Result<GainMapRGB> decode(const GainMapRGB& signal) const;
  Result<GainMapRGB> encode(const GainMapRGB& linear) const;
  Result<GainMapMatrix> matrix_to(const GainMapColour& target) const;
  const nclx_profile& raster_profile() const { return m_nclx; }
  const std::shared_ptr<const color_profile_raw>& icc_profile() const { return m_profile; }

private:
  nclx_profile m_nclx;
  std::shared_ptr<const color_profile_raw> m_profile;
  std::shared_ptr<const GainMapIccColour> m_matrix_trc;
};

#endif
