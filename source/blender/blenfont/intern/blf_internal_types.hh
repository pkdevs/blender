/* SPDX-FileCopyrightText: 2008 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup blf
 */

#pragma once

#include <atomic>
#include <cmath>

#include "DNA_vec_types.h"

#include "BLF_api.hh"

#include "BLI_map.hh"
#include "BLI_mutex.hh"
#include "BLI_vector.hh"

#include "GPU_shader_shared.hh"
#include "GPU_storage_buffer.hh"
#include "GPU_texture.hh"

#include <ft2build.h>

#ifdef WITH_HARFBUZZ
#  include <harfbuzz/hb.h>
#endif

namespace blender {

struct FontBLF;
struct GlyphCacheBLF;
struct GlyphBLF;

namespace gpu {
class Batch;
class VertBuf;
}  // namespace gpu
struct GPUVertBufRaw;

namespace ocio {
class ColorSpace;
}  // namespace ocio
using ColorSpace = ocio::ColorSpace;

#include FT_MULTIPLE_MASTERS_H /* Variable font support. */

/** Maximum variation axes per font. */
#define BLF_VARIATIONS_MAX 16

#define MAKE_DVAR_TAG(a, b, c, d) \
  ((uint32_t(a) << 24u) | (uint32_t(b) << 16u) | (uint32_t(c) << 8u) | (uint32_t(d)))

/* OpenType Variation Axes. */
#define BLF_VARIATION_AXIS_WEIGHT MAKE_DVAR_TAG('w', 'g', 'h', 't')  /* `wght` weight axis. */
#define BLF_VARIATION_AXIS_SLANT MAKE_DVAR_TAG('s', 'l', 'n', 't')   /* `slnt` slant axis. */
#define BLF_VARIATION_AXIS_WIDTH MAKE_DVAR_TAG('w', 'd', 't', 'h')   /* `wdth` width axis. */
#define BLF_VARIATION_AXIS_SPACING MAKE_DVAR_TAG('s', 'p', 'a', 'c') /* `spac` spacing axis. */
#define BLF_VARIATION_AXIS_OPTSIZE MAKE_DVAR_TAG('o', 'p', 's', 'z') /* `opsz` optical size. */

/* OpenType Features - Ligatures and Substitution. */
#define BLF_OTF_LIGA MAKE_DVAR_TAG('l', 'i', 'g', 'a') /* Standard Ligatures. */
#define BLF_OTF_DLIG MAKE_DVAR_TAG('d', 'l', 'i', 'g') /* Discretionary Ligatures. */
#define BLF_OTF_CLIG MAKE_DVAR_TAG('c', 'l', 'i', 'g') /* Contextual Ligatures. */
#define BLF_OTF_HLIG MAKE_DVAR_TAG('h', 'l', 'i', 'g') /* Historical Ligatures. */
#define BLF_OTF_LOCL MAKE_DVAR_TAG('l', 'o', 'c', 'l') /* Localized Forms. */
#define BLF_OTF_SMCP MAKE_DVAR_TAG('s', 'm', 'c', 'p') /* Small Capitals from Lowercase. */
#define BLF_OTF_S2SC MAKE_DVAR_TAG('s', '2', 's', 'c') /* Small Capitals from Uppercase. */
#define BLF_OTF_SUBS MAKE_DVAR_TAG('s', 'u', 'b', 's') /* Set characters below the baseline. */
#define BLF_OTF_SUPS MAKE_DVAR_TAG('s', 'u', 'p', 's') /* Set characters above the baseline. */
#define BLF_OTF_ORDN MAKE_DVAR_TAG('o', 'r', 'd', 'n') /* Superscripted letters for ordinals. */

/* OpenType Features - Numerals and Figures. */
#define BLF_OTF_LNUM MAKE_DVAR_TAG('l', 'n', 'u', 'm') /* Lining Figures. */
#define BLF_OTF_ONUM MAKE_DVAR_TAG('o', 'n', 'u', 'm') /* Oldstyle Figures. */
#define BLF_OTF_PNUM MAKE_DVAR_TAG('p', 'n', 'u', 'm') /* Proportional Figures. */
#define BLF_OTF_TNUM MAKE_DVAR_TAG('t', 'n', 'u', 'm') /* Tabular Numbers. */
#define BLF_OTF_FRAC MAKE_DVAR_TAG('f', 'r', 'a', 'c') /* Automatic Fractions. */

/* OpenType Features - Alternates and Styling. */
#define BLF_OTF_SALT MAKE_DVAR_TAG('s', 'a', 'l', 't') /* Stylistic Alternates. */
#define BLF_OTF_SS01 MAKE_DVAR_TAG('s', 's', '0', '1') /* Stylistic Set 1. Inter Open Digits. */
#define BLF_OTF_SS02 MAKE_DVAR_TAG('s', 's', '0', '2') /* Stylistic Set 2. Inter Disam w/o 0. */
#define BLF_OTF_SS03 MAKE_DVAR_TAG('s', 's', '0', '3') /* Stylistic Set 3. Inter Round Quotes. */
#define BLF_OTF_SS04 MAKE_DVAR_TAG('s', 's', '0', '4') /* Stylistic Set 4. Inter Disambiguate. */
#define BLF_OTF_SS05 MAKE_DVAR_TAG('s', 's', '0', '5') /* Stylistic Set 5. Inter Circled. */
#define BLF_OTF_SS06 MAKE_DVAR_TAG('s', 's', '0', '6') /* Stylistic Set 6. Inter Square char. */
#define BLF_OTF_SS07 MAKE_DVAR_TAG('s', 's', '0', '7') /* Stylistic Set 7. Inter Square punc. */
#define BLF_OTF_SS08 MAKE_DVAR_TAG('s', 's', '0', '8') /* Stylistic Set 8. Inter Square quote. */
#define BLF_OTF_SS09 MAKE_DVAR_TAG('s', 's', '0', '9') /* Stylistic Set 9. */
#define BLF_OTF_ZERO MAKE_DVAR_TAG('z', 'e', 'r', 'o') /* Slashed Zero. */
#define BLF_OTF_SWSH MAKE_DVAR_TAG('s', 'w', 's', 'h') /* Swash. */
#define BLF_OTF_CALT MAKE_DVAR_TAG('c', 'a', 'l', 't') /* Contextual Alternates. */
#define BLF_OTF_HIST MAKE_DVAR_TAG('h', 'i', 's', 't') /* Historical Forms. */

/* OpenType Features - Positioning and Spacing. */
#define BLF_OTF_KERN MAKE_DVAR_TAG('k', 'e', 'r', 'n') /* Kerning. */
#define BLF_OTF_CASE MAKE_DVAR_TAG('c', 'a', 's', 'e') /* Case Sensitive Forms. */
#define BLF_OTF_VERT MAKE_DVAR_TAG('v', 'e', 'r', 't') /* Vertical Forms. */
#define BLF_OTF_MARK MAKE_DVAR_TAG('m', 'a', 'r', 'k') /* Mark Positioning. */
#define BLF_OTF_MKMK MAKE_DVAR_TAG('m', 'k', 'm', 'k') /* Mark-to-Mark Positioning. */

/* -------------------------------------------------------------------- */
/** \name Sub-Pixel Offset & Utilities
 *
 * Free-type uses fixed point precision for sub-pixel offsets.
 * Utility functions here avoid exposing the details in the BLF API.
 * \{ */

/**
 * This is an internal type that represents sub-pixel positioning,
 * users of this type are to use `ft_pix_*` functions to keep scaling/rounding in one place.
 */
using ft_pix = int32_t;

/* Macros copied from `include/freetype/internal/ftobjs.h`. */

#define FT_PIX_FLOOR(x) ((x) & ~63)
#define FT_PIX_ROUND(x) FT_PIX_FLOOR((x) + 32)
#define FT_PIX_CEIL(x) ((x) + 63)

inline int ft_pix_to_int(ft_pix v)
{
  return int(v >> 6);
}

inline int ft_pix_to_int_floor(ft_pix v)
{
  return int(v >> 6); /* No need for explicit floor as the bits are removed when shifting. */
}

inline int ft_pix_to_int_ceil(ft_pix v)
{
  return (FT_PIX_CEIL(v) >> 6);
}

inline ft_pix ft_pix_from_int(int v)
{
  return v * 64;
}

inline ft_pix ft_pix_from_float(float v)
{
  return lroundf(v * 64.0f);
}

/** \} */

#define BLF_BATCH_DRAW_LEN_MAX 128 /* in glyph */

/* -------------------------------------------------------------------- */
/** \name OpenType Feature Set
 * \{ */

struct OTFFeature {
  uint32_t tag;
  uint32_t value;

  friend bool operator==(const OTFFeature &a, const OTFFeature &b)
  {
    return a.tag == b.tag && a.value == b.value;
  }
};

using FeatureSet = Vector<OTFFeature>;

FeatureSet blf_font_otf_features_base();
FeatureSet blf_font_otf_features_default();

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shaping Cache
 *
 * Least-frequently-used cache of shaped strings, with dynamic aging.
 * \{ */

#define BLF_SHAPING_CACHE_SIZE 256

struct CachedGlyph {
  FontBLF *glyph_font = nullptr;
  uint32_t glyph_id = 0;
  uint32_t charcode = 0;
  rcti bounds = {};
  size_t index_utf8 = 0;
  uint8_t subpixel = 0;
};

struct CachedString {
  Array<CachedGlyph> glyphs;
  uint32_t str_len = 0;
  rcti bounds = {};  /* ft_pix. */
  uint32_t freq = 0; /* Eviction priority: the cache age when added, plus the number of uses. */
};

/**
 * Everything that changes the shaping of a string within one glyph cache: the text, and the
 * OpenType features (UI text uses the features from the preferences, text drawn into image
 * buffers uses #blf_font_otf_features_base).
 */
struct ShapingCacheKey {
  std::string text;
  FeatureSet features;

  uint64_t hash() const
  {
    uint64_t hash = get_default_hash(this->text);
    for (const OTFFeature &feature : this->features) {
      hash = get_default_hash(hash, feature.tag, feature.value);
    }
    return hash;
  }

  friend bool operator==(const ShapingCacheKey &a, const ShapingCacheKey &b)
  {
    return a.text == b.text && a.features.as_span() == b.features.as_span();
  }
};

struct ShapingCache {
  ShapingCache()
  {
    this->data.reserve(BLF_SHAPING_CACHE_SIZE);
  }

  Map<ShapingCacheKey, CachedString> data = {};
  size_t lookups = 0;
  size_t hits = 0;
  size_t inserts = 0;
  size_t evictions = 0;
  uint32_t age = 0;

  const CachedString *lookup_ptr(const ShapingCacheKey &key)
  {
    ++this->lookups;

    CachedString *ptr = this->data.lookup_ptr(key);
    if (ptr == nullptr) {
      return nullptr;
    }

    ++this->hits;
    ++ptr->freq;
    return ptr;
  }

  bool contains(const ShapingCacheKey &key) const
  {
    return this->data.lookup_ptr(key) != nullptr;
  }

  void add_new(ShapingCacheKey key, CachedString &&value)
  {
    ++this->inserts;

    if (this->data.size() >= BLF_SHAPING_CACHE_SIZE) {
      uint32_t best_freq = std::numeric_limits<uint32_t>::max();
      const ShapingCacheKey *evict_key_ptr = nullptr;
      for (const auto item : this->data.items()) {
        const CachedString &v = item.value;
        const uint32_t f = v.freq;
        if (f < best_freq) {
          best_freq = f;
          evict_key_ptr = &item.key;
        }
      }
      if (evict_key_ptr) {
        this->data.remove(*evict_key_ptr);
        ++this->evictions;
        this->age = best_freq;
      }
    }

    value.freq = this->age + 1;
    this->data.add_new(std::move(key), std::move(value));
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shaped Text
 * \{ */

struct ShapedGlyph {
  FontBLF *font = nullptr;
  GlyphCacheBLF *gc = nullptr;
  GlyphBLF *g = nullptr;
  /**
   * String-relative bounds in ft_pix: `xmin`/`ymin` is the glyph origin (pen plus shaping offset),
   * `xmax`/`ymax` is the origin plus the glyph's `box_xmax`/`box_ymax`.
   */
  rcti bounds = {};
  size_t index_utf8 = 0; /* Maps back original UTF-8 string byte offsets. */
  rcti integer_bounds() const;
  bool is_combining() const;
};

struct ShapingData {
  Vector<ShapedGlyph> glyphs = {};
  rcti bounds = {};
  ShapingData(FontBLF *font,
              GlyphCacheBLF *gc,
              const char *str,
              size_t len,
              std::optional<FeatureSet> features = std::nullopt);
  bool load_from_cache(FontBLF *font, GlyphCacheBLF *gc, const ShapingCacheKey &key);
  void legacy_layout(FontBLF *font, GlyphCacheBLF *gc, const char *str, size_t len);

  void draw(ft_pix pen_y, ResultBLF *r_info = nullptr) const;
  int draw_mono(int tab_columns) const;
  size_t width_to_strlen(int width, size_t str_len, int *r_width = nullptr) const;
  size_t width_to_rstrlen(int width, size_t str_len, int *r_width = nullptr) const;
  void boundbox(const FontBLF *font, ft_pix pen_y, rcti *r_box, ResultBLF *r_info = nullptr) const;
  size_t offset_from_cursor_position(int location_x) const;
  void offset_to_glyph_bounds(size_t str_offset, rcti *r_glyph_bounds) const;
};

/** \} */

struct BatchBLF {
  /** Can only batch glyph from the same font. */
  FontBLF *font;
  gpu::Batch *batch;
  gpu::StorageBuf *glyph_buf;
  int glyph_len;
  /** Copy of `font->pos`. */
  int ofs[2];
  /** Previous call `modelmatrix`. */
  float mat[4][4];
  bool enabled, active, simple_shader;
  GlyphCacheBLF *glyph_cache;

  GlyphQuad glyph_data[BLF_BATCH_DRAW_LEN_MAX];
};

extern BatchBLF g_batch;

struct GlyphCacheKey {
  const FontBLF *font;
  uint glyph_index;
  uint8_t subpixel;
  friend bool operator==(const GlyphCacheKey &a, const GlyphCacheKey &b)
  {
    return a.font == b.font && a.glyph_index == b.glyph_index && a.subpixel == b.subpixel;
  }
  uint64_t hash() const
  {
    return get_default_hash(font, glyph_index, subpixel);
  }
};

struct GlyphCacheBLF {
  /** Font size. */
  float size;

  int char_weight;
  float char_slant;
  float char_width;
  float char_spacing;

  bool bold;
  bool italic;

  /** Column width when printing monospaced. */
  int fixed_width;

#ifdef WITH_HARFBUZZ
  ShapingCache shaping_cache;
#endif

  /** The glyphs. */
  Map<GlyphCacheKey, std::unique_ptr<GlyphBLF>> glyphs;

  /** Texture array, to draw the glyphs. */
  gpu::Texture *texture;
  char *bitmap_result;
  int bitmap_len;
  int bitmap_len_landed;
  int bitmap_len_alloc;

  ~GlyphCacheBLF();
};

struct GlyphBLF {
  /** The character, as UTF32. */
  unsigned int c;

  /** Freetype2 index, to speed-up the search. */
  FT_UInt idx;

  FontBLF *font;

  /** Glyph bounding-box. */
  ft_pix box_xmin;
  ft_pix box_xmax;
  ft_pix box_ymin;
  ft_pix box_ymax;

  ft_pix advance_x;
  uint8_t subpixel;

  /** The difference in bearings when hinting is active, zero otherwise. */
  ft_pix lsb_delta;
  ft_pix rsb_delta;

  /** Position inside the texture where this glyph is store. */
  int offset;

  /**
   * Bitmap data, from freetype. Take care that this
   * can be NULL.
   */
  unsigned char *bitmap;

  /** Glyph width and height. */
  int dims[2];
  int pitch;
  int num_channels;

  /**
   * X and Y bearing of the glyph.
   * The X bearing is from the origin to the glyph left bounding-box edge.
   * The Y bearing is from the baseline to the top of the glyph edge.
   */
  int pos[2];

  GlyphCacheBLF *glyph_cache;

  ~GlyphBLF();
};

struct FontBufInfoBLF {
  /** For draw to buffer, always set this to NULL after finish! */
  float *fbuf;

  /** The same but unsigned char. */
  unsigned char *cbuf;

  /** Buffer size, keep signed so comparisons with negative values work. */
  int dims[2];

  /**
   * The number of channels in the buffer. Can be either 1 or 4 for grayscale and color buffers
   * respectively. The red channel of the color is used in case of a grayscale buffer.
   */
  int channel_count;

  /** Color-space of the byte buffer (float is scene linear). */
  const ColorSpace *colorspace;

  /**
   * The color, the alphas is get from the glyph! (color is sRGB space). The red channel of the
   * color is used in case of a grayscale buffer.
   */
  float col_init[4];
  /** Cached conversion from 'col_init'. */
  unsigned char col_char[4];
  float col_float[4];
};

struct FontMetrics {
  /** Indicate that these values have been properly loaded. */
  bool valid;
  /** This font's default weight, 100-900, 400 is normal. */
  short weight;
  /** This font's default width, 1 is normal, 2 is twice as wide. */
  float width;
  /** This font's slant in clockwise degrees, 0 being upright. */
  float slant;
  /** This font's default spacing, 1 is normal. */
  float spacing;

  /** Number of font units in an EM square. 2048, 1024, 1000 are typical. */
  short units_per_EM; /* */
  /** Design classification from OS/2 sFamilyClass. */
  short family_class;
  /** Style classification from OS/2 fsSelection. */
  short selection_flags;
  /** Total number of glyphs in the font. */
  int num_glyphs;
  /** Minimum Unicode index, typically 0x0020. */
  short first_charindex;
  /** Maximum Unicode index, or 0xFFFF if greater than. */
  short last_charindex;

  /**
   * Positive number of font units from baseline to top of typical capitals. Can be slightly more
   * than cap height when head serifs, terminals, or apexes extend above cap line.
   */
  short ascender;
  /** Negative (!) number of font units from baseline to bottom of letters like `gjpqy`. */
  short descender;
  /** Positive number of font units between consecutive baselines. */
  short line_height;
  /** Font units from baseline to lowercase mean line, typically to top of "x". */
  short x_height;
  /** Font units from baseline to top of capital letters, specifically "H". */
  short cap_height;
  /** Ratio width to height of lowercase "O". Reliable indication of font proportion. */
  float o_proportion;
  /** Font unit maximum horizontal advance for all glyphs in font. Can help with wrapping. */
  short max_advance_width;
  /** As above but only for vertical layout fonts, otherwise is set to line_height value. */
  short max_advance_height;

  /** Negative (!) number of font units below baseline to center (!) of underlining stem. */
  short underline_position;
  /** thickness of the underline in font units. */
  short underline_thickness;
  /** Positive number of font units above baseline to the top (!) of strikeout stroke. */
  short strikeout_position;
  /** thickness of the strikeout line in font units. */
  short strikeout_thickness;
  /** EM size font units of recommended subscript letters. */
  short subscript_size;
  /** Horizontal offset before first subscript character, typically 0. */
  short subscript_xoffset;
  /** Positive number of font units above baseline for subscript characters. */
  short subscript_yoffset;
  /** EM size font units of recommended superscript letters. */
  short superscript_size;
  /** Horizontal offset before first superscript character, typically 0. */
  short superscript_xoffset;
  /** Positive (!) number of font units below baseline for subscript characters. */
  short superscript_yoffset;
};

struct FontBLF {
  /** The full path to font file or NULL when from memory. */
  char *filepath;

  /** Pointer to in-memory font, or NULL when from a file. */
  const void *mem;
  size_t mem_size;
  /** Handle for in-memory fonts to avoid loading them multiple times. */
  char *mem_name;

#ifdef WITH_HARFBUZZ
  /** HarfBuzz font, created on demand for text shaping. */
  hb_font_t *hb_font;
#endif

  /**
   * Copied from the SFNT OS/2 table. Bit flags for unicode blocks and ranges
   * considered "functional". Cached here because face might not always exist.
   * See: https://docs.microsoft.com/en-us/typography/opentype/spec/os2#ur
   */
  uint unicode_ranges[4];

  /** Number of references to this font object. When it reaches zero, font is unloaded. */
  std::atomic<uint32_t> reference_count;

  /** Aspect ratio or scale. */
  float aspect[3];

  /** Initial position for draw the text. */
  int pos[3];

  /** Angle in radians. */
  float angle;

  /** Shadow type. */
  FontShadowType shadow;

  /** And shadow offset. */
  int shadow_x;
  int shadow_y;

  /** Shadow color. */
  unsigned char shadow_color[4];

  /** Main text color. */
  unsigned char color[4];

  /** Clipping rectangle. */
  rcti clip_rec;

  /** The width to wrap the text, see #BLF_WORD_WRAP. */
  int wrap_width;
  BLFWrapMode wrap_mode;

  /** Font size. */
  float size;

  /** Axes data for Adobe MM, TrueType GX, or OpenType variation fonts. */
  FT_MM_Var *variations;

  /* Character variations. */

  /** Wight in range: 100 - 900, 400 = normal. */
  int char_weight;
  /** Slant in clockwise degrees. 0.0 = upright. */
  float char_slant;
  /** Factor of normal character width. 1.0 = normal. */
  float char_width;
  /** Factor of normal character spacing. 0.0 = normal. */
  float char_spacing;

  /** Max texture size. */
  int tex_size_max;

  /** Font options. */
  FontFlags flags;

  /**
   * List of glyph caches (#GlyphCacheBLF) for this font for size, DPI, bold, italic.
   * Use `blf_glyph_cache_acquire(font)` and `blf_glyph_cache_release(font)` to access cache!
   */
  Vector<std::unique_ptr<GlyphCacheBLF>> cache;

  /** Freetype2 lib handle. */
  FT_Library ft_lib;

  /** Freetype2 face. */
  FT_Face face;

  /** Point to face->size or to cache's size. */
  FT_Size ft_size;

  /** Copy of the font->face->face_flags, in case we don't have a face loaded. */
  FT_Long face_flags;

  /** Details about the font's design and style and sizes (in un-sized font units). */
  FontMetrics metrics;

  /** Data for buffer usage (drawing into a texture buffer) */
  FontBufInfoBLF buf_info;

  /** Mutex lock for glyph cache. */
  Mutex glyph_cache_mutex;
};

}  // namespace blender
