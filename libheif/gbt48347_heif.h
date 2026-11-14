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
 * along with libheif. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef LIBHEIF_GBT48347_HEIF_H
#define LIBHEIF_GBT48347_HEIF_H

// GB/T 48347-2026 clause 9.3 HEIF dual-layer (DRE-AIG) integration.
//
// This layer reuses the existing ISO 21496-1 / HEIF Amd.1 'tmap' machinery for
// the gain-map graph and reconstruction; it adds the GB/T-specific structure:
// the optional 'it35' item carrying UWAInfo, the hidden enhancement item, the
// base-as-primary rule, and irot consistency. It does not re-implement the ISO
// gain map and does not relax any generic ISO 'tmap' validation.

#include "gbt48347.h"
#include "error.h"

#include "libheif/heif.h"


class HeifContext;


namespace gbt48347
{

// A discovered GB/T 48347 DRE-AIG (dynamic range extending associated image group).
struct DreAig
{
  heif_item_id tmap_id = 0;
  heif_item_id base_id = 0;
  heif_item_id gain_id = 0;
  bool base_is_primary = false;
  bool gain_hidden = false;
  bool has_it35 = false;
  heif_item_id it35_id = 0;
  UWAInfo uwa;  // populated only when has_it35 is true
};


// Discover and validate the GB/T dual-layer graph in a context.
//
// Requires exactly one 'tmap' derived image with a valid ISO structure (two
// ordered 'dimg' inputs and the required 'colr' properties), then applies the
// GB/T-specific checks: the enhancement item is hidden (9.3.3a), the base is the
// primary item (9.3.2a), irot is consistent across base/gain/tmap (9.3.3c/9.3.4c),
// and any 'it35' item carries a conformant UWAInfo (9.3.6). The 'it35' item is
// optional (10.2.2).
Result<DreAig> inspect_dre_aig(HeifContext& ctx);


// Attach a GB/T 'it35' item carrying UWAInfo to a context (write path). The
// context must already contain a 'tmap' derived image. The item is added hidden,
// consistent with GB/T metadata items. Returns the new item id.
Error add_it35_uwa_info(HeifContext& ctx, const UWAInfo& uwa, heif_item_id* out_it35_id);

}  // namespace gbt48347

#endif  // LIBHEIF_GBT48347_HEIF_H