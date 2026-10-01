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

#include "tmap.h"

#include "context.h"
#include "file.h"


ImageItem_tmap::ImageItem_tmap(HeifContext* ctx, heif_item_id id)
    : ImageItem(ctx, id)
{
}


Result<std::array<heif_item_id, 2>>
ImageItem_tmap::get_input_item_ids() const
{
  auto iref = get_file()->get_iref_box();
  if (!iref) {
    return Error{
        heif_error_Invalid_input,
        heif_suberror_No_iref_box,
        "Tone-map derived image requires an 'iref' box"
    };
  }

  std::vector<heif_item_id> references =
      iref->get_references(get_id(), fourcc("dimg"));
  if (references.size() != 2) {
    return Error{
        heif_error_Invalid_input,
        heif_suberror_Unspecified,
        "Tone-map derived image must have exactly two ordered 'dimg' references"
    };
  }

  if (references[0] == get_id() ||
      references[1] == get_id()) {
    return Error{
        heif_error_Invalid_input,
        heif_suberror_Unspecified,
        "Tone-map derived image must not reference itself"
    };
  }

  if (references[0] == references[1]) {
    return Error{
        heif_error_Invalid_input,
        heif_suberror_Unspecified,
        "Tone-map base and gain-map inputs must be distinct image items"
    };
  }

  auto base = get_context()->get_image(references[0], true);
  auto gain = get_context()->get_image(references[1], true);
  if (!base || !gain) {
    return Error{
        heif_error_Invalid_input,
        heif_suberror_Nonexisting_item_referenced,
        "Tone-map derived image references a non-existing or unsupported image item"
    };
  }

  return std::array<heif_item_id, 2>{
      references[0], references[1]
  };
}


ToneMapImageParseResult ImageItem_tmap::read_tone_map_image() const
{
  auto payload = get_file()->get_uncompressed_item_data(get_id());
  if (!payload) {
    ToneMapImageParseResult result;
    result.status = ToneMapImageParseStatus::malformed;
    result.error = payload.error();
    return result;
  }

  return parse_tone_map_image(*payload);
}


Error ImageItem_tmap::validate_tone_map_structure() const
{
  auto input_ids = get_input_item_ids();
  if (!input_ids) {
    return input_ids.error();
  }

  auto base = get_context()->get_image((*input_ids)[0], true);
  auto gain = get_context()->get_image((*input_ids)[1], true);

  if (Error error = base->get_item_error()) {
    return error;
  }
  if (Error error = gain->get_item_error()) {
    return error;
  }

  if (!base->has_nclx_color_profile() &&
      !base->has_icc_color_profile()) {
    return Error{
        heif_error_Invalid_input,
        heif_suberror_Unspecified,
        "Tone-map base image must have an associated 'colr' property"
    };
  }

  if (!gain->has_nclx_color_profile()) {
    return Error{
        heif_error_Invalid_input,
        heif_suberror_Unspecified,
        "Tone-map gain image must have an NCLX 'colr' property"
    };
  }

  const nclx_profile gain_nclx =
      gain->get_color_profile_nclx();
  if (gain_nclx.get_colour_primaries() !=
          heif_color_primaries_unspecified ||
      gain_nclx.get_transfer_characteristics() !=
          heif_transfer_characteristic_unspecified) {
    return Error{
        heif_error_Invalid_input,
        heif_suberror_Unspecified,
        "Tone-map gain image NCLX must use colour_primaries=2 and transfer_characteristics=2"
    };
  }

  if (!has_nclx_color_profile() &&
      !has_icc_color_profile()) {
    return Error{
        heif_error_Invalid_input,
        heif_suberror_Unspecified,
        "Tone-map derived image must have an associated 'colr' property"
    };
  }

  return Error::Ok;
}


Result<std::shared_ptr<HeifPixelImage>>
ImageItem_tmap::decode_compressed_image(
    const heif_decoding_options&,
    bool,
    uint32_t,
    uint32_t,
    DecodeTraversalState decode_state) const
{
  if (decode_state.processed_ids.contains(get_id())) {
    return Error{
        heif_error_Invalid_input,
        heif_suberror_Unspecified,
        "'iref' has cyclic references"
    };
  }
  decode_state.processed_ids.insert(get_id());

  ToneMapImageParseResult payload = read_tone_map_image();
  switch (payload.status) {
    case ToneMapImageParseStatus::unsupported_tone_map_version:
      return Error{
          heif_error_Unsupported_feature,
          heif_suberror_Unsupported_data_version,
          "Unsupported ToneMapImage version"
      };

    case ToneMapImageParseStatus::unsupported_minimum_version:
      return Error{
          heif_error_Unsupported_feature,
          heif_suberror_Unsupported_data_version,
          "Unsupported ISO 21496-1 gain map metadata minimum version"
      };

    case ToneMapImageParseStatus::malformed:
      return payload.error;

    case ToneMapImageParseStatus::parsed:
      break;
  }

  if (Error error = validate_tone_map_structure()) {
    return error;
  }

  return Error{
      heif_error_Unsupported_feature,
      heif_suberror_Unsupported_image_type,
      "ISO 21496-1 tone-map reconstruction is not implemented yet"
  };
}
