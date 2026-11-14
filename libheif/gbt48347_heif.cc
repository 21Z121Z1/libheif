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

#include "gbt48347_heif.h"

#include "context.h"
#include "file.h"
#include "box.h"
#include "common_utils.h"
#include "image-items/tmap.h"

#include <memory>
#include <vector>


namespace
{

Error gbt_invalid(const char* message)
{
  return {heif_error_Invalid_input, heif_suberror_Unspecified, message};
}

// Effective irot angle (0 when no irot property is associated).
Result<int> item_rotation(HeifContext& ctx, heif_item_id id)
{
  auto item = ctx.get_image(id, true);
  if (!item) {
    return gbt_invalid("GB/T DRE-AIG references an unavailable image item");
  }
  auto irot = item->get_property<Box_irot>();
  return irot ? irot->get_rotation_ccw() : 0;
}

}  // namespace


namespace gbt48347
{

Result<DreAig> inspect_dre_aig(HeifContext& ctx)
{
  auto file = ctx.get_heif_file();
  if (!file) {
    return gbt_invalid("GB/T DRE-AIG inspection requires a loaded file");
  }

  heif_item_id tmap_id = 0;
  int tmap_count = 0;
  std::vector<heif_item_id> it35_ids;
  for (heif_item_id id : file->get_item_IDs()) {
    const uint32_t type = file->get_item_type_4cc(id);
    if (type == fourcc("tmap")) {
      tmap_id = id;
      ++tmap_count;
    }
    else if (type == fourcc("it35")) {
      it35_ids.push_back(id);
    }
  }

  if (tmap_count == 0) {
    return gbt_invalid("No 'tmap' derived image; not a GB/T 48347 dual-layer file");
  }
  if (tmap_count > 1) {
    return gbt_invalid("Multiple 'tmap' items; ambiguous GB/T DRE-AIG");
  }

  auto tmap = std::dynamic_pointer_cast<ImageItem_tmap>(ctx.get_image(tmap_id, true));
  if (!tmap) {
    return gbt_invalid("GB/T 'tmap' item is not a tone-map derived image");
  }

  // Reuse the ISO structure validation: one ordered 'dimg' entry with two
  // distinct inputs and the required 'colr' properties.
  if (Error error = tmap->validate_tone_map_structure()) {
    return error;
  }
  auto input_ids = tmap->get_input_item_ids();
  if (!input_ids) {
    return input_ids.error();
  }

  DreAig aig;
  aig.tmap_id = tmap_id;
  aig.base_id = (*input_ids)[0];
  aig.gain_id = (*input_ids)[1];

  // GB/T 9.3.2a: the base image is identified by 'pitm'.
  aig.base_is_primary = file->get_primary_image_ID() == aig.base_id;
  if (!aig.base_is_primary) {
    return gbt_invalid("GB/T base image must be the primary item ('pitm')");
  }

  // GB/T 9.3.3a: the enhancement image item shall be hidden.
  auto gain_infe = file->get_infe_box(aig.gain_id);
  aig.gain_hidden = gain_infe && gain_infe->is_hidden_item();
  if (!aig.gain_hidden) {
    return gbt_invalid("GB/T enhancement image must be a hidden item");
  }

  // GB/T 9.3.3c / 9.3.4c: irot must be identical across base, gain and tmap.
  auto base_rot = item_rotation(ctx, aig.base_id);
  auto gain_rot = item_rotation(ctx, aig.gain_id);
  auto tmap_rot = item_rotation(ctx, aig.tmap_id);
  if (!base_rot) {
    return base_rot.error();
  }
  if (!gain_rot) {
    return gain_rot.error();
  }
  if (!tmap_rot) {
    return tmap_rot.error();
  }
  if (*base_rot != *gain_rot || *base_rot != *tmap_rot) {
    return gbt_invalid("GB/T irot must match across base, enhancement and tmap");
  }

  // GB/T 9.3.6 / 10.2.2: the 'it35' item is optional; if present it must carry a
  // conformant UWAInfo.
  if (!it35_ids.empty()) {
    if (it35_ids.size() > 1) {
      return gbt_invalid("Multiple 'it35' items in a GB/T DRE-AIG");
    }
    aig.has_it35 = true;
    aig.it35_id = it35_ids[0];
    const auto* limits = ctx.get_security_limits();
    auto data = file->get_uncompressed_item_data(aig.it35_id, limits->max_memory_block_size);
    if (!data) {
      return data.error();
    }
    auto parsed = parse_uwa_info(*data);
    if (parsed.status != ParseStatus::parsed) {
      return parsed.error;
    }
    aig.uwa = *parsed.info;
  }

  return aig;
}


Error add_it35_uwa_info(HeifContext& ctx, const UWAInfo& uwa, heif_item_id* out_it35_id)
{
  auto file = ctx.get_heif_file();
  if (!file) {
    return gbt_invalid("Adding GB/T 'it35' requires a loaded context");
  }

  // The it35 metadata describes a tone-map DRE-AIG; require a tmap to exist.
  bool has_tmap = false;
  for (heif_item_id id : file->get_item_IDs()) {
    if (file->get_item_type_4cc(id) == fourcc("tmap")) {
      has_tmap = true;
      break;
    }
  }
  if (!has_tmap) {
    return gbt_invalid("GB/T 'it35' requires an existing 'tmap' derived image");
  }

  auto payload = serialize_uwa_info(uwa);
  if (!payload) {
    return payload.error();
  }

  auto infe = file->add_new_meta_infe_box(fourcc("it35"));
  if (!infe) {
    return infe.error();
  }
  (*infe)->set_hidden_item(true);
  const heif_item_id it35_id = (*infe)->get_item_ID();

  if (Error error = file->set_item_data(*infe, payload->data(), payload->size(),
                                        heif_metadata_compression_off)) {
    return error;
  }

  if (out_it35_id) {
    *out_it35_id = it35_id;
  }
  return Error::Ok;
}

}  // namespace gbt48347