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
#include "gain_map_color.h"
#include "gain_map_reconstruction.h"

#include <memory>
#include <utility>
#include <vector>


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

  std::vector<heif_item_id> references;
  size_t dimg_entry_count = 0;
  for (const auto& reference : iref->get_references_from(get_id())) {
    if (reference.header.get_short_type() != fourcc("dimg")) {
      continue;
    }

    ++dimg_entry_count;
    references = reference.to_item_ID;
  }

  if (dimg_entry_count != 1) {
    return Error{
        heif_error_Invalid_input,
        heif_suberror_Unspecified,
        "Tone-map derived image must have exactly one 'dimg' reference entry"
    };
  }

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


namespace
{

Error require_colour_property(const ImageItem& item, bool nclx_only, const char* message)
{
  auto properties = item.get_properties();
  if (!properties) { return properties.error(); }
  for (const auto& property : *properties) {
    auto colr = std::dynamic_pointer_cast<Box_colr>(property);
    if (colr && (!nclx_only || colr->get_color_profile_type() == fourcc("nclx"))) {
      return Error::Ok;
    }
  }
  return {heif_error_Invalid_input, heif_suberror_Unspecified, message};
}

Error validate_tone_map_inputs(
    const ImageItem& base,
    const ImageItem& gain)
{
  if (base.get_id() == gain.get_id()) {
    return Error{
        heif_error_Usage_error,
        heif_suberror_Invalid_parameter_value,
        "Tone-map base and gain-map inputs must be distinct image items"
    };
  }

  if (Error error = base.get_item_error()) {
    return error;
  }
  if (Error error = gain.get_item_error()) {
    return error;
  }

  if (Error error = require_colour_property(base, false,
          "Tone-map base image must have an associated 'colr' property")) {
    return error;
  }

  // An explicitly associated NCLX (2,2,2,full) is valid for a mono
  // gain map even though ImageDescription treats that tuple as undefined.
  // Presence must be checked on the associated property, not the values.
  if (Error error = require_colour_property(gain, true,
          "Tone-map gain image must have an NCLX 'colr' property")) {
    return error;
  }

  const nclx_profile gain_nclx =
      gain.get_color_profile_nclx();
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

  return Error::Ok;
}

Result<GainMapColour> resolve_tone_map_colour(const ImageItem& item)
{
  const auto nclx = item.get_color_profile_nclx();
  const auto ndwt = item.get_property<Box_ndwt>();
  const uint32_t diffuse_white = ndwt ? ndwt->get_diffuse_white_luminance() : 0;
  // HEIF 6.5.5 allows ICC plus NCLX: the latter then describes storage,
  // with CP=2/TC=2, while the ICC supplies RGB colourimetry.
  if (item.has_nclx_color_profile() &&
      nclx.m_colour_primaries != 2 && nclx.m_transfer_characteristics != 2) {
    return GainMapColour(item.get_color_profile_nclx(), diffuse_white);
  }

  const auto& icc = item.get_color_profile_icc();
  if (icc) {
    return GainMapColour::from_icc(icc, diffuse_white);
  }
  if (item.has_nclx_color_profile()) { return GainMapColour(nclx, diffuse_white); }

  return Error{
      heif_error_Unsupported_feature,
      heif_suberror_Unsupported_color_conversion,
      "Tone-map item has no supported colour description"
  };
}

}  // namespace


Error ImageItem_tmap::validate_tone_map_structure() const
{
  auto input_ids = get_input_item_ids();
  if (!input_ids) {
    return input_ids.error();
  }

  auto base = get_context()->get_image((*input_ids)[0], true);
  auto gain = get_context()->get_image((*input_ids)[1], true);

  if (!base || !gain) {
    return Error{
        heif_error_Input_does_not_exist,
        heif_suberror_Nonexisting_item_referenced,
        "Tone-map input image is unavailable"
    };
  }

  if (Error error = validate_tone_map_inputs(*base, *gain)) {
    return error;
  }

  return require_colour_property(*this, false,
      "Tone-map derived image must have an associated 'colr' property");
}


Result<std::shared_ptr<HeifPixelImage>>
ImageItem_tmap::decode_compressed_image(
    const heif_decoding_options& options,
    bool decode_tile_only,
    uint32_t tile_x0,
    uint32_t tile_y0,
    DecodeTraversalState decode_state) const
{
  if (decode_tile_only && (tile_x0 != 0 || tile_y0 != 0)) {
    return Error{heif_error_Usage_error, heif_suberror_Invalid_parameter_value,
                 "Tone-map derived images expose a single whole-image tile"};
  }

  if (decode_state.processed_ids.contains(get_id())) {
    return Error{
        heif_error_Invalid_input,
        heif_suberror_Unspecified,
        "'iref' has cyclic references"
    };
  }
  decode_state.processed_ids.insert(get_id());

  const auto target_headroom = decode_state.root_tmap_target_headroom;
  decode_state.root_tmap_target_headroom.reset();

  ToneMapImageParseResult payload = read_tone_map_image();
  switch (payload.status) {
    case ToneMapImageParseStatus::unsupported_tone_map_version:
      return Error{
          heif_error_Unsupported_feature,
          heif_suberror_Unsupported_data_version,
          "Unsupported ToneMapImage version"
      };

    case ToneMapImageParseStatus::unsupported_minimum_version: {
      auto ids = get_input_item_ids();
      if (!ids) {
        return ids.error();
      }
      // Annex C requires the baseline when the minimum version is unknown.
      // Do not decode or interpret the gain input in this case.
      auto base = get_context()->get_image((*ids)[0], true);
      auto base_options = options;
      base_options.ignore_transformations = false;
      base_options.autocorrect_broken_input = false;
      // The tmap exposes one logical tile for the whole reconstructed image.
      // Its children's tiling need not match, including on baseline fallback.
      return base->decode_image(base_options, false, 0, 0, decode_state);
    }

    case ToneMapImageParseStatus::malformed:
      return payload.error;

    case ToneMapImageParseStatus::parsed:
      break;
  }

  if (Error error = validate_tone_map_structure()) {
    return error;
  }

  auto ids = get_input_item_ids();
  if (!ids) {
    return ids.error();
  }
  auto base = get_context()->get_image((*ids)[0], true);
  auto gain = get_context()->get_image((*ids)[1], true);

  auto baseline_colour = resolve_tone_map_colour(*base);
  if (!baseline_colour) {
    return baseline_colour.error();
  }
  auto alternate_colour = resolve_tone_map_colour(*this);
  if (!alternate_colour) {
    return alternate_colour.error();
  }

  // Child transformations establish their display-space geometry regardless
  // of whether the caller suppresses the ROOT tmap's transformations.
  auto child_options = options;
  child_options.ignore_transformations = false;
  child_options.autocorrect_broken_input = false;
  auto base_pixels = base->decode_image(child_options, false, 0, 0, decode_state);
  if (!base_pixels) {
    return base_pixels.error();
  }
  auto gain_pixels = gain->decode_image(child_options, false, 0, 0, decode_state);
  if (!gain_pixels) {
    return gain_pixels.error();
  }
  return reconstruct_tone_map(*base_pixels, *gain_pixels,
                              payload.tone_map_image->gain_map_metadata,
                              *alternate_colour, options,
                              get_context()->get_security_limits(),
                              *baseline_colour, target_headroom, true,
                              decode_state.centered_gain_map_samples);
}


bool ImageItem_tmap::use_item_color_profile_for_decoding() const
{
  return read_tone_map_image().status != ToneMapImageParseStatus::unsupported_minimum_version;
}

Error ImageItem_tmap::get_coded_image_colorspace(heif_colorspace* colorspace, heif_chroma* chroma) const
{
  if (colorspace) { *colorspace = heif_colorspace_RGB; }
  if (chroma) { *chroma = heif_chroma_444; }
  return Error::Ok;
}
