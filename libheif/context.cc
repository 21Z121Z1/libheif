Warning: truncated output (original token count: 18889)
Total output lines: 2368

/*
 * HEIF codec.
 * Copyright (c) 2017 Dirk Farin <dirk.farin@gmail.com>
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

#include "box.h"
#include "error.h"
#include "libheif/heif.h"
#include "region.h"
#include "brands.h"
#include <cstdint>
#include <cassert>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <limits>
#include <cmath>
#include <deque>
#include <set>
#include "image-items/image_item.h"
#include <codecs/hevc_boxes.h>
#include "sequences/track.h"
#include "sequences/track_visual.h"
#include "sequences/track_metadata.h"
#include "libheif/heif_sequences.h"

#if ENABLE_PARALLEL_TILE_DECODING
#include <future>
#endif

#include "context.h"
#include "file.h"
#include "image/pixelimage.h"
#include "api_structs.h"
#include "security_limits.h"
#include "compression.h"
#include "color-conversion/colorconversion.h"
#include "plugin_registry.h"
#include "image-items/hevc.h"
#include "image-items/vvc.h"
#include "image-items/avif.h"
#include "image-items/jpeg.h"
#include "image-items/mask_image.h"
#include "image-items/jpeg2000.h"
#include "image-items/grid.h"
#include "image-items/overlay.h"
#include "image-items/tiled.h"

#if WITH_UNCOMPRESSED_CODEC
#include "image-items/unc_image.h"
#endif
#include "text.h"


heif_encoder::heif_encoder(const heif_encoder_plugin* _plugin)
    : plugin(_plugin)
{

}

heif_encoder::~heif_encoder()
{
  release();
}

void heif_encoder::release()
{
  if (encoder) {
    plugin->free_encoder(encoder);
    encoder = nullptr;
  }
}


heif_error heif_encoder::alloc()
{
  if (encoder == nullptr) {
    heif_error error = plugin->new_encoder(&encoder);
    // TODO: error handling
    return error;
  }

  return {heif_error_Ok, heif_suberror_Unspecified, Error::kSuccess};
}


void heif_encoder::copy_parameters_from(const heif_encoder& src)
{
  // Copy dedicated quality/lossless/logging parameters
  int ival;
  plugin->get_parameter_quality(src.encoder, &ival);
  plugin->set_parameter_quality(encoder, ival);

  plugin->get_parameter_lossless(src.encoder, &ival);
  plugin->set_parameter_lossless(encoder, ival);

  if (plugin->get_parameter_logging_level && plugin->set_parameter_logging_level) {
    plugin->get_parameter_logging_level(src.encoder, &ival);
    plugin->set_parameter_logging_level(encoder, ival);
  }

  // Copy all enumerable plugin parameters
  const heif_encoder_parameter* const* params = plugin->list_parameters(src.encoder);
  if (!params) return;

  for (; *params; params++) {
    const char* name = (*params)->name;
    switch ((*params)->type) {
      case heif_encoder_parameter_type_integer: {
        int v;
        if (plugin->get_parameter_integer(src.encoder, name, &v).code == heif_error_Ok)
          plugin->set_parameter_integer(encoder, name, v);
        break;
      }
      case heif_encoder_parameter_type_boolean: {
        int v;
        if (plugin->get_parameter_boolean(src.encoder, name, &v).code == heif_error_Ok)
          plugin->set_parameter_boolean(encoder, name, v);
        break;
      }
      case heif_encoder_parameter_type_string: {
        char v[256];
        if (plugin->get_parameter_string(src.encoder, name, v, sizeof(v)).code == heif_error_Ok)
          plugin->set_parameter_string(encoder, name, v);
        break;
      }
    }
  }
}


// Selects the initial security limits for a new context. The environment variable
// LIBHEIF_SECURITY_LIMITS=off disables all limits (for trusted input only).
static const heif_security_limits& initial_security_limits()
{
  const char* security_limits_variable = getenv("LIBHEIF_SECURITY_LIMITS");

  if (security_limits_variable && (strcmp(security_limits_variable, "off") == 0 ||
                                   strcmp(security_limits_variable, "OFF") == 0)) {
    return disabled_security_limits;
  }
  else {
    return global_security_limits;
  }
}


HeifContext::HeifContext()
    : m_limits(initial_security_limits()),
      m_memory_tracker(&m_limits)
{
  // m_limits must be fully initialized above before its address is passed to the memory tracker.
  // (m_limits is declared before m_memory_tracker in context.h, so it is constructed first.)

  reset_to_empty_heif();
}


HeifContext::~HeifContext()
{
  // Break circular references between Images (when a faulty input image has circular image references)
  for (auto& it : m_all_images) {
    std::shared_ptr<ImageItem> image = it.second;
    image->clear();
  }
}


static void copy_security_limits(heif_security_limits* dst, const heif_security_limits* src)
{
  dst->max_image_size_pixels = src->max_image_size_pixels;
  dst->max_number_of_tiles = src->max_number_of_tiles;
  dst->max_bayer_pattern_pixels = src->max_bayer_pattern_pixels;
  dst->max_items = src->max_items;

  dst->max_color_profile_size = src->max_color_profile_size;
  dst->max_memory_block_size = src->max_memory_block_size;

  dst->max_components = src->max_components;

  dst->max_iloc_extents_per_item = src->max_iloc_extents_per_item;
  dst->max_size_entity_group = src->max_size_entity_group;

  dst->max_children_per_box = src->max_children_per_box;

  if (src->version >= 2) {
    dst->max_total_memory = src->max_total_memory;
    dst->max_sample_description_box_entries = src->max_sample_description_box_entries;
    dst->max_sample_group_description_box_entries = src->max_sample_group_description_box_entries;
  }

  if (src->version >= 3) {
    dst->max_sequence_frames = src->max_sequence_frames;
    dst->max_number_of_file_brands = src->max_number_of_file_brands;
  }

  if (src->version >= 4) {
    dst->max_bad_pixels = src->max_bad_pixels;
    dst->max_iso23001_17_pixel_size_bytes = src->max_iso23001_17_pixel_size_bytes;
  }

  // `parent` is an internal field; user-supplied limits are always treated as
  // a root context. dst is HeifContext::m_limits, which is registered.
  dst->parent = nullptr;
}


void HeifContext::set_security_limits(const heif_security_limits* limits)
{
  // copy default limits
  if (limits->version < global_security_limits.version) {
    copy_security_limits(&m_limits, &global_security_limits);
  }

  // overwrite with input limits
  copy_security_limits(&m_limits, limits);
}


void HeifContext::set_unif(bool flag)
{
  m_heif_file->get_id_creator().set_unif(flag);
}


bool HeifContext::get_unif() const
{
  return m_heif_file->get_id_creator().get_unif();
}


IDCreator& HeifContext::get_id_creator()
{
  return m_heif_file->get_id_creator();
}


Error HeifContext::read(const std::shared_ptr<StreamReader>& reader)
{
  m_heif_file = std::make_shared<HeifFile>();
  m_heif_file->set_security_limits(&m_limits);
  Error err = m_heif_file->read(reader);
  if (err) {
    return err;
  }

  return interpret_heif_file();
}

Error HeifContext::read_from_file(const char* input_filename)
{
  m_heif_file = std::make_shared<HeifFile>();
  m_heif_file->set_security_limits(&m_limits);
  Error err = m_heif_file->read_from_file(input_filename);
  if (err) {
    return err;
  }

  return interpret_heif_file();
}

Error HeifContext::read_from_memory(const void* data, size_t size, bool copy)
{
  m_heif_file = std::make_shared<HeifFile>();
  m_heif_file->set_security_limits(&m_limits);
  Error err = m_heif_file->read_from_memory(data, size, copy);
  if (err) {
    return err;
  }

  return interpret_heif_file();
}

void HeifContext::reset_to_empty_heif()
{
  m_heif_file = std::make_shared<HeifFile>();
  m_heif_file->set_security_limits(&m_limits);
  m_heif_file->new_empty_file();

  m_all_images.clear();
  m_top_level_images.clear();
  m_primary_image.reset();
}


std::vector<std::shared_ptr<ImageItem>> HeifContext::get_top_level_images(bool return_error_images)
{
  if (return_error_images) {
    return m_top_level_images;
  }
  else {
    std::vector<std::shared_ptr<ImageItem>> filtered;
    for (auto& item : m_top_level_images) {
      if (!item->get_item_error()) {
        filtered.push_back(item);
      }
    }

    return filtered;
  }
}


std::shared_ptr<ImageItem> HeifContext::get_image(heif_item_id id, bool return_error_images)
{
  auto iter = m_all_images.find(id);
  if (iter == m_all_images.end()) {
    return nullptr;
  }
  else {
    if (iter->second->get_item_error() && !return_error_images) {
      return nullptr;
    }
    else {
      return iter->second;
    }
  }
}


std::shared_ptr<ImageItem> HeifContext::get_primary_image(bool return_error_image)
{
  if (m_primary_image == nullptr)
    return nullptr;
  else if (!return_error_image && m_primary_image->get_item_error())
    return nullptr;
  else
    return m_primary_image;
}


std::shared_ptr<const ImageItem> HeifContext::get_primary_image(bool return_error_image) const
{
  return const_cast<HeifContext*>(this)->get_primary_image(return_error_image);
}


bool HeifContext::is_image(heif_item_id ID) const
{
  return m_all_images.contains(ID);
}


Result<std::shared_ptr<RegionItem>> HeifContext::add_region_item(uint32_t reference_width, uint32_t reference_height)
{
  auto boxResult = m_heif_file->add_new_infe_box(fourcc("rgan"));
  if (!boxResult) {
    return boxResult.error();
  }
  auto box = *boxResult;
  box->set_hidden_item(true);

  auto regionItem = std::make_shared<RegionItem>(box->get_item_ID(), reference_width, reference_height);
  add_region_item(regionItem);

  return regionItem;
}

void HeifContext::add_region_referenced_mask_ref(heif_item_id region_item_id, heif_item_id mask_item_id)
{
  m_heif_file->add_iref_reference(region_item_id, fourcc("mask"), {mask_item_id});
}


static uint64_t rescale(uint64_t duration, uint32_t old_base, uint32_t new_base)
{
  // prevent division by zero
  // TODO: we might emit an error in this case
  if (old_base == 0) {
    return 0;
  }

  return duration * new_base / old_base;
}


Error HeifContext::write(StreamWriter& writer)
{
  // Writing is only implemented for contexts that were built in memory. In a context read
  // from a file, the item and sample data still live in the input file and would not be
  // copied to the output: the result would be a truncated file that no reader accepts.
  if (m_heif_file->get_reader()) {
    return Error(heif_error_Unsupported_feature,
                 heif_suberror_Unspecified,
                 "Writing a context that was read from a file is not supported");
  }

  // --- finalize some parameters

  uint64_t max_sequence_duration = 0;
  if (auto mvhd = m_heif_file->get_mvhd_box()) {
    for (const auto& track : m_tracks) {
      track.second->finalize_track();

      // rescale track duration to movie timescale units

      uint64_t track_duration_in_media_units = track.second->get_duration_in_media_units();
      uint32_t media_timescale = track.second->get_timescale();

      uint32_t mvhd_timescale = m_heif_file->get_mvhd_box()->get_time_scale();
      if (mvhd_timescale == 0) {
        mvhd_timescale = track.second->get_timescale();
        m_heif_file->get_mvhd_box()->set_time_scale(mvhd_timescale);
      }

      uint64_t movie_duration = rescale(track_duration_in_media_units, media_timescale, mvhd_timescale);
      uint64_t unrepeated_movie_duration = movie_duration;

      // sequence repetitions

      if (m_sequence_repetitions == heif_sequence_maximum_number_of_repetitions) {
        movie_duration = std::numeric_limits<uint64_t>::max();
      }
      else {
        if (std::numeric_limits<uint64_t>::max() / m_sequence_repetitions < movie_duration) {
          movie_duration = std::numeric_limits<uint64_t>::max();
        }
        else {
          movie_duration *= m_sequence_repetitions;
        }
      }

      if (m_sequence_repetitions != 1) {
        track.second->enable_edit_list_repeat_mode(true);
      }

      track.second->set_track_duration_in_movie_units(movie_duration, unrepeated_movie_duration);

      max_sequence_duration = std::max(max_sequence_duration, movie_duration);
    }

    mvhd->set_duration(max_sequence_duration);
  }

  // --- serialize regions

  for (auto& image : m_all_images) {
    for (auto region : image.second->get_region_item_ids()) {
      m_heif_file->add_iref_reference(region,
                                      fourcc("cdsc"), {image.first});
    }
  }

  for (auto& region : m_region_items) {
    std::vector<uint8_t> data_array;
    Error err = region->encode(data_array);
    if (err) {
      return err;
    }

    m_heif_file->append_iloc_data(region->item_id, data_array, 0);
  }

  // --- serialise text items

  for (auto& image : m_all_images) {
    for (auto text_item_id : image.second->get_text_item_ids()) {
      m_heif_file->add_iref_reference(text_item_id, fourcc("text"), {image.first});
    }
  }

  for (auto& text_item : m_text_items) {
    auto encodeResult = text_item->encode();
    if (encodeResult) {
      m_heif_file->append_iloc_data(text_item->get_item_id(), *encodeResult, 1);
    }
  }

  // --- post-process images

  for (auto& img : m_all_images) {
    Error err = img.second->process_before_write();
    if (err) {
      return err;
    }
  }

  // --- sort item properties

  if (auto ipma = m_heif_file->get_ipma_box()) {
    ipma->sort_properties(m_heif_file->get_ipco_box());
  }

  // --- derive box versions

  m_heif_file->derive_box_versions();

  // --- determine brands

  heif_brand2 main_brand;
  std::vector<heif_brand2> compatible_brands;
  compatible_brands = compute_compatible_brands(this, &main_brand);

  // Note: major brand should be repeated in the compatible brands, according to this:
  //   ISOBMFF (ISO/IEC 14496-12:2020) § K.4:
  //   NOTE This document requires that the major brand be repeated in the compatible-brands,
  //   but this requirement is relaxed in the 'profiles' parameter for compactness.
  // See https://github.com/strukturag/libheif/issues/478

  auto ftyp = m_heif_file->get_ftyp_box();

  // set major brand if not set manually yet
  if (ftyp->get_major_brand() == 0) {
    ftyp->set_major_brand(main_brand);
  }

  // A file without images and without an image sequence (e.g. only metadata items) has no
  // brand that describes it. Instead of writing a file with an all-zero 'ftyp' box that no
  // reader (including libheif) accepts, refuse to write it.
  if (ftyp->get_major_brand() == 0) {
    return Error(heif_error_Usage_error,
                 heif_suberror_Unspecified,
                 "Cannot write a file that contains neither images nor an image sequence");
  }

  ftyp->set_minor_version(0);
  for (auto brand : compatible_brands) {
    ftyp->add_compatible_brand(brand);
  }

  // --- write to file

  m_heif_file->write(writer);

  return {};
}

std::string HeifContext::debug_dump_boxes() const
{
  return m_heif_file->debug_dump_boxes();
}

std::string HeifContext::debug_dump_item_data() const
{
  return m_heif_file->debug_dump_item_data();
}


void HeifContext::set_write_mini_format(bool enable)
{
  m_heif_file->set_write_mini_format(enable);
}


static bool item_type_is_image(uint32_t item_type, const std::string& content_type)
{
  return (item_type == fourcc("hvc1") ||
          item_type == fourcc("av01") ||
          item_type == fourcc("grid") ||
          item_type == fourcc("tmap") ||
          item_type == fourcc("tili") ||
          item_type == fourcc("iden") ||
          item_type == fourcc("iovl") ||
          item_type == fourcc("avc1") ||
          item_type == fourcc("unci") ||
          item_type == fourcc("vvc1") ||
          item_type == fourcc("jpeg") ||
          (item_type == fourcc("mime") && content_type == "image/jpeg") ||
          item_type == fourcc("j2k1") ||
          item_type == fourcc("mski"));
}


void HeifContext::remove_top_level_image(const std::shared_ptr<ImageItem>& image)
{
  std::vector<std::shared_ptr<ImageItem>> new_list;

  for (const auto& img : m_top_level_images) {
    if (img != image) {
      new_list.push_back(img);
    }
  }

  m_top_level_images = std::move(new_list);
}


Error HeifContext::interpret_heif_file()
{
  if (m_heif_file->has_images()) {
    Error err = interpret_heif_file_images();
    if (err) {
      return err;
    }
  }

  if (m_heif_file->has_sequences()) {
    Error err = interpret_heif_file_sequences();
    if (err) {
      return err;
    }
  }

  return Error::Ok;
}


Error HeifContext::interpret_heif_file_images()
{
  m_all_images.clear();
  m_top_level_images.clear();
  m_primary_image.reset();


  // --- reference all non-hidden images

  std::vector<heif_item_id> image_IDs = m_heif_file->get_item_IDs();

  for (heif_item_id id : image_IDs) {
    auto infe_box = m_heif_file->get_infe_box(id);
    if (!infe_box) {
      // TODO(farindk): Should we return an error instead of skipping the invalid id?
      continue;
    }

    auto imageItem = ImageItem::alloc_for_infe_box(this, infe_box);
    if (!imageItem) {
      // It is no imageItem item, skip it.
      continue;
    }

    std::vector<std::shared_ptr<Box>> properties;
    Error err = m_heif_file->get_properties(id, properties);
    if (err) {
      imageItem = std::make_shared<ImageItem_Error>(this, imageItem->get_infe_type(), id, err);
    }

    imageItem->set_properties(properties);

    err = imageItem->initialize_decoder();
    if (err) {
      imageItem = std::make_shared<ImageItem_Error>(this, imageItem->get_infe_type(), id, err);
      imageItem->set_properties(properties);
    } else {
      // The decoder's input data extent must be set before any codec-config
      // query: some decoders (e.g. JPEG, whose jpgC box is optional) read the
      // actual bitstream to answer colorspace/bit-depth queries.
      imageItem->set_decoder_input_data();

      // After initialize_decoder, codec-config queries (colorspace, bit depth)
      // are available, so visual-codec items can now populate their component
      // descriptions. Idempotent for items already populated by set_properties
      // (e.g. unci items).
      imageItem->populate_component_descriptions();
    }

    m_all_images.insert(std::make_pair(id, imageItem));

    if (!infe_box->is_hidden_item()) {
      if (id == m_heif_file->get_primary_image_ID()) {
        imageItem->set_primary(true);
        m_primary_image = imageItem;
      }

      m_top_level_images.push_back(imageItem);
    }
  }

  if (!m_primary_image) {
    return Error(heif_error_Invalid_input,
                 heif_suberror_Nonexisting_item_referenced,
                 "'pitm' box references an unsupported or non-existing image");
  }


  // --- process image properties

  for (auto& pair : m_all_images) {
    auto& image = pair.second;

    if (image->get_item_error()) {
      continue;
    }

    std::vector<std::shared_ptr<Box>> properties;

    Error err = m_heif_file->get_properties(pair.first, properties);
    if (err) {
      return err;
    }


    // --- are there any 'essential' properties that we did not parse?

    for (const auto& prop : properties) {
      if (std::dynamic_pointer_cast<Box_other>(prop) &&
          get_heif_file()->get_ipco_box()->is_property_essential_for_item(pair.first, prop, get_heif_file()->get_ipma_box())) {

        std::stringstream sstr;
        sstr << "could not parse item property '" << prop->get_type_string() << "'";
        return {heif_error_Unsupported_feature, heif_suberror_Unsupported_essential_property, sstr.str()};
      }
    }


    // --- Are there any `rref` reference types that we do not process.
    // This only makes the affected item undecodable; other items i…8889 tokens truncated…_name());

    case heif_compression_mask: {
      error = encode_image_as_mask(pixel_image,
                                  encoder,
                                  options,
                                  input_class,
                                  out_image);
    }
      break;

    default:
      return Error(heif_error_Encoder_plugin_error, heif_suberror_Unsupported_codec);
  }
#endif


  // --- check whether we have to convert the image color space

  // The reason for doing the color conversion here is that the input might be an RGBA image and the color conversion
  // will extract the alpha plane anyway. We can reuse that plane below instead of having to do a new conversion.

  heif_encoding_options options = in_options;

  std::shared_ptr<HeifPixelImage> colorConvertedImage;

  if (output_image_item->get_encoder()) {
    if (const auto* nclx = output_image_item->get_encoder()->get_forced_output_nclx()) {
      options.output_nclx_profile = const_cast<heif_color_profile_nclx*>(nclx);
    }

    Result<std::shared_ptr<HeifPixelImage>> srcImageResult;
    srcImageResult = output_image_item->get_encoder()->convert_colorspace_for_encoding(pixel_image,
                                                                                       encoder,
                                                                                       options.output_nclx_profile,
                                                                                       &options.color_conversion_options,
                                                                                       get_security_limits());
    if (!srcImageResult) {
      return srcImageResult.error();
    }

    colorConvertedImage = *srcImageResult;
  }
  else {
    colorConvertedImage = pixel_image;
  }

  Error err = output_image_item->encode_to_item(this,
                                                colorConvertedImage,
                                                encoder, options, input_class);
  if (err) {
    return err;
  }

  insert_image_item(output_image_item->get_id(), output_image_item);


  // --- if there is an alpha channel, add it as an additional image

  if (options.save_alpha_channel &&
      colorConvertedImage->has_alpha() &&
      output_image_item->get_auxC_alpha_channel_type() != nullptr) { // does not need a separate alpha aux image

    // --- generate alpha image
    // TODO: can we directly code a monochrome image instead of the dummy color channels?

    std::shared_ptr<HeifPixelImage> alpha_image;
    auto alpha_image_result = create_alpha_image_from_image_alpha_channel(colorConvertedImage, get_security_limits());
    if (!alpha_image_result) {
      return alpha_image_result.error();
    }

    alpha_image = *alpha_image_result;


    // --- encode the alpha image using a fresh encoder instance to avoid
    //     leaking codec state from the color encoding pass

    heif_encoder alpha_enc(encoder->plugin);
    heif_error alloc_err = alpha_enc.alloc();
    if (alloc_err.code) {
      return Error(alloc_err.code, alloc_err.subcode, alloc_err.message);
    }
    alpha_enc.copy_parameters_from(*encoder);

    auto alphaEncodingResult = encode_image(alpha_image, &alpha_enc, options,
                         heif_image_input_class_alpha);
    if (!alphaEncodingResult) {
      return alphaEncodingResult.error();
    }

    std::shared_ptr<ImageItem> heif_alpha_image = *alphaEncodingResult;

    m_heif_file->add_iref_reference(heif_alpha_image->get_id(), fourcc("auxl"), {output_image_item->get_id()});
    if (Error err = m_heif_file->set_auxC_property(heif_alpha_image->get_id(), output_image_item->get_auxC_alpha_channel_type())) {
      return err;
    }

    if (pixel_image->is_premultiplied_alpha()) {
      m_heif_file->add_iref_reference(output_image_item->get_id(), fourcc("prem"), {heif_alpha_image->get_id()});
    }
  }

  std::vector<std::shared_ptr<Box>> properties;
  err = m_heif_file->get_properties(output_image_item->get_id(), properties);
  if (err) {
    return err;
  }
  output_image_item->set_properties(properties);

  //m_heif_file->set_brand(encoder->plugin->compression_format,
  //                       output_image_item->is_miaf_compatible());

  return output_image_item;
}


void HeifContext::set_primary_image(const std::shared_ptr<ImageItem>& image)
{
  // update heif context

  if (m_primary_image) {
    m_primary_image->set_primary(false);
  }

  image->set_primary(true);
  m_primary_image = image;


  // update pitm box in HeifFile

  m_heif_file->set_primary_item_id(image->get_id());
}


Error HeifContext::assign_thumbnail(const std::shared_ptr<ImageItem>& master_image,
                                    const std::shared_ptr<ImageItem>& thumbnail_image)
{
  m_heif_file->add_iref_reference(thumbnail_image->get_id(),
                                  fourcc("thmb"), {master_image->get_id()});

  return Error::Ok;
}


Result<std::shared_ptr<ImageItem>> HeifContext::encode_thumbnail(const std::shared_ptr<HeifPixelImage>& image,
                                                                 heif_encoder* encoder,
                                                                 const heif_encoding_options& options,
                                                                 int bbox_size)
{
  int orig_width = image->get_width();
  int orig_height = image->get_height();

  int thumb_width, thumb_height;

  if (orig_width <= bbox_size && orig_height <= bbox_size) {
    // original image is smaller than thumbnail size -> do not encode any thumbnail

    return Error::Ok;
  }
  else if (orig_width > orig_height) {
    thumb_height = orig_height * bbox_size / orig_width;
    thumb_width = bbox_size;
  }
  else {
    thumb_width = orig_width * bbox_size / orig_height;
    thumb_height = bbox_size;
  }


  // round size to even width and height

  thumb_width &= ~1;
  thumb_height &= ~1;


  std::shared_ptr<HeifPixelImage> thumbnail_image;
  Error error = image->scale_nearest_neighbor(thumbnail_image, thumb_width, thumb_height, get_security_limits());
  if (error) {
    return error;
  }

  auto encodingResult = encode_image(thumbnail_image,
                       encoder, options,
                       heif_image_input_class_thumbnail);
  if (!encodingResult) {
    return encodingResult.error();
  }

  return *encodingResult;
}


Error HeifContext::add_exif_metadata(const std::shared_ptr<ImageItem>& master_image, const void* data, int size)
{
  // find location of TIFF header
  uint32_t offset = 0;
  const char* tiffmagic1 = "MM\0*";
  const char* tiffmagic2 = "II*\0";
  while (offset + 4 < (unsigned int) size) {
    if (!memcmp((uint8_t*) data + offset, tiffmagic1, 4)) break;
    if (!memcmp((uint8_t*) data + offset, tiffmagic2, 4)) break;
    offset++;
  }
  if (offset >= (unsigned int) size) {
    return Error(heif_error_Usage_error,
                 heif_suberror_Invalid_parameter_value,
                 "Could not find location of TIFF header in Exif metadata.");
  }


  std::vector<uint8_t> data_array;
  data_array.resize(size + 4);
  data_array[0] = (uint8_t) ((offset >> 24) & 0xFF);
  data_array[1] = (uint8_t) ((offset >> 16) & 0xFF);
  data_array[2] = (uint8_t) ((offset >> 8) & 0xFF);
  data_array[3] = (uint8_t) ((offset) & 0xFF);
  memcpy(data_array.data() + 4, data, size);


  return add_generic_metadata(master_image,
                              data_array.data(), (int) data_array.size(),
                              fourcc("Exif"), nullptr, nullptr, heif_metadata_compression_off, nullptr);
}


Error HeifContext::add_XMP_metadata(const std::shared_ptr<ImageItem>& master_image, const void* data, int size,
                                    heif_metadata_compression compression)
{
  return add_generic_metadata(master_image, data, size, fourcc("mime"), "application/rdf+xml", nullptr, compression, nullptr);
}


Error HeifContext::add_generic_metadata(const std::shared_ptr<ImageItem>& master_image, const void* data, int size,
                                        uint32_t item_type, const char* content_type, const char* item_uri_type, heif_metadata_compression compression,
                                        heif_item_id* out_item_id)
{
  // create an infe box describing what kind of data we are storing (this also creates a new ID)

  auto infe_result = m_heif_file->add_new_infe_box(item_type);
  if (!infe_result) {
    return infe_result.error();
  }
  auto metadata_infe_box = *infe_result;
  metadata_infe_box->set_hidden_item(true);
  if (content_type != nullptr) {
    metadata_infe_box->set_content_type(content_type);
  }

  heif_item_id metadata_id = metadata_infe_box->get_item_ID();
  if (out_item_id) {
    *out_item_id = metadata_id;
  }


  // we assign this data to the image

  m_heif_file->add_iref_reference(metadata_id,
                                  fourcc("cdsc"), {master_image->get_id()});


  // --- metadata compression

  if (compression == heif_metadata_compression_auto) {
    compression = heif_metadata_compression_off; // currently, we don't use header compression by default
  }

  // only set metadata compression for MIME type data which has 'content_encoding' field
  if (compression != heif_metadata_compression_off &&
      item_type != fourcc("mime")) {
    // TODO: error, compression not supported
  }


  std::vector<uint8_t> data_array;
  if (compression == heif_metadata_compression_zlib) {
#if HAVE_ZLIB
    data_array = compress_zlib((const uint8_t*) data, size);
    metadata_infe_box->set_content_encoding("compress_zlib");
#else
    return Error(heif_error_Unsupported_feature,
                 heif_suberror_Unsupported_header_compression_method);
#endif
  }
  else if (compression == heif_metadata_compression_deflate) {
#if HAVE_ZLIB
    data_array = compress_zlib((const uint8_t*) data, size);
    metadata_infe_box->set_content_encoding("deflate");
#else
    return Error(heif_error_Unsupported_feature,
                 heif_suberror_Unsupported_header_compression_method);
#endif
  }
  else {
    // uncompressed data, plain copy

    if (size > 0) { // memcpy() with a NULL pointer is UB even for size 0 (until C2y/N3322), see StreamReader_memory::read()
      data_array.resize(size);
      memcpy(data_array.data(), data, size);
    }
  }

  // copy the data into the file, store the pointer to it in an iloc box entry

  m_heif_file->append_iloc_data(metadata_id, data_array, 0);

  return Error::Ok;
}


Result<heif_property_id> HeifContext::add_property(heif_item_id targetItem, const std::shared_ptr<Box>& property, bool essential)
{
  // Like writing, adding properties is only implemented for contexts that were built in memory.
  // In a context read from a file, the item data still lives in the input file and the context
  // cannot be written out again (see HeifContext::write()).
  if (m_heif_file->get_reader()) {
    return Error(heif_error_Unsupported_feature,
                 heif_suberror_Unspecified,
                 "Adding a property to a context that was read from a file is not supported");
  }

  heif_property_id id;

  if (auto img = get_image(targetItem, false)) {
    id = img->add_property(property, essential);
  }
  else {
    id = m_heif_file->add_property(targetItem, property, essential);
  }

  if (id == 0) {
    return Error(heif_error_Encoding_error,
                 heif_suberror_Unspecified,
                 "Cannot add property to item");
  }

  return id;
}


Result<heif_item_id> HeifContext::add_pyramid_group(const std::vector<heif_item_id>& layer_item_ids)
{
  struct pymd_entry
  {
    std::shared_ptr<ImageItem> item;
    uint32_t width = 0;
  };

  // --- sort all images by size

  std::vector<pymd_entry> pymd_entries;
  for (auto id : layer_item_ids) {
    auto image_item = get_image(id, true);
    if (auto error = image_item->get_item_error()) {
      return error;
    }

    pymd_entry entry;
    entry.item = image_item;
    entry.width = image_item->get_width();
    pymd_entries.emplace_back(entry);
  }

  std::sort(pymd_entries.begin(), pymd_entries.end(), [](const pymd_entry& a, const pymd_entry& b) {
    return a.width < b.width;
  });


  // --- generate pymd box

  auto pymd = std::make_shared<Box_pymd>();
  std::vector<Box_pymd::LayerInfo> layers;
  std::vector<heif_item_id> ids;

  auto base_item = pymd_entries.back().item;

  uint32_t tile_w=0, tile_h=0;
  base_item->get_tile_size(tile_w, tile_h);

  uint32_t last_width=0, last_height=0;

  for (const auto& entry : pymd_entries) {
    auto layer_item = entry.item;

    if (false) {
      // according to pymd definition, we should check that all layers have the same tile size
      uint32_t item_tile_w = 0, item_tile_h = 0;
      base_item->get_tile_size(item_tile_w, item_tile_h);
      if (item_tile_w != tile_w || item_tile_h != tile_h) {
        // TODO: add warning that tile sizes are not the same
      }
    }

    heif_image_tiling tiling = layer_item->get_heif_image_tiling();

    if (tiling.image_width < last_width || tiling.image_height < last_height) {
      return Error{
        heif_error_Invalid_input,
        heif_suberror_Invalid_parameter_value,
        "Multi-resolution pyramid images have to be provided ordered from smallest to largest."
      };
    }

    last_width = tiling.image_width;
    last_height = tiling.image_height;

    Box_pymd::LayerInfo layer{};
    layer.layer_binning = (uint16_t)(base_item->get_width() / tiling.image_width);
    layer.tiles_in_layer_row_minus1 = static_cast<uint16_t>(tiling.num_rows - 1);
    layer.tiles_in_layer_column_minus1 = static_cast<uint16_t>(tiling.num_columns - 1);
    layers.push_back(layer);
    ids.push_back(layer_item->get_id());
  }

  auto groupIdResult = m_heif_file->get_id_creator().get_new_id(IDCreator::Namespace::entity_group);
  if (!groupIdResult) {
    return groupIdResult.error();
  }
  heif_item_id group_id = *groupIdResult;

  pymd->set_group_id(group_id);
  pymd->set_layers((uint16_t)tile_w, (uint16_t)tile_h, layers, ids);

  m_heif_file->add_entity_group_box(pymd);

  // add back-references to base image

  for (size_t i = 0; i < ids.size() - 1; i++) {
    m_heif_file->add_iref_reference(ids[i], fourcc("base"), {ids.back()});
  }

  return {group_id};
}


Result<heif_property_id> HeifContext::add_text_property(heif_item_id itemId, const std::string& language)
{
  if (find_property<Box_elng>(itemId)) {
    return Error{
      heif_error_Usage_error,
      heif_suberror_Unspecified,
      "Item already has an 'elng' language property."
    };
  }

  auto elng = std::make_shared<Box_elng>();
  elng->set_lang(std::string(language));

  return add_property(itemId, elng, false);
}


Error HeifContext::interpret_heif_file_sequences()
{
  m_tracks.clear();


  // --- reference all non-hidden images

  auto moov = m_heif_file->get_moov_box();
  assert(moov);

  auto mvhd = moov->get_child_box<Box_mvhd>();
  if (!mvhd) {
    assert(false); // TODO
  }

  auto tracks = moov->get_child_boxes<Box_trak>();
  for (const auto& track_box : tracks) {
    auto trackResult = Track::alloc_track(this, track_box);
    bool skip_track = false;

    if (auto err = trackResult.error()) {
      if (err.error_code == heif_error_Unsupported_feature &&
          err.sub_error_code == heif_suberror_Unsupported_track_type) {
        // ignore error, skip track
        skip_track = true;
      }
      else {
        return trackResult.error();
      }
    }

    if (!skip_track) {
      assert(*trackResult);
      auto track = *trackResult;
      m_tracks.insert({track->get_id(), track});

      if (track->is_visual_track() && m_visual_track_id == 0) {
        m_visual_track_id = track->get_id();
      }
    }
  }

  // --- post-parsing initialization

  std::vector<std::shared_ptr<Track>> all_tracks;
  all_tracks.reserve(m_tracks.size());
  for (auto& track : m_tracks) {
   all_tracks.push_back(track.second);
  }

  for (auto& track : m_tracks) {
    Error err = track.second->initialize_after_parsing(this, all_tracks);
    if (err) {
      return err;
    }
  }

  return Error::Ok;
}


std::vector<uint32_t> HeifContext::get_track_IDs() const
{
  std::vector<uint32_t> ids;

  ids.reserve(m_tracks.size());
  for (const auto& track : m_tracks) {
    ids.push_back(track.first);
  }

  return ids;
}


Result<std::shared_ptr<Track>> HeifContext::get_track(uint32_t track_id)
{
  // The caller is expected to have confirmed (via has_sequence()) that there are
  // sequence tracks before requesting one. Guard against an empty track map anyway,
  // since this is reachable through the public API (e.g. on a still image file).
  if (!has_sequence()) {
    return Error{heif_error_Usage_error,
                 heif_suberror_Unspecified,
                 "File contains no sequence tracks"};
  }

  if (track_id != 0) {
    auto iter = m_tracks.find(track_id);
    if (iter == m_tracks.end()) {
      return Error{heif_error_Usage_error,
                   heif_suberror_Unspecified,
                   "Invalid track id"};
    }

    return iter->second;
  }

  if (m_visual_track_id != 0) {
    return m_tracks[m_visual_track_id];
  }

  return m_tracks.begin()->second;
}


Result<std::shared_ptr<const Track>> HeifContext::get_track(uint32_t track_id) const
{
  auto result = const_cast<HeifContext*>(this)->get_track(track_id);
  if (!result) {
    return result.error();
  }
  else {
    Result<std::shared_ptr<const Track>> my_result(*result);
    return my_result;
  }
}


uint32_t HeifContext::get_sequence_timescale() const
{
  auto mvhd = m_heif_file->get_mvhd_box();
  if (!mvhd) {
    return 0;
  }

  return mvhd->get_time_scale();
}


void HeifContext::set_sequence_timescale(uint32_t timescale)
{
  get_heif_file()->init_for_sequence();

  auto mvhd = m_heif_file->get_mvhd_box();

  /* unnecessary, since mvhd duration is set during writing

  uint32_t old_timescale = mvhd->get_time_scale();
  if (old_timescale != 0) {
    uint64_t scaled_duration = mvhd->get_duration() * timescale / old_timescale;
    mvhd->set_duration(scaled_duration);
  }
  */

  mvhd->set_time_scale(timescale);
}


void HeifContext::set_number_of_sequence_repetitions(uint32_t repetitions)
{
  m_sequence_repetitions = repetitions;
}


uint64_t HeifContext::get_sequence_duration() const
{
  auto mvhd = m_heif_file->get_mvhd_box();
  if (!mvhd) {
    return 0;
  }

  return mvhd->get_duration();
}


bool HeifContext::is_sequence_duration_indefinite() const
{
  auto mvhd = m_heif_file->get_mvhd_box();
  if (!mvhd) {
    return false;
  }

  return mvhd->is_duration_indefinite();
}


Result<std::shared_ptr<Track_Visual>> HeifContext::add_visual_sequence_track(const TrackOptions* options,
                                                                             uint32_t handler_type,
                                                                             uint16_t width, uint16_t height)
{
  m_heif_file->init_for_sequence();

  std::shared_ptr<Track_Visual> trak = std::make_shared<Track_Visual>(this, 0, width, height, options, handler_type);
  m_tracks.insert({trak->get_id(), trak});

  return trak;
}


Result<std::shared_ptr<class Track_Metadata>> HeifContext::add_uri_metadata_sequence_track(const TrackOptions* options,
                                                                                           const std::string& uri)
{
  m_heif_file->init_for_sequence();

  std::shared_ptr<Track_Metadata> trak = std::make_shared<Track_Metadata>(this, 0, uri, options);
  m_tracks.insert({trak->get_id(), trak});

  return trak;
}

Result<std::shared_ptr<TextItem>> HeifContext::add_text_item(const char* content_type, const char* text)
{
  auto boxResult = m_heif_file->add_new_infe_box(fourcc("mime"));
  if (!boxResult) {
    return boxResult.error();
  }
  auto box = *boxResult;
  box->set_hidden_item(true);
  box->set_content_type(std::string(content_type));
  auto textItem = std::make_shared<TextItem>(box->get_item_ID(), text);
  add_text_item(textItem);
  return textItem;
}
