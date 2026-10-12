/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup blf
 *
 * Text shaping and glyph positioning using HarfBuzz.
 */

#ifdef WITH_HARFBUZZ
#  include <harfbuzz/hb-ft.h>
#  include <harfbuzz/hb-ot.h>
#  include <harfbuzz/hb.h>
#endif

#include <algorithm>

#include "BLI_array.hh"
#include "BLI_rect.hh"
#include "BLI_string.hh"
#include "BLI_string_utf8.hh"
#include "BLI_vector.hh"

#include "BLT_lang.hh"

#include "DNA_userdef_types.h"

#include "blf_internal.hh"
#include "blf_internal_types.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Text Shaping
 * \{ */

rcti ShapedGlyph::integer_bounds() const
{
  rcti r;
  r.xmin = ft_pix_to_int_floor(this->bounds.xmin) + ft_pix_to_int_floor(this->g->box_xmin);
  r.xmax = ft_pix_to_int_floor(this->bounds.xmin) + ft_pix_to_int_ceil(this->g->box_xmax);
  r.ymin = ft_pix_to_int_floor(this->bounds.ymin + this->g->box_ymin);
  r.ymax = ft_pix_to_int_ceil(this->bounds.ymin + this->g->box_ymax);
  return r;
}

bool ShapedGlyph::is_combining() const
{
  return BLI_wcwidth_or_error(char32_t(this->g->c)) == 0;
}

void ShapingData::legacy_layout(FontBLF *font,
                                GlyphCacheBLF *gc,
                                const char *str,
                                const size_t len)
{
  const size_t char_count = BLI_strnlen_utf8(str, len);
  std::u32string str32(char_count + 1, 0);
  BLI_str_utf8_as_utf32(str32.data(), str, char_count + 1);
  this->bounds.xmin = INT_MAX;
  size_t offset = 0;
  for (size_t i = 0; i < char_count; i++) {
    const char32_t codepoint = str32[i];
    GlyphBLF *g = blf_glyph_ensure(font, gc, codepoint);
    if (g) {
      g = blf_glyph_ensure_subpixel(font, gc, g, this->bounds.xmax);
      const rcti bounds = {this->bounds.xmax, this->bounds.xmax + g->box_xmax, 0, g->box_ymax};
      this->glyphs.append({font, gc, g, bounds, offset});
      this->bounds.xmin = std::min(this->bounds.xmin, bounds.xmin + g->box_xmin);
      this->bounds.xmax += g->advance_x;
      this->bounds.ymin = std::min(this->bounds.ymin, g->box_ymin);
      this->bounds.ymax = std::max(this->bounds.ymax, g->box_ymax);
    }
    offset += size_t(BLI_str_utf8_from_unicode_len(codepoint));
  }
}

#ifdef WITH_HARFBUZZ
static void blf_font_otf_feature_set(Vector<hb_feature_t> &features,
                                     const hb_tag_t tag,
                                     const uint32_t value)
{
  for (hb_feature_t &feature : features) {
    if (feature.tag == tag) {
      feature.value = value;
      return;
    }
  }
  features.append({tag, value, HB_FEATURE_GLOBAL_START, HB_FEATURE_GLOBAL_END});
}
#endif

FeatureSet blf_font_otf_features_base()
{
  static const FeatureSet features = {
      {BLF_OTF_KERN, true},  /* Kerning. */
      {BLF_OTF_LOCL, true},  /* Localized Forms. */
      {BLF_OTF_LIGA, true},  /* Standard Ligatures. */
      {BLF_OTF_CASE, true},  /* Case Sensitive Forms. */
      {BLF_OTF_TNUM, true},  /* Tabular Numbers. */
      {BLF_OTF_HLIG, false}, /* Historical Ligatures. */
      {BLF_OTF_SALT, false}, /* Stylistic Alternates. */
      {BLF_OTF_CLIG, false}, /* Contextual Ligatures. */
  };
  return features;
}

FeatureSet blf_font_otf_features_default()
{
  FeatureSet features = blf_font_otf_features_base();
  features.append({BLF_OTF_DLIG, (U.text_render & USER_TEXT_DISCRETIONARY_LIGATURES_UI) != 0});
  features.append({BLF_OTF_CALT, (U.text_render & USER_TEXT_CONTEXTUAL_ALTERNATES_UI) != 0});
  features.append({BLF_OTF_ZERO, (U.text_render & USER_TEXT_SLASHED_ZERO_UI) != 0});
  features.append({BLF_OTF_SS01, (U.text_render & USER_TEXT_OPEN_DIGITS_INTER) != 0});
  features.append({BLF_OTF_SS04, (U.text_render & USER_TEXT_DISAMBIGUATION_INTER) != 0});
  return features;
}

#ifdef WITH_HARFBUZZ
bool ShapingData::load_from_cache(FontBLF *font, GlyphCacheBLF *gc, const ShapingCacheKey &key)
{
  const CachedString *cached = gc->shaping_cache.lookup_ptr(key);
  if (cached == nullptr) {
    return false;
  }

  for (const CachedGlyph &glyph : cached->glyphs) {
    GlyphBLF *g = blf_glyph_ensure_cached(
        font, gc, glyph.glyph_font, glyph.charcode, glyph.glyph_id, glyph.subpixel);
    if (g == nullptr) [[unlikely]] {
      this->glyphs.clear();
      return false;
    }
    this->glyphs.append({font, gc, g, glyph.bounds, glyph.index_utf8});
  }

  this->bounds = cached->bounds;

  return true;
}

ShapingData::ShapingData(FontBLF *font,
                         GlyphCacheBLF *gc,
                         const char *str,
                         const size_t len,
                         std::optional<FeatureSet> features)
{
  if (!str || !str[0] || !len) {
    return;
  }

  ShapingCacheKey cache_key = {std::string(str, BLI_strnlen(str, len)),
                               features.has_value() ? std::move(*features) :
                                                      blf_font_otf_features_default()};
  if (gc && load_from_cache(font, gc, cache_key)) {
    return;
  }

  Vector<hb_feature_t> otf_features;
  for (const OTFFeature &feature : cache_key.features) {
    blf_font_otf_feature_set(otf_features, feature.tag, feature.value);
  }

  size_t segment_start = 0;
  size_t segment_len = 0;
  FontBLF *segment_font = font;
  hb_script_t script = HB_SCRIPT_UNKNOWN;
  hb_script_t last_script = HB_SCRIPT_UNKNOWN;
  hb_buffer_t *hb_buf = hb_buffer_create();
  this->bounds.xmin = INT_MAX;

  if (!hb_buf) {
    return; /* Out of memory. */
  }

  /* Include space for null terminator. */
  const size_t char_count = BLI_strnlen_utf8(str, len);
  std::u32string str32(char_count + 1, 0);

  /* Convert entire input string into array of 32-bit code points. */
  BLI_str_utf8_as_utf32(str32.data(), str, char_count + 1);

  /* Map UTF-32 code point index -> UTF-8 byte offset, for the whole string.
   * HarfBuzz may reorder glyphs and multiple glyphs can share the same cluster,
   * so we must not rely on glyph iteration order to compute UTF-8 offsets. */
  Array<size_t> utf8_offsets(char_count);
  size_t tmp_offset = 0;
  for (size_t ci = 0; ci < char_count; ++ci) {
    utf8_offsets[ci] = tmp_offset;
    tmp_offset += size_t(BLI_str_utf8_from_unicode_len(str32[ci]));
  }

  /* Process text by script segments. HarfBuzz requires text to be
   * shaped in runs of the same script, direction, and font. We break
   * on script changes, ignoring Common/Inherited which can merge. */
  while ((segment_start + segment_len) < char_count) {
    segment_start += segment_len;
    segment_len = 0;
    size_t i;
    for (i = segment_start; i < char_count; i++) {
      script = hb_unicode_script(hb_unicode_funcs_get_default(), str32[i]);
      if (script != last_script && script != HB_SCRIPT_INHERITED && script != HB_SCRIPT_COMMON) {
        last_script = script;
        if (i > segment_start) {
          break;
        }
      }
    }

    segment_len = (i - segment_start);
    if (segment_len == 0) {
      break;
    }

    hb_buffer_clear_contents(hb_buf);
    hb_buffer_add_utf32(hb_buf,
                        reinterpret_cast<const uint32_t *>(str32.data()),
                        int(char_count),
                        uint(segment_start),
                        int(segment_len));
    hb_buffer_guess_segment_properties(hb_buf);

    script = hb_buffer_get_script(hb_buf);
    if (script == HB_SCRIPT_HAN) {
      hb_buffer_set_language(hb_buf, hb_language_from_string(BLT_lang_get(), -1));
    }

    /* Is the current font ideal for this script? */
    segment_font = font;
    if (!ELEM(script, HB_SCRIPT_COMMON, HB_SCRIPT_INHERITED, HB_SCRIPT_UNKNOWN, HB_SCRIPT_LATIN)) {
      segment_font = blf_font_script_ensure(font, str32[segment_start]);
    }
    if (!segment_font->hb_font) {
      if (blf_ensure_face(segment_font)) {
        segment_font->hb_font = hb_ft_font_create_referenced(segment_font->face);
        hb_ot_font_set_funcs(segment_font->hb_font);
      }
      else {
        segment_font = font;
      }
    }

    hb_font_set_scale(
        segment_font->hb_font, ft_pix_from_float(font->size), ft_pix_from_float(font->size));

    hb_buffer_set_cluster_level(hb_buf, HB_BUFFER_CLUSTER_LEVEL_MONOTONE_CHARACTERS);

    /* Variable font axes, the same as used to render the glyphs of this segment, so the advances
     * match the glyphs (bold and italic, width factor, slant direction, optical size). The glyphs
     * of all segments take their style from the main font. */
    float var_coords[BLF_VARIATIONS_MAX];
    const int var_coords_num = blf_glyph_variation_coords(font, segment_font, var_coords);
    hb_font_set_var_coords_design(segment_font->hb_font, var_coords, uint(var_coords_num));

    hb_segment_properties_t props;
    hb_buffer_get_segment_properties(hb_buf, &props);

    hb_shape(segment_font->hb_font, hb_buf, otf_features.data(), uint(otf_features.size()));

    const int cwidth = std::max(gc->fixed_width, 1);
    uint glyph_count;
    const hb_glyph_info_t *hb_glyph_info = hb_buffer_get_glyph_infos(hb_buf, &glyph_count);
    const hb_glyph_position_t *glyph_pos = hb_buffer_get_glyph_positions(hb_buf, nullptr);

    size_t glyph_str8_offset = 0;
    for (i = 0; i < glyph_count; i++) {
      const uint32_t glyph_id = hb_glyph_info[i].codepoint;
      /* Safety clamp; cluster should normally be within range. */
      const uint32_t cluster = std::min(hb_glyph_info[i].cluster, uint32_t(char_count - 1));
      const char32_t codepoint = str32[cluster];
      GlyphBLF *g = blf_glyph_ensure(font, gc, codepoint, glyph_id, segment_font);
      int advance;
      if (font->flags & BLF_MONOSPACED) {
        advance = ft_pix_from_int(cwidth) * BLI_wcwidth_safe(codepoint);
      }
      else if (g && g->font != segment_font) {
        advance = g->advance_x;
      }
      else {
        advance = glyph_pos[i].x_advance + (g ? g->advance_transform : 0);
      }

      if (g == nullptr) [[unlikely]] {
        /* Still advance pen for missing glyphs using HarfBuzz-provided advance. */
        this->bounds.xmax += advance;
        glyph_str8_offset += BLI_str_utf8_from_unicode_len(codepoint);
        continue;
      }

      if (g->box_xmin == g->box_xmax) {
        /* Can happen with some spacing characters. */
        g->box_xmax = g->box_xmin + advance;
      }

      g = blf_glyph_ensure_subpixel(font, gc, g, this->bounds.xmax);

      const rcti bounds = {this->bounds.xmax + glyph_pos[i].x_offset,
                           this->bounds.xmax + g->box_xmax + glyph_pos[i].x_offset,
                           glyph_pos[i].y_offset,
                           g->box_ymax + glyph_pos[i].y_offset};

      if (props.direction == HB_DIRECTION_RTL) {
        /* Purposely use glyph-ordered offsets to simplify editing for now. */
        this->glyphs.append({font, gc, g, bounds, glyph_str8_offset});
        glyph_str8_offset += BLI_str_utf8_from_unicode_len(codepoint);
      }
      else {
        /* Use precomputed UTF-8 byte offset for the glyph's cluster. */
        this->glyphs.append({font, gc, g, bounds, utf8_offsets[cluster]});
      }

      this->bounds.xmin = std::min(this->bounds.xmin, bounds.xmin + g->box_xmin);
      this->bounds.xmax += advance;
      this->bounds.ymin = std::min(this->bounds.ymin, bounds.ymin + g->box_ymin);
      this->bounds.ymax = std::max(this->bounds.ymax, bounds.ymin + g->box_ymax);
    }
  }
  if (hb_buf) {
    hb_buffer_destroy(hb_buf);
  }

  const size_t glyph_count = this->glyphs.size();
  if (gc && glyph_count > 0 && glyph_count < 128 && !gc->shaping_cache.contains(cache_key)) {
    CachedString cache_string;
    cache_string.str_len = len;
    cache_string.bounds = this->bounds;
    cache_string.glyphs = Array<CachedGlyph>(glyph_count, NoInitialization());
    for (const int64_t i : this->glyphs.index_range()) {
      const ShapedGlyph &glyph = this->glyphs[i];
      cache_string.glyphs[i] = {glyph.g->font,
                                glyph.g->idx,
                                glyph.g->c,
                                glyph.bounds,
                                glyph.index_utf8,
                                glyph.g->subpixel};
    }
    gc->shaping_cache.add_new(std::move(cache_key), std::move(cache_string));
  }
}

#else

/* Fallback when HarfBuzz is not available, legacy layout only. */
ShapingData::ShapingData(FontBLF *font,
                         GlyphCacheBLF *gc,
                         const char *str,
                         const size_t len,
                         std::optional<FeatureSet> /*features*/)
{
  legacy_layout(font, gc, str, len);
}

#endif

void ShapingData::draw(const ft_pix pen_y, ResultBLF *r_info) const
{
  for (const ShapedGlyph &glyph : this->glyphs) {
    const int x = ft_pix_to_int_floor(glyph.bounds.xmin);
    const int y = ft_pix_to_int_floor(pen_y + glyph.bounds.ymin);
    blf_glyph_draw(glyph.font, glyph.gc, glyph.g, x, y);
  }

  if (r_info) {
    r_info->lines = 1;
    r_info->width = ft_pix_to_int(this->bounds.xmax);
  }
}

int ShapingData::draw_mono(const int tab_columns) const
{
  int columns = 0;
  for (const ShapedGlyph &glyph : this->glyphs) {
    const int x = ft_pix_to_int_floor(glyph.bounds.xmin);
    const int y = ft_pix_to_int_floor(glyph.bounds.ymin);
    blf_glyph_draw(glyph.font, glyph.gc, glyph.g, x, y);
    const int col = (glyph.g->c == '\t') ? (tab_columns - (columns % tab_columns)) :
                                           BLI_wcwidth_safe(char32_t(glyph.g->c));
    columns += col;
  }

  return columns;
}

/** A character with its combining marks, in logical order. */
struct LogicalChar {
  size_t offset;
  /** Horizontal space taken up, in ft_pix. */
  ft_pix advance;
  /** Right edge of the ink relative to the start of the character, in ft_pix. */
  ft_pix ink_right;
};

/**
 * Group glyphs into characters sorted by string offset. Glyphs are in visual order, which differs
 * from the logical order inside right-to-left runs.
 */
static Vector<LogicalChar> logical_chars(const ShapingData &text)
{
  const Span<ShapedGlyph> glyphs = text.glyphs;
  Array<int64_t> order(glyphs.size());
  for (const int64_t i : glyphs.index_range()) {
    order[i] = i;
  }
  std::stable_sort(order.begin(), order.end(), [&](const int64_t a, const int64_t b) {
    return glyphs[a].index_utf8 < glyphs[b].index_utf8;
  });

  Vector<LogicalChar> chars;
  ft_pix start = 0;
  for (const int64_t i : order) {
    const ShapedGlyph &glyph = glyphs[i];
    /* The pen advance is the distance to the visually next glyph. */
    const ft_pix next_x = (i + 1 < glyphs.size()) ? glyphs[i + 1].bounds.xmin : text.bounds.xmax;
    const ft_pix advance = next_x - glyph.bounds.xmin;
    if (chars.is_empty() || (!glyph.is_combining() && glyph.index_utf8 != chars.last().offset)) {
      chars.append({glyph.index_utf8, 0, 0});
      start = glyph.bounds.xmin;
    }
    LogicalChar &c = chars.last();
    c.advance += advance;
    c.ink_right = std::max(c.ink_right, glyph.bounds.xmax - start);
  }
  return chars;
}

size_t ShapingData::width_to_strlen(const int width, const size_t str_len, int *r_width) const
{
  const ft_pix width_max = ft_pix_from_int(width);
  ft_pix pen = 0;
  size_t len = str_len;
  for (const LogicalChar &c : logical_chars(*this)) {
    /* Include the whole advance like #blf_font_width does, along with any ink overhanging it. */
    if (pen + std::max(c.advance, c.ink_right) > width_max) {
      len = c.offset;
      break;
    }
    pen += c.advance;
  }

  if (r_width) {
    *r_width = ft_pix_to_int(pen);
  }

  return len;
}

size_t ShapingData::width_to_rstrlen(const int width, const size_t str_len, int *r_width) const
{
  const Vector<LogicalChar> chars = logical_chars(*this);
  const ft_pix width_max = ft_pix_from_int(width);
  ft_pix pen = 0;
  size_t len = chars.is_empty() ? str_len : 0;
  for (int64_t i = chars.size() - 1; i >= 0; i--) {
    if (pen + chars[i].advance > width_max) {
      len = (i + 1 < chars.size()) ? chars[i + 1].offset : str_len;
      break;
    }
    pen += chars[i].advance;
  }

  if (r_width) {
    *r_width = ft_pix_to_int(pen);
  }

  return len;
}

void ShapingData::boundbox(const FontBLF *font,
                           const ft_pix pen_y,
                           rcti *r_box,
                           ResultBLF *r_info) const
{
  const ft_pix pen_x_end = this->bounds.xmax;

  ft_pix box_xmin = ft_pix_from_int(32000);
  ft_pix box_xmax = ft_pix_from_int(-32000);
  ft_pix box_ymin = ft_pix_from_int(32000);
  ft_pix box_ymax = ft_pix_from_int(-32000);

  for (const ShapedGlyph &glyph : this->glyphs) {
    const ft_pix pen_x = glyph.bounds.xmin;
    const ft_pix offset_y = glyph.bounds.ymin;

    box_xmin = std::min({box_xmin, pen_x, pen_x + glyph.g->box_xmin});
    if (!(font->flags & BLF_MONOSPACED)) {
      box_xmax = std::max(box_xmax, pen_x + glyph.g->box_xmax);
    }
    box_ymin = std::min(box_ymin, glyph.g->box_ymin + pen_y + offset_y);
    box_ymax = std::max(box_ymax, glyph.g->box_ymax + pen_y + offset_y);
  }

  if (this->glyphs.is_empty()) {
    box_xmin = 0;
    box_ymin = 0;
    box_xmax = 0;
    box_ymax = 0;
  }
  else {
    box_xmax = std::max(box_xmax, pen_x_end);
  }

  r_box->xmin = ft_pix_to_int_floor(box_xmin);
  r_box->xmax = ft_pix_to_int_ceil(box_xmax);
  r_box->ymin = ft_pix_to_int_floor(box_ymin);
  r_box->ymax = ft_pix_to_int_ceil(box_ymax);

  if (r_info) {
    r_info->lines = 1;
    r_info->width = ft_pix_to_int(pen_x_end);
  }
}

size_t ShapingData::offset_from_cursor_position(const int location_x) const
{
  const ft_pix location = ft_pix_from_int(location_x);
  for (const ShapedGlyph &glyph : this->glyphs) {
    if (glyph.is_combining()) {
      continue;
    }
    if (location < ((glyph.bounds.xmin + glyph.bounds.xmax) / 2)) {
      return glyph.index_utf8;
    }
  }
  return this->glyphs.is_empty() ? 0 : size_t(-1);
}

void ShapingData::offset_to_glyph_bounds(const size_t str_offset, rcti *r_glyph_bounds) const
{
  for (const ShapedGlyph &glyph : this->glyphs) {
    if (glyph.index_utf8 == str_offset) {
      *r_glyph_bounds = glyph.integer_bounds();
      return;
    }
  }
  *r_glyph_bounds = {};
}

/** \} */

}  // namespace blender
