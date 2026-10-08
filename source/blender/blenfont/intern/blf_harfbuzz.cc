/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup blf
 *
 * Text shaping and glyph positioning using Harfbuzz.
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

rcti ShapedGlyph::integer_bounds() const
{
  rcti r;
  r.xmin = ft_pix_to_int_floor(bounds.xmin) + ft_pix_to_int_floor(g->box_xmin);
  r.xmax = ft_pix_to_int_floor(bounds.xmin) + ft_pix_to_int_ceil(g->box_xmax);
  r.ymin = ft_pix_to_int_floor(bounds.ymin + g->box_ymin);
  r.ymax = ft_pix_to_int_ceil(bounds.ymin + g->box_ymax);
  return r;
}

bool ShapedGlyph::is_combining() const
{
  return BLI_wcwidth_or_error(char32_t(g->c)) == 0;
}

void ShapingData::legacy_layout(FontBLF *font, GlyphCacheBLF *gc, const char *str, size_t len)
{
  size_t char_count = BLI_strnlen_utf8(str, len);
  std::u32string str32(char_count + 1, 0);
  BLI_str_utf8_as_utf32(str32.data(), str, char_count + 1);
  this->bounds.xmin = INT_MAX;
  size_t offset = 0;
  for (size_t i = 0; i < char_count; i++) {
    const char32_t codepoint = str32[i];
    GlyphBLF *g = blf_glyph_ensure(font, gc, codepoint);
    g = blf_glyph_ensure_subpixel(font, gc, g, this->bounds.xmax);
    if (g) {
      rcti bounds = {this->bounds.xmax, this->bounds.xmax + g->box_xmax, 0, g->box_ymax};
      this->glyphs.append({font, gc, g, bounds, offset});
      this->bounds.xmin = std::min(this->bounds.xmin, bounds.xmin + g->box_xmin);
      this->bounds.xmax += g->advance_x;
      this->bounds.ymin = std::min(this->bounds.ymin, g->box_ymin);
      this->bounds.ymax = std::max(this->bounds.ymax, g->box_ymax);
      offset += size_t(BLI_str_utf8_from_unicode_len(codepoint));
    }
  }
}

#ifdef WITH_HARFBUZZ
static void blf_font_otf_feature_set(blender::Vector<hb_feature_t> &features,
                                     hb_tag_t tag,
                                     uint32_t value)
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
  static FeatureSet features = {
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
bool ShapingData::load_from_cache(FontBLF *font, GlyphCacheBLF *gc, const char *str, size_t len)
{
  const CachedString *cached = gc->shaping_cache.lookup_ptr(str);
  if (cached == nullptr || cached->str_len != len) {
    return false;
  }

  for (const CachedGlyph &glyph : cached->glyphs) {
    GlyphBLF *g = blf_glyph_ensure(font, gc, glyph.charcode, glyph.glyph_id, glyph.subpixel);
    this->glyphs.append({font, gc, g, glyph.bounds, glyph.index_utf8});
  }

  this->bounds = cached->bounds;

  return true;
}

ShapingData::ShapingData(FontBLF *font,
                         GlyphCacheBLF *gc,
                         const char *str,
                         size_t len,
                         std::optional<FeatureSet> features)
{
  if (!str || !str[0] || !len) {
    return;
  }

  if (!features && load_from_cache(font, gc, str, len)) {
    return;
  }

  size_t segment_start = 0;
  size_t segment_len = 0;
  FontBLF *segment_font = font;
  hb_script_t script = HB_SCRIPT_UNKNOWN;
  hb_script_t last_script = HB_SCRIPT_UNKNOWN;
  hb_buffer_t *hb_buf = hb_buffer_create();
  this->bounds.xmin = INT_MAX;
  bool single_gc = true;

  if (!hb_buf) {
    return; /* Out of memory */
  }

  /* Include space for null terminator. */
  size_t char_count = BLI_strnlen_utf8(str, len);
  std::u32string str32(char_count + 1, 0);

  /* Convert entire input string into array of 32-bit code points. */
  BLI_str_utf8_as_utf32(str32.data(), str, char_count + 1);

  /* Process text by script segments. Harfbuzz requires text to be
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
    hb_buffer_add_utf32(
        hb_buf, (uint32_t *)str32.data(), int(char_count), uint(segment_start), int(segment_len));
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

    blender::Vector<hb_feature_t> otf_features;
    for (otf_feature &feature :
         (features.has_value()) ? features.value() : blf_font_otf_features_default())
    {
      blf_font_otf_feature_set(otf_features, feature.tag, feature.value);
    }

    /* Variable font axes. */
    hb_variation_t variations[3];
    variations[0].tag = HB_OT_TAG_VAR_AXIS_WEIGHT;
    variations[0].value = segment_font->char_weight;
    variations[1].tag = HB_OT_TAG_VAR_AXIS_WIDTH;
    variations[1].value = segment_font->char_width;
    variations[2].tag = HB_OT_TAG_VAR_AXIS_SLANT;
    variations[2].value = segment_font->char_slant;
    hb_font_set_variations(segment_font->hb_font, variations, 3);

    const bool need_release = (!gc || segment_font != font);
    GlyphCacheBLF *segment_gc = need_release ? blf_glyph_cache_acquire(segment_font) : gc;
    if (segment_gc != gc) {
      single_gc = false;
    }

    hb_segment_properties_t props;
    hb_buffer_get_segment_properties(hb_buf, &props);

    if (segment_gc->shaping_plan != nullptr &&
        (segment_gc->props.direction != props.direction ||
         segment_gc->props.script != props.script || segment_gc->props.language != props.language))
    {
      hb_shape_plan_destroy(segment_gc->shaping_plan);
      segment_gc->shaping_plan = nullptr;
    }

    if (segment_gc->shaping_plan == nullptr) {
      hb_face_t *face = hb_font_get_face(segment_font->hb_font);
      hb_buffer_get_segment_properties(hb_buf, &segment_gc->props);
      segment_gc->shaping_plan = hb_shape_plan_create_cached(
          face, &segment_gc->props, otf_features.data(), uint(otf_features.size()), nullptr);
    }

    hb_shape_plan_execute(segment_gc->shaping_plan,
                          segment_font->hb_font,
                          hb_buf,
                          otf_features.data(),
                          uint(otf_features.size()));

    /* Unlikely. Drawing monospaced but changed mid-string to a proportional font. */
    bool set_mono = segment_font != font && font->flags & BLF_MONOSPACED &&
                    !(segment_font->flags & BLF_MONOSPACED);
    if (set_mono) {
      segment_font->flags |= BLF_MONOSPACED;
    }

    int cwidth = std::max(gc->fixed_width, 1);
    uint glyph_count;
    hb_glyph_info_t *hb_glyph_info = hb_buffer_get_glyph_infos(hb_buf, &glyph_count);
    hb_glyph_position_t *glyph_pos = hb_buffer_get_glyph_positions(hb_buf, nullptr);

    /* Precompute mapping from UTF-32 codepoint index -> UTF-8 byte offset.
     * HarfBuzz may reorder glyphs and multiple glyphs can share the same cluster,
     * so we must not rely on glyph iteration order to compute UTF-8 offsets. */
    std::vector<size_t> utf8_offsets(char_count);
    size_t tmp_offset = 0;
    for (size_t ci = 0; ci < char_count; ++ci) {
      utf8_offsets[ci] = tmp_offset;
      tmp_offset += size_t(BLI_str_utf8_from_unicode_len(str32[ci]));
    }

    size_t glyph_str8_offset = 0;
    for (i = 0; i < glyph_count; i++) {
      uint32_t glyph_id = hb_glyph_info[i].codepoint;
      unsigned int cluster = hb_glyph_info[i].cluster;
      if (cluster >= char_count) {
        /* Safety clamp; cluster should normally be within range. */
        cluster = unsigned(int(char_count) - 1);
      }
      char32_t codepoint = str32[cluster];
      GlyphBLF *g = blf_glyph_ensure(segment_font, segment_gc, codepoint, glyph_id);
      const int advance = ((font->flags & BLF_MONOSPACED) ?
                               ft_pix_from_int(cwidth) * BLI_wcwidth_safe(codepoint) :
                               glyph_pos[i].x_advance);

      if (UNLIKELY(g == nullptr)) {
        /* Still advance pen for missing glyphs using HarfBuzz-provided advance. */
        this->bounds.xmax += advance;
        glyph_str8_offset += BLI_str_utf8_from_unicode_len(codepoint);
        continue;
      }

      if (g->box_xmin == g->box_xmax) {
        /* Can happen with some spacing characters. */
        g->box_xmax = g->box_xmin + advance;
      }

      g = blf_glyph_ensure_subpixel(segment_font, segment_gc, g, this->bounds.xmax);

      rcti bounds = {this->bounds.xmax + glyph_pos[i].x_offset,
                     this->bounds.xmax + g->box_xmax + glyph_pos[i].x_offset,
                     glyph_pos[i].y_offset,
                     g->box_ymax + glyph_pos[i].y_offset};

      if (props.direction == HB_DIRECTION_RTL) {
        /* Purposely use glyph-ordered offsets to simplify editing for now. */
        this->glyphs.append({segment_font, segment_gc, g, bounds, glyph_str8_offset});
        glyph_str8_offset += BLI_str_utf8_from_unicode_len(codepoint);
      }
      else {
        /* Use precomputed UTF-8 byte offset for the glyph's cluster. */
        this->glyphs.append({segment_font, segment_gc, g, bounds, utf8_offsets[cluster]});
      }

      this->bounds.xmin = std::min(this->bounds.xmin, bounds.xmin + g->box_xmin);
      this->bounds.xmax += advance;
      this->bounds.ymin = std::min(this->bounds.ymin, bounds.ymin + g->box_ymin);
      this->bounds.ymax = std::max(this->bounds.ymax, bounds.ymin + g->box_ymax);
    }

    if (set_mono) {
      segment_font->flags &= ~BLF_MONOSPACED;
    }

    if (need_release) {
      blf_glyph_cache_release(segment_font);
    }
  }
  if (hb_buf) {
    hb_buffer_destroy(hb_buf);
  }

  const size_t glyph_count = this->glyphs.size();
  if (gc && single_gc && glyph_count > 0 && glyph_count < 128 && !gc->shaping_cache.contains(str))
  {
    CachedString cache_string;
    cache_string.str_len = len;
    cache_string.bounds = this->bounds;
    cache_string.glyphs = Array<CachedGlyph>(glyph_count, NoInitialization());
    for (int i = 0; i < glyph_count; i++) {
      cache_string.glyphs[i] = {this->glyphs[i].g->idx,
                                this->glyphs[i].g->c,
                                this->glyphs[i].bounds,
                                this->glyphs[i].index_utf8,
                                this->glyphs[i].g->subpixel};
    }
    gc->shaping_cache.add_new(str, cache_string);
  }
}

#else

/* Fallback when Harfbuzz is not available, legacy layout only. */
ShapingData::ShapingData(FontBLF *font,
                         GlyphCacheBLF *gc,
                         const char *str,
                         size_t len,
                         std::optional<FeatureSet> /*features*/)
{
  legacy_layout(font, gc, str, len);
}

#endif

void ShapingData::draw(const ft_pix pen_y, ResultBLF *r_info)
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

int ShapingData::draw_mono(const int tab_columns)
{
  int columns = 0;
  for (const ShapedGlyph &glyph : this->glyphs) {
    const int x = ft_pix_to_int_floor(glyph.bounds.xmin);
    const int y = ft_pix_to_int_floor(glyph.bounds.ymin);
    blf_glyph_draw(glyph.font, glyph.gc, glyph.g, x, y);
    const int col = UNLIKELY(glyph.g->c == '\t') ? (tab_columns - (columns % tab_columns)) :
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

size_t ShapingData::offset_from_cursor_position(int location_x) const
{
  for (const ShapedGlyph &glyph : this->glyphs) {
    if (glyph.is_combining()) {
      continue;
    }
    if (ft_pix_from_int(location_x) < ((glyph.bounds.xmin + glyph.bounds.xmax) / 2)) {
      return glyph.index_utf8;
    }
  }
  return this->glyphs.is_empty() ? 0 : size_t(-1);
}

void ShapingData::offset_to_glyph_bounds(size_t str_offset, rcti *r_glyph_bounds) const
{
  for (const ShapedGlyph &glyph : this->glyphs) {
    if (glyph.index_utf8 == str_offset) {
      *r_glyph_bounds = glyph.integer_bounds();
      return;
    }
  }
  std::memset(r_glyph_bounds, 0, sizeof(rcti));
}

/** \} */

}  // namespace blender
