/*
 * HEIF codec.
 * Copyright (c) 2024 Dirk Farin <dirk.farin@gmail.com>
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

#include "heif_experimental.h"
#include "context.h"
#include "api_structs.h"
#include "image-items/unc_image.h"
#include "image-items/tiled.h"
#include "image-items/tmap.h"

#include <array>
#include <cstring>
#include <memory>
#include <vector>
#include <limits>
#include <utility>




namespace
{

std::shared_ptr<ImageItem_tmap> get_tmap_item(
    const heif_image_handle* handle)
{
  if (!handle || !handle->image) {
    return nullptr;
  }

  return std::dynamic_pointer_cast<ImageItem_tmap>(handle->image);
}


heif_error make_tmap_input_handle(
    const heif_image_handle* tmap_handle,
    size_t input_index,
    heif_image_handle** out_handle)
{
  if (!tmap_handle || !out_handle) {
    return heif_error_null_pointer_argument;
  }

  *out_handle = nullptr;
  auto tmap = get_tmap_item(tmap_handle);
  if (!tmap) {
    return {
        heif_error_Usage_error,
        heif_suberror_Invalid_parameter_value,
        "Image handle is not a tone-map derived image"
    };
  }

  auto input_ids = tmap->get_input_item_ids();
  if (!input_ids) {
    return input_ids.error_struct(tmap_handle->context.get());
  }

  auto image = tmap_handle->context->get_image(
      (*input_ids)[input_index], true);
  if (!image) {
    return {
        heif_error_Invalid_input,
        heif_suberror_Nonexisting_item_referenced,
        "Tone-map input image is unavailable"
    };
  }

  if (Error error = image->get_item_error()) {
    return error.error_struct(tmap_handle->context.get());
  }

  *out_handle = new heif_image_handle;
  (*out_handle)->image = std::move(image);
  (*out_handle)->context = tmap_handle->context;
  return heif_error_success;
}


void copy_signed_rational(
    const GainMapSignedRational32& input,
    heif_signed_rational32* output)
{
  output->numerator = input.numerator;
  output->denominator = input.denominator;
}


void copy_unsigned_rational(
    const GainMapUnsignedRational32& input,
    heif_unsigned_rational32* output)
{
  output->numerator = input.numerator;
  output->denominator = input.denominator;
}



Result<GainMapMetadata> gain_map_metadata_from_c(
    const heif_gain_map_metadata& input)
{
  if (input.struct_version != 1) {
    return Error{
        heif_error_Usage_error,
        heif_suberror_Invalid_parameter_value,
        "Unsupported heif_gain_map_metadata struct version"
    };
  }

  if (input.channel_count != 1 && input.channel_count != 3) {
    return Error{
        heif_error_Usage_error,
        heif_suberror_Invalid_parameter_value,
        "Gain-map metadata must have one or three channels"
    };
  }

  GainMapMetadata metadata;
  metadata.version.minimum_version = input.minimum_version;
  metadata.version.writer_version = input.writer_version;
  metadata.channel_count = input.channel_count;
  metadata.is_multichannel = input.channel_count == 3;
  metadata.use_base_colour_space =
      input.use_base_colour_space != 0;
  metadata.base_hdr_headroom = {
      input.base_hdr_headroom.numerator,
      input.base_hdr_headroom.denominator
  };
  metadata.alternate_hdr_headroom = {
      input.alternate_hdr_headroom.numerator,
      input.alternate_hdr_headroom.denominator
  };

  for (uint8_t i = 0; i < input.channel_count; ++i) {
    metadata.channels[i].gain_map_min = {
        input.channels[i].gain_map_min.numerator,
        input.channels[i].gain_map_min.denominator
    };
    metadata.channels[i].gain_map_max = {
        input.channels[i].gain_map_max.numerator,
        input.channels[i].gain_map_max.denominator
    };
    metadata.channels[i].gamma = {
        input.channels[i].gamma.numerator,
        input.channels[i].gamma.denominator
    };
    metadata.channels[i].base_offset = {
        input.channels[i].base_offset.numerator,
        input.channels[i].base_offset.denominator
    };
    metadata.channels[i].alternate_offset = {
        input.channels[i].alternate_offset.numerator,
        input.channels[i].alternate_offset.denominator
    };
  }

  return metadata;
}


heif_gain_map_metadata_status to_c_status(
    ToneMapImageParseStatus status)
{
  switch (status) {
    case ToneMapImageParseStatus::parsed:
      return heif_gain_map_metadata_status_parsed;
    case ToneMapImageParseStatus::unsupported_tone_map_version:
      return heif_gain_map_metadata_status_unsupported_tone_map_version;
    case ToneMapImageParseStatus::unsupported_minimum_version:
      return heif_gain_map_metadata_status_unsupported_minimum_version;
    case ToneMapImageParseStatus::malformed:
      return heif_gain_map_metadata_status_malformed;
  }

  return heif_gain_map_metadata_status_malformed;
}

}  // namespace


int heif_image_handle_is_tone_map_derived_image(
    const heif_image_handle* handle)
{
  return get_tmap_item(handle) ? 1 : 0;
}


heif_error heif_image_handle_get_tone_map_base_image_handle(
    const heif_image_handle* tmap,
    heif_image_handle** out_base)
{
  return exception_guard([&]() -> heif_error {
    return make_tmap_input_handle(tmap, 0, out_base);
  });
}


heif_error heif_image_handle_get_tone_map_gain_map_image_handle(
    const heif_image_handle* tmap,
    heif_image_handle** out_gain_map)
{
  return exception_guard([&]() -> heif_error {
    return make_tmap_input_handle(tmap, 1, out_gain_map);
  });
}


heif_gain_map_metadata_status
heif_image_handle_get_gain_map_metadata_status(
    const heif_image_handle* tmap)
{
  auto item = get_tmap_item(tmap);
  if (!item) {
    return heif_gain_map_metadata_status_not_a_tone_map;
  }

  return to_c_status(item->read_tone_map_image().status);
}


heif_error heif_image_handle_get_gain_map_metadata(
    const heif_image_handle* tmap,
    heif_gain_map_metadata* out_metadata)
{
  return exception_guard([&]() -> heif_error {
    if (!tmap || !out_metadata) {
      return heif_error_null_pointer_argument;
    }

    auto item = get_tmap_item(tmap);
    if (!item) {
      return {
          heif_error_Usage_error,
          heif_suberror_Invalid_parameter_value,
          "Image handle is not a tone-map derived image"
      };
    }

    ToneMapImageParseResult parsed =
        item->read_tone_map_image();
    if (parsed.status ==
        ToneMapImageParseStatus::unsupported_tone_map_version) {
      return {
          heif_error_Unsupported_feature,
          heif_suberror_Unsupported_data_version,
          "Unsupported ToneMapImage version"
      };
    }
    if (parsed.status ==
        ToneMapImageParseStatus::unsupported_minimum_version) {
      return {
          heif_error_Unsupported_feature,
          heif_suberror_Unsupported_data_version,
          "Unsupported ISO 21496-1 gain map metadata minimum version"
      };
    }
    if (parsed.status == ToneMapImageParseStatus::malformed) {
      return parsed.error.error_struct(tmap->context.get());
    }

    if (Error error = item->validate_tone_map_structure()) {
      return error.error_struct(tmap->context.get());
    }

    const GainMapMetadata& metadata =
        parsed.tone_map_image->gain_map_metadata;

    *out_metadata = {};
    out_metadata->struct_version = 1;
    out_metadata->minimum_version =
        metadata.version.minimum_version;
    out_metadata->writer_version =
        metadata.version.writer_version;
    out_metadata->channel_count = metadata.channel_count;
    out_metadata->use_base_colour_space =
        metadata.use_base_colour_space ? 1 : 0;

    copy_unsigned_rational(
        metadata.base_hdr_headroom,
        &out_metadata->base_hdr_headroom);
    copy_unsigned_rational(
        metadata.alternate_hdr_headroom,
        &out_metadata->alternate_hdr_headroom);

    for (uint8_t i = 0; i < metadata.channel_count; ++i) {
      copy_signed_rational(
          metadata.channels[i].gain_map_min,
          &out_metadata->channels[i].gain_map_min);
      copy_signed_rational(
          metadata.channels[i].gain_map_max,
          &out_metadata->channels[i].gain_map_max);
      copy_unsigned_rational(
          metadata.channels[i].gamma,
          &out_metadata->channels[i].gamma);
      copy_signed_rational(
          metadata.channels[i].base_offset,
          &out_metadata->channels[i].base_offset);
      copy_signed_rational(
          metadata.channels[i].alternate_offset,
          &out_metadata->channels[i].alternate_offset);
    }

    return heif_error_success;
  });
}



heif_error heif_context_add_tone_map_derived_image(
    heif_context* ctx,
    const heif_image_handle* base,
    const heif_image_handle* gain,
    const heif_gain_map_metadata* metadata,
    const heif_tone_map_options* options,
    heif_image_handle** out_tmap)
{
  return exception_guard([&]() -> heif_error {
    if (out_tmap) {
      *out_tmap = nullptr;
    }

    if (!ctx || !base || !gain || !metadata || !options) {
      return heif_error_null_pointer_argument;
    }

    if (!base->image || !gain->image ||
        base->context.get() != ctx->context.get() ||
        gain->context.get() != ctx->context.get()) {
      return {
          heif_error_Usage_error,
          heif_suberror_Invalid_parameter_value,
          "Tone-map input handles must belong to the target context"
      };
    }

    if (options->version != 1) {
      return {
          heif_error_Usage_error,
          heif_suberror_Invalid_parameter_value,
          "Unsupported heif_tone_map_options version"
      };
    }

    if (!options->alternate_nclx) {
      return {
          heif_error_Usage_error,
          heif_suberror_Null_pointer_argument,
          "Tone-map writer currently requires an alternate NCLX profile"
      };
    }

    if (options->pixi_num_channels > 4) {
      return {
          heif_error_Usage_error,
          heif_suberror_Invalid_parameter_value,
          "Tone-map PIXI hint may contain at most four channels"
      };
    }

    auto internal_metadata =
        gain_map_metadata_from_c(*metadata);
    if (!internal_metadata) {
      return internal_metadata.error_struct(ctx->context.get());
    }

    ToneMapImage tone_map_image;
    tone_map_image.version = 0;
    tone_map_image.gain_map_metadata =
        *internal_metadata;

    nclx_profile alternate_nclx;
    alternate_nclx.set_from_heif_color_profile_nclx(
        options->alternate_nclx);

    std::vector<uint8_t> pixi_bits;
    pixi_bits.reserve(options->pixi_num_channels);
    for (uint8_t i = 0;
         i < options->pixi_num_channels; ++i) {
      pixi_bits.push_back(
          options->pixi_bits_per_channel[i]);
    }

    const heif_content_light_level* clli =
        options->has_clli ? &options->clli : nullptr;

    auto result = ImageItem_tmap::add_new_tone_map_item(
        ctx->context.get(),
        base->image,
        gain->image,
        tone_map_image,
        alternate_nclx,
        clli,
        pixi_bits);
    if (!result) {
      return result.error_struct(ctx->context.get());
    }

    if (out_tmap) {
      *out_tmap = new heif_image_handle;
      (*out_tmap)->image = *result;
      (*out_tmap)->context = ctx->context;
    }

    return heif_error_success;
  });
}


struct heif_property_camera_intrinsic_matrix
{
  Box_cmin::RelativeIntrinsicMatrix matrix;
};

heif_error heif_item_get_property_camera_intrinsic_matrix(const heif_context* context,
                                                          heif_item_id itemId,
                                                          heif_property_id propertyId,
                                                          heif_property_camera_intrinsic_matrix** out_matrix)
{
  if (!out_matrix || !context) {
    return heif_error_null_pointer_argument;
  }

  auto cmin = context->context->find_property<Box_cmin>(itemId, propertyId);
  if (!cmin) {
    return cmin.error_struct(context->context.get());
  }

  *out_matrix = new heif_property_camera_intrinsic_matrix;
  (*out_matrix)->matrix = (*cmin)->get_intrinsic_matrix();

  return heif_error_success;
}


void heif_property_camera_intrinsic_matrix_release(heif_property_camera_intrinsic_matrix* matrix)
{
  delete matrix;
}

heif_error heif_property_camera_intrinsic_matrix_get_focal_length(const heif_property_camera_intrinsic_matrix* matrix,
                                                                  int image_width, int image_height,
                                                                  double* out_focal_length_x,
                                                                  double* out_focal_length_y)
{
  if (!matrix) {
    return heif_error_null_pointer_argument;
  }

  double fx, fy;
  matrix->matrix.compute_focal_length(image_width, image_height, fx, fy);

  if (out_focal_length_x) *out_focal_length_x = fx;
  if (out_focal_length_y) *out_focal_length_y = fy;

  return heif_error_success;
}


heif_error heif_property_camera_intrinsic_matrix_get_principal_point(const heif_property_camera_intrinsic_matrix* matrix,
                                                                     int image_width, int image_height,
                                                                     double* out_principal_point_x,
                                                                     double* out_principal_point_y)
{
  if (!matrix) {
    return heif_error_null_pointer_argument;
  }

  double px, py;
  matrix->matrix.compute_principal_point(image_width, image_height, px, py);

  if (out_principal_point_x) *out_principal_point_x = px;
  if (out_principal_point_y) *out_principal_point_y = py;

  return heif_error_success;
}


heif_error heif_property_camera_intrinsic_matrix_get_skew(const heif_property_camera_intrinsic_matrix* matrix,
                                                          double* out_skew)
{
  if (!matrix || !out_skew) {
    return heif_error_null_pointer_argument;
  }

  *out_skew = matrix->matrix.skew;

  return heif_error_success;
}


heif_property_camera_intrinsic_matrix* heif_property_camera_intrinsic_matrix_alloc()
{
  return new heif_property_camera_intrinsic_matrix;
}

void heif_property_camera_intrinsic_matrix_set_simple(heif_property_camera_intrinsic_matrix* matrix,
                                                      int image_width, int image_height,
                                                      double focal_length, double principal_point_x, double principal_point_y)
{
  if (!matrix) {
    return;
  }

  matrix->matrix.is_anisotropic = false;
  matrix->matrix.focal_length_x = focal_length / image_width;
  matrix->matrix.principal_point_x = principal_point_x / image_width;
  matrix->matrix.principal_point_y = principal_point_y / image_height;
}

void heif_property_camera_intrinsic_matrix_set_full(heif_property_camera_intrinsic_matrix* matrix,
                                                    int image_width, int image_height,
                                                    double focal_length_x,
                                                    double focal_length_y,
                                                    double principal_point_x, double principal_point_y,
                                                    double skew)
{
  if (!matrix) {
    return;
  }

  if (focal_length_x == focal_length_y && skew == 0) {
    heif_property_camera_intrinsic_matrix_set_simple(matrix, image_width, image_height, focal_length_x, principal_point_x, principal_point_y);
    return;
  }

  matrix->matrix.is_anisotropic = true;
  matrix->matrix.focal_length_x = focal_length_x / image_width;
  matrix->matrix.focal_length_y = focal_length_y / image_width;
  matrix->matrix.principal_point_x = principal_point_x / image_width;
  matrix->matrix.principal_point_y = principal_point_y / image_height;
  matrix->matrix.skew = skew;
}


heif_error heif_item_add_property_camera_intrinsic_matrix(const heif_context* context,
                                                          heif_item_id itemId,
                                                          const heif_property_camera_intrinsic_matrix* matrix,
                                                          heif_property_id* out_propertyId)
{
  if (!context || !matrix) {
    return heif_error_null_pointer_argument;
  }

  auto cmin = std::make_shared<Box_cmin>();
  cmin->set_intrinsic_matrix(matrix->matrix);

  auto id = context->context->add_property(itemId, cmin, false);
  if (!id) {
    return id.error_struct(context->context.get());
  }

  if (out_propertyId) {
    *out_propertyId = *id;
  }

  return heif_error_success;
}


struct heif_property_camera_extrinsic_matrix
{
  Box_cmex::ExtrinsicMatrix matrix;
};


heif_error heif_item_get_property_camera_extrinsic_matrix(const heif_context* context,
                                                          heif_item_id itemId,
                                                          heif_property_id propertyId,
                                                          heif_property_camera_extrinsic_matrix** out_matrix)
{
  if (!out_matrix || !context) {
    return heif_error_null_pointer_argument;
  }

  auto cmex = context->context->find_property<Box_cmex>(itemId, propertyId);
  if (!cmex) {
    return cmex.error_struct(context->context.get());
  }

  *out_matrix = new heif_property_camera_extrinsic_matrix;
  (*out_matrix)->matrix = (*cmex)->get_extrinsic_matrix();

  return heif_error_success;
}


void heif_property_camera_extrinsic_matrix_release(heif_property_camera_extrinsic_matrix* matrix)
{
  delete matrix;
}


heif_error heif_property_camera_extrinsic_matrix_get_rotation_matrix(const heif_property_camera_extrinsic_matrix* matrix,
                                                                     double* out_matrix)
{
  if (!matrix || !out_matrix) {
    return heif_error_null_pointer_argument;
  }

  auto rot_matrix = matrix->matrix.calculate_rotation_matrix();
  for (int i = 0; i < 9; i++) {
    out_matrix[i] = rot_matrix[i];
  }

  return heif_error_success;
}


heif_error heif_property_camera_extrinsic_matrix_get_position_vector(const heif_property_camera_extrinsic_matrix* matrix,
                                                                     int32_t* out_vector)
{
  if (!matrix || !out_vector) {
    return heif_error_null_pointer_argument;
  }

  out_vector[0] = matrix->matrix.pos_x;
  out_vector[1] = matrix->matrix.pos_y;
  out_vector[2] = matrix->matrix.pos_z;

  return heif_error_success;
}


heif_error heif_property_camera_extrinsic_matrix_get_world_coordinate_system_id(const heif_property_camera_extrinsic_matrix* matrix,
                                                                                uint32_t* out_wcs_id)
{
  if (!matrix || !out_wcs_id) {
    return heif_error_null_pointer_argument;
  }

  *out_wcs_id = matrix->matrix.world_coordinate_system_id;

  return heif_error_success;
}


#if HEIF_ENABLE_EXPERIMENTAL_FEATURES

heif_error heif_context_add_pyramid_entity_group(struct heif_context* ctx,
                                                 const heif_item_id* layer_item_ids,
                                                 size_t num_layers,
                                                 /*
                                                 uint16_t tile_width,
                                                 uint16_t tile_height,
                                                 uint32_t num_layers,
                                                 const heif_pyramid_layer_info* in_layers,
                                                  */
                                                 heif_item_id* out_group_id)
{
  if (!layer_item_ids) {
    return heif_error_null_pointer_argument;
  }

  if (num_layers == 0) {
    return {heif_error_Usage_error, heif_suberror_Invalid_parameter_value, "Number of layers cannot be 0."};
  }

  std::vector<heif_item_id> layers(num_layers);
  for (size_t i = 0; i < num_layers; i++) {
    layers[i] = layer_item_ids[i];
  }

  Result<heif_item_id> result = ctx->context->add_pyramid_group(layers);

  if (result) {
    if (out_group_id) {
      *out_group_id = *result;
    }
    return heif_error_success;
  }
  else {
    return result.error_struct(ctx->context.get());
  }
}


heif_pyramid_layer_info* heif_context_get_pyramid_entity_group_info(heif_context* ctx, heif_entity_group_id id, int* out_num_layers)
{
  if (!out_num_layers) {
    return nullptr;
  }

  std::shared_ptr<Box_EntityToGroup> groupBox = ctx->context->get_heif_file()->get_entity_group(id);
  if (!groupBox) {
    return nullptr;
  }

  const auto pymdBox = std::dynamic_pointer_cast<Box_pymd>(groupBox);
  if (!pymdBox) {
    return nullptr;
  }

  const std::vector<Box_pymd::LayerInfo> pymd_layers = pymdBox->get_layers();
  if (pymd_layers.empty()) {
    return nullptr;
  }

  auto items = pymdBox->get_item_ids();
  assert(items.size() == pymd_layers.size());

  auto* layerInfo = new heif_pyramid_layer_info[pymd_layers.size()];
  for (size_t i = 0; i < pymd_layers.size(); i++) {
    layerInfo[i].layer_image_id = items[i];
    layerInfo[i].layer_binning = pymd_layers[i].layer_binning;
    layerInfo[i].tile_rows_in_layer = pymd_layers[i].tiles_in_layer_row_minus1 + 1;
    layerInfo[i].tile_columns_in_layer = pymd_layers[i].tiles_in_layer_column_minus1 + 1;
  }

  *out_num_layers = static_cast<int>(pymd_layers.size());

  return layerInfo;
}


void heif_pyramid_layer_info_release(heif_pyramid_layer_info* infos)
{
  delete[] infos;
}


#endif


#if HEIF_ENABLE_EXPERIMENTAL_FEATURES
heif_error heif_context_add_tiled_image(heif_context* ctx,
                                        const heif_tiled_image_parameters* parameters,
                                        const heif_encoding_options* options,
                                        const heif_encoder* encoder,
                                        heif_image_handle** out_grid_image_handle)
{
  if (out_grid_image_handle) {
    *out_grid_image_handle = nullptr;
  }

  Result<std::shared_ptr<ImageItem_Tiled> > gridImageResult;
  gridImageResult = ImageItem_Tiled::add_new_tiled_item(ctx->context.get(), parameters, encoder, options);

  if (!gridImageResult) {
    return gridImageResult.error_struct(ctx->context.get());
  }

  if (out_grid_image_handle) {
    *out_grid_image_handle = new heif_image_handle;
    (*out_grid_image_handle)->image = *gridImageResult;
    (*out_grid_image_handle)->context = ctx->context;
  }

  return heif_error_success;
}
#endif
