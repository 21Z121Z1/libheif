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

#ifndef LIBHEIF_HEIF_EXPERIMENTAL_H
#define LIBHEIF_HEIF_EXPERIMENTAL_H

#include "libheif/heif.h"

#ifdef __cplusplus
extern "C" {
#endif

#if HEIF_ENABLE_EXPERIMENTAL_FEATURES

/* ===================================================================================
 *   This file contains candidate APIs that did not make it into the public API yet.
 * ===================================================================================
 */



// --- ISO 21496-1 / HEIF 'tmap' inspection

typedef struct heif_signed_rational32
{
  int32_t numerator;
  uint32_t denominator;
} heif_signed_rational32;

typedef struct heif_unsigned_rational32
{
  uint32_t numerator;
  uint32_t denominator;
} heif_unsigned_rational32;

typedef struct heif_gain_map_channel
{
  heif_signed_rational32 gain_map_min;
  heif_signed_rational32 gain_map_max;
  heif_unsigned_rational32 gamma;
  heif_signed_rational32 base_offset;
  heif_signed_rational32 alternate_offset;
} heif_gain_map_channel;

typedef struct heif_gain_map_metadata
{
  uint32_t struct_version;

  uint16_t minimum_version;
  uint16_t writer_version;

  uint8_t channel_count;
  uint8_t use_base_colour_space;

  heif_unsigned_rational32 base_hdr_headroom;
  heif_unsigned_rational32 alternate_hdr_headroom;
  heif_gain_map_channel channels[3];
} heif_gain_map_metadata;

typedef enum heif_gain_map_metadata_status
{
  heif_gain_map_metadata_status_not_a_tone_map = 0,
  heif_gain_map_metadata_status_parsed = 1,
  heif_gain_map_metadata_status_unsupported_tone_map_version = 2,
  heif_gain_map_metadata_status_unsupported_minimum_version = 3,
  heif_gain_map_metadata_status_malformed = 4
} heif_gain_map_metadata_status;

LIBHEIF_API
int heif_image_handle_is_tone_map_derived_image(
    const heif_image_handle* handle);

LIBHEIF_API
heif_error heif_image_handle_get_tone_map_base_image_handle(
    const heif_image_handle* tmap,
    heif_image_handle** out_base);

LIBHEIF_API
heif_error heif_image_handle_get_tone_map_gain_map_image_handle(
    const heif_image_handle* tmap,
    heif_image_handle** out_gain_map);

LIBHEIF_API
heif_error heif_image_handle_get_gain_map_metadata(
    const heif_image_handle* tmap,
    heif_gain_map_metadata* out_metadata);

LIBHEIF_API
heif_gain_map_metadata_status
heif_image_handle_get_gain_map_metadata_status(
    const heif_image_handle* tmap);


typedef struct heif_tone_map_options
{
  uint32_t version;

  // Required in version 1. In version 2, choose NCLX or ICC, but not both.
  // Describes the fully-applied alternate image.
  const heif_color_profile_nclx* alternate_nclx;

  // Optional version-1 properties.
  int has_clli;
  heif_content_light_level clli;

  // Optional reconstructed-colour-resolution hint. Zero channels omits PIXI.
  uint8_t pixi_num_channels;
  uint8_t pixi_bits_per_channel[4];

  // Version 2. ICC bytes are copied; the caller retains ownership.
  heif_color_profile_type alternate_icc_type; // prof or rICC
  const void* alternate_icc;
  size_t alternate_icc_size;

  // Compatibility policy, independent of normative tmap reconstruction.
  // The allocator defaults to hiding the gain map and creating {tmap,base}.
  // Primary selection is always preserved. Disable group creation when
  // assembling several alternatives, then use the generic altr writer.
  int hide_gain_map;
  int create_altr_group;
} heif_tone_map_options;

LIBHEIF_API
heif_tone_map_options* heif_tone_map_options_alloc(void);

LIBHEIF_API
void heif_tone_map_options_free(heif_tone_map_options* options);

typedef struct heif_gain_map_image_options
{
  uint32_t version;
  // Describes the sample-domain matrix/range, with CP=2 and TC=2.
  // NULL defaults to full-range identity for RGB and unspecified for mono.
  // YCbCr input requires explicit sample-domain signalling.
  const heif_color_profile_nclx* nclx;
  int hidden;
} heif_gain_map_image_options;

LIBHEIF_API
heif_gain_map_image_options* heif_gain_map_image_options_alloc(void);

LIBHEIF_API
void heif_gain_map_image_options_free(heif_gain_map_image_options* options);

// Input samples are normalized gain data, not an SDR/HDR colour image.
// Does not modify the input image or select a primary image. NULL gain_options
// uses defaults (including hidden=true). The encoded gain may be shared.
LIBHEIF_API
heif_error heif_context_encode_gain_map_image(
    heif_context* ctx,
    const heif_image* gain_pixels,
    heif_encoder* encoder,
    const heif_encoding_options* encoding_options,
    const heif_gain_map_image_options* gain_options,
    heif_image_handle** out_gain);

LIBHEIF_API
heif_error heif_context_add_tone_map_derived_image(
    heif_context* ctx,
    const heif_image_handle* base,
    const heif_image_handle* gain,
    const heif_gain_map_metadata* metadata,
    const heif_tone_map_options* options,
    heif_image_handle** out_tmap);

/*
heif_item_property_type_camera_intrinsic_matrix = heif_fourcc('c', 'm', 'i', 'n'),
heif_item_property_type_camera_extrinsic_matrix = heif_fourcc('c', 'm', 'e', 'x')
*/

typedef struct heif_property_camera_intrinsic_matrix heif_property_camera_intrinsic_matrix;
typedef struct heif_property_camera_extrinsic_matrix heif_property_camera_extrinsic_matrix;

//LIBHEIF_API
heif_error heif_item_get_property_camera_intrinsic_matrix(const heif_context* context,
                                                          heif_item_id itemId,
                                                          heif_property_id propertyId,
                                                          heif_property_camera_intrinsic_matrix** out_matrix);

//LIBHEIF_API
void heif_property_camera_intrinsic_matrix_release(heif_property_camera_intrinsic_matrix* matrix);

//LIBHEIF_API
heif_error heif_property_camera_intrinsic_matrix_get_focal_length(const heif_property_camera_intrinsic_matrix* matrix,
                                                                  int image_width, int image_height,
                                                                  double* out_focal_length_x,
                                                                  double* out_focal_length_y);

//LIBHEIF_API
heif_error heif_property_camera_intrinsic_matrix_get_principal_point(const heif_property_camera_intrinsic_matrix* matrix,
                                                                     int image_width, int image_height,
                                                                     double* out_principal_point_x,
                                                                     double* out_principal_point_y);

//LIBHEIF_API
heif_error heif_property_camera_intrinsic_matrix_get_skew(const heif_property_camera_intrinsic_matrix* matrix,
                                                          double* out_skew);

//LIBHEIF_API
heif_property_camera_intrinsic_matrix* heif_property_camera_intrinsic_matrix_alloc(void);

//LIBHEIF_API
void heif_property_camera_intrinsic_matrix_set_simple(heif_property_camera_intrinsic_matrix* matrix,
                                                      int image_width, int image_height,
                                                      double focal_length, double principal_point_x, double principal_point_y);

//LIBHEIF_API
void heif_property_camera_intrinsic_matrix_set_full(heif_property_camera_intrinsic_matrix* matrix,
                                                    int image_width, int image_height,
                                                    double focal_length_x,
                                                    double focal_length_y,
                                                    double principal_point_x, double principal_point_y,
                                                    double skew);

//LIBHEIF_API
heif_error heif_item_add_property_camera_intrinsic_matrix(const heif_context* context,
                                                          heif_item_id itemId,
                                                          const heif_property_camera_intrinsic_matrix* matrix,
                                                          heif_property_id* out_propertyId);


//LIBHEIF_API
heif_error heif_item_get_property_camera_extrinsic_matrix(const heif_context* context,
                                                          heif_item_id itemId,
                                                          heif_property_id propertyId,
                                                          heif_property_camera_extrinsic_matrix** out_matrix);

//LIBHEIF_API
void heif_property_camera_extrinsic_matrix_release(heif_property_camera_extrinsic_matrix* matrix);

// `out_matrix` must point to a 9-element matrix, which will be filled in row-major order.
//LIBHEIF_API
heif_error heif_property_camera_extrinsic_matrix_get_rotation_matrix(const heif_property_camera_extrinsic_matrix* matrix,
                                                                     double* out_matrix);

// `out_vector` must point to a 3-element vector, which will be filled with the (X,Y,Z) coordinates (in micrometers).
//LIBHEIF_API
heif_error heif_property_camera_extrinsic_matrix_get_position_vector(const heif_property_camera_extrinsic_matrix* matrix,
                                                                     int32_t* out_vector);

//LIBHEIF_API
heif_error heif_property_camera_extrinsic_matrix_get_world_coordinate_system_id(const heif_property_camera_extrinsic_matrix* matrix,
                                                                                uint32_t* out_wcs_id);
#endif

// --- Tiled images

typedef struct heif_tiled_image_parameters
{
  int version;

  // --- version 1

  uint32_t image_width;
  uint32_t image_height;

  uint32_t tile_width;
  uint32_t tile_height;

  uint32_t compression_format_fourcc;  // will be set automatically when calling heif_context_add_tiled_image()

  uint8_t offset_field_length;   // one of: 32, 40, 48, 64
  uint8_t size_field_length;     // one of:  0, 24, 32, 64

  uint8_t number_of_extra_dimensions;  // 0 for normal images, 1 for volumetric (3D), ...
  uint32_t extra_dimensions[8];        // size of extra dimensions (first 8 dimensions)

  // boolean flags
  uint8_t tiles_are_sequential;  // TODO: can we derive this automatically
} heif_tiled_image_parameters;

#if HEIF_ENABLE_EXPERIMENTAL_FEATURES
LIBHEIF_API
heif_error heif_context_add_tiled_image(heif_context* ctx,
                                        const heif_tiled_image_parameters* parameters,
                                        const heif_encoding_options* options, // TODO: do we need this?
                                        const heif_encoder* encoder,
                                        heif_image_handle** out_tiled_image_handle);
#endif

// --- 'pymd' entity group (pyramid layers)

typedef struct heif_pyramid_layer_info
{
  heif_item_id layer_image_id;
  uint16_t layer_binning;
  uint32_t tile_rows_in_layer;
  uint32_t tile_columns_in_layer;
} heif_pyramid_layer_info;

#if HEIF_ENABLE_EXPERIMENTAL_FEATURES
// The input images are automatically sorted according to resolution. You can provide them in any order.
LIBHEIF_API
heif_error heif_context_add_pyramid_entity_group(heif_context* ctx,
                                                 const heif_item_id* layer_item_ids,
                                                 size_t num_layers,
                                                 heif_item_id* out_group_id);

LIBHEIF_API
heif_pyramid_layer_info* heif_context_get_pyramid_entity_group_info(heif_context*,
                                                                    heif_entity_group_id id,
                                                                    int* out_num_layers);

LIBHEIF_API
void heif_pyramid_layer_info_release(heif_pyramid_layer_info*);
#endif


#ifdef __cplusplus
}
#endif

#endif
