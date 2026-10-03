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

#ifndef LIBHEIF_TMAP_H
#define LIBHEIF_TMAP_H

#include "image_item.h"
#include "gain_map_metadata.h"

#include <array>
#include <memory>
#include <vector>


class ImageItem_tmap final : public ImageItem
{
public:
  ImageItem_tmap(HeifContext* ctx, heif_item_id id);

  uint32_t get_infe_type() const override { return fourcc("tmap"); }

  Result<std::array<heif_item_id, 2>> get_input_item_ids() const;

  ToneMapImageParseResult read_tone_map_image() const;

  Error validate_tone_map_structure() const;

  bool use_item_color_profile_for_decoding() const override;
  Error get_coded_image_colorspace(heif_colorspace*, heif_chroma*) const override;
  int get_luma_bits_per_pixel() const override { return 16; }
  int get_chroma_bits_per_pixel() const override { return 16; }

  Result<std::shared_ptr<HeifPixelImage>> decode_compressed_image(
      const heif_decoding_options& options,
      bool decode_tile_only,
      uint32_t tile_x0,
      uint32_t tile_y0,
      DecodeTraversalState decode_state) const override;

  Result<Encoder::CodedImageData> encode(
      const std::shared_ptr<HeifPixelImage>& image,
      heif_encoder* encoder,
      const heif_encoding_options& options,
      heif_image_input_class input_class) override
  {
    return Error{
        heif_error_Unsupported_feature,
        heif_suberror_Unspecified,
        "Cannot encode image directly to 'tmap'"
    };
  }
};


#endif
