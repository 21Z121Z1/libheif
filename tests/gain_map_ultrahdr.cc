// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libheif contributors
// Links an independently built upstream implementation, never a copied parser.
#include "catch_amalgamated.hpp"
#include "gain_map_metadata.h"
#include "ultrahdr/gainmapmetadata.h"

TEST_CASE("Final Annex C bytes interoperate with upstream libultrahdr")
{
  for (uint8_t count : {uint8_t{1}, uint8_t{3}}) {
    for (bool use_base : {false, true}) {
      GainMapMetadata ours;
      ultrahdr::uhdr_gainmap_metadata_frac google;
      ours.channel_count = count;
      ours.use_base_colour_space = google.useBaseColorSpace = use_base;
      ours.base_hdr_headroom = {1, 2};
      ours.alternate_hdr_headroom = {7, 2};
      google.baseHdrHeadroomN = 1;
      google.baseHdrHeadroomD = 2;
      google.alternateHdrHeadroomN = 7;
      google.alternateHdrHeadroomD = 2;
      for (uint8_t c = 0; c < 3; ++c) {
        const int n = count == 1 ? 0 : c;
        ours.channels[c].gain_map_min = {-1 - n, 3};
        ours.channels[c].gain_map_max = {5 + n, 2};
        ours.channels[c].gamma = {3, 2};
        ours.channels[c].base_offset = {-1, 64};
        ours.channels[c].alternate_offset = {1, 32};
        google.gainMapMinN[c] = -1 - n;
        google.gainMapMinD[c] = 3;
        google.gainMapMaxN[c] = 5 + n;
        google.gainMapMaxD[c] = 2;
        google.gainMapGammaN[c] = 3;
        google.gainMapGammaD[c] = 2;
        google.baseOffsetN[c] = -1;
        google.baseOffsetD[c] = 64;
        google.alternateOffsetN[c] = 1;
        google.alternateOffsetD[c] = 32;
      }
      auto bytes = serialize_gain_map_metadata(ours);
      REQUIRE(bytes);
      ultrahdr::uhdr_gainmap_metadata_frac decoded;
      REQUIRE(ultrahdr::uhdr_gainmap_metadata_frac::decodeGainmapMetadata(*bytes, &decoded).error_code == UHDR_CODEC_OK);
      REQUIRE(decoded.useBaseColorSpace == use_base);
      REQUIRE(decoded.baseHdrHeadroomN == 1);
      REQUIRE(decoded.alternateHdrHeadroomN == 7);
      for (uint8_t c = 0; c < count; ++c) {
        REQUIRE(decoded.gainMapMinN[c] == google.gainMapMinN[c]);
        REQUIRE(decoded.gainMapMaxN[c] == google.gainMapMaxN[c]);
        REQUIRE(decoded.baseOffsetN[c] == -1);
        REQUIRE(decoded.alternateOffsetD[c] == 32);
      }
      std::vector<uint8_t> google_bytes;
      REQUIRE(ultrahdr::uhdr_gainmap_metadata_frac::encodeGainmapMetadata(&google, google_bytes).error_code == UHDR_CODEC_OK);
      REQUIRE(google_bytes == *bytes);
      auto parsed = parse_gain_map_metadata(google_bytes);
      REQUIRE(parsed.status == GainMapMetadataParseStatus::parsed);
      REQUIRE(parsed.metadata->channel_count == count);
      REQUIRE(*serialize_gain_map_metadata(*parsed.metadata) == google_bytes);
    }
  }
}
