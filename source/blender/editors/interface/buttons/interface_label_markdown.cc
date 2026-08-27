/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <cfloat>
#include <md4c.h>

#include "BLF_api.hh"

#include "BLI_math_base.hh"
#include "BLI_rect.hh"
#include "BLI_string_utf8.hh"

#include "RNA_access.hh"

#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "interface_intern.hh"
#include "interface_label_markdown.hh"

#include <fmt/format.h>

namespace blender::ui {

bool button_label_is_markdown(const Button *button)
{
  return button->type == ButtonType::Label &&
         static_cast<const ButtonLabel *>(button)->label_type == ButtonLabelType::Markdown;
}

static void markdown_apply_item_font(const MarkdownItemText &item, const float aspect)
{
  BLF_enable(item.fontid, BLF_ASPECT);
  BLF_aspect(item.fontid, aspect, aspect, 1.0f);
  BLF_size(item.fontid, item.size);
  BLF_character_weight(item.fontid, item.weight);
  if (item.italic) {
    BLF_enable(item.fontid, BLF_ITALIC);
  }
  else {
    BLF_disable(item.fontid, BLF_ITALIC);
  }
}

static void markdown_disable_item_font(const MarkdownItemText &item)
{
  BLF_disable(item.fontid, BLF_ASPECT | BLF_ITALIC);
}

struct MdHeaderStyle {
  float size_factor = 1.0f;
  int weight = 400;
  bool italic = false;
};

constexpr std::array<MdHeaderStyle, 6> header_styles = {
    /* H1: Main Title - Largest & Boldest. */
    MdHeaderStyle{.size_factor = 2.00f, .weight = 700, .italic = false},
    /* H2: Major Section. */
    MdHeaderStyle{.size_factor = 1.50f, .weight = 600, .italic = false},
    /* H3: Subsection. */
    MdHeaderStyle{.size_factor = 1.25f, .weight = 600, .italic = false},
    /* H4: Sub-subsection. */
    MdHeaderStyle{.size_factor = 1.00f, .weight = 600, .italic = false},
    /* H5: Small Heading (Same size as body text, but medium-bold). */
    MdHeaderStyle{.size_factor = 1.00f, .weight = 500, .italic = false},
    /* H6: Micro Heading (Same size as body text, but styled with italics). */
    MdHeaderStyle{.size_factor = 1.00f, .weight = 400, .italic = true},
};

class MarkdownLayouter {
 private:
  MarkdownLayoutCache &cache_;

  struct FontConfig {
    int fontid;
    float default_size;
    float default_weight;
    float bold_weight;
    float default_line_height;
  };

  FontConfig main_font_;
  FontConfig mono_font_;

  float x_;
  float y_;

  struct DocBlock {};

  struct HeaderBlock {
    /** Only values from 1 to 6 are valid. */
    int level = 1;
  };

  struct UnorderedListBlock {
    bool is_tight = false;
  };

  struct OrderedListBlock {
    bool is_tight = false;
    int next_number = 1;
  };

  struct ListItemBlock {
    std::optional<int> number = 0;
  };

  struct ParagraphBlock {};

  struct CodeBlock {};

  struct QuoteBlock {};

  struct UnknownBlock {};

  struct EmSpan {};
  struct StrongSpan {};
  struct LinkSpan {
    StringRef url;
  };
  struct CodeSpan {};
  struct UnknownSpan {};

  using MdBlock = std::variant<DocBlock,
                               HeaderBlock,
                               UnorderedListBlock,
                               OrderedListBlock,
                               ListItemBlock,
                               ParagraphBlock,
                               CodeBlock,
                               QuoteBlock,
                               UnknownBlock>;
  using MdSpan = std::variant<EmSpan, StrongSpan, LinkSpan, CodeSpan, UnknownSpan>;

  Vector<MdBlock, 16> block_stack_;
  Vector<MdSpan, 16> span_stack_;
  std::optional<MdBlock> last_left_block_;

 public:
  MarkdownLayouter(MarkdownLayoutCache &cache_) : cache_(cache_)
  {
    main_font_ = make_font_config(style_get()->widget.uifont_id, 12.0f, 400, 700);
    mono_font_ = make_font_config(blf_mono_font, 12.0f, 400, 700);
    x_ = 0.0f;
    y_ = 0.0f;
  }

  FontConfig make_font_config(const int fontid,
                              const float default_size,
                              const float default_weight,
                              const int bold_weight)
  {
    FontConfig config;
    config.fontid = fontid;
    config.default_size = default_size;
    config.default_weight = default_weight;
    config.bold_weight = bold_weight;
    BLF_size(fontid, default_size);
    BLF_character_weight(fontid, default_weight);
    config.default_line_height = BLF_height_max(fontid);
    return config;
  }

  void compute_layout()
  {
    MD_PARSER md_parser{};
    md_parser.enter_block = [](MD_BLOCKTYPE block_type, void *detail, void *userdata) {
      static_cast<MarkdownLayouter *>(userdata)->md_enter_block(block_type, detail);
      return 0;
    };
    md_parser.leave_block = [](MD_BLOCKTYPE block_type, void *detail, void *userdata) {
      static_cast<MarkdownLayouter *>(userdata)->md_leave_block(block_type, detail);
      return 0;
    };
    md_parser.enter_span = [](MD_SPANTYPE span_type, void *detail, void *userdata) {
      static_cast<MarkdownLayouter *>(userdata)->md_enter_span(span_type, detail);
      return 0;
    };
    md_parser.leave_span = [](MD_SPANTYPE span_type, void *detail, void *userdata) {
      static_cast<MarkdownLayouter *>(userdata)->md_leave_span(span_type, detail);
      return 0;
    };
    md_parser.text = [](MD_TEXTTYPE text_type, const char *text, uint32_t length, void *userdata) {
      static_cast<MarkdownLayouter *>(userdata)->md_text(text_type, StringRef(text, length));
      return 0;
    };

    const StringRef text = cache_.key.text;
    md_parse(text.data(), text.size(), &md_parser, this);
    this->finalize_vertical_positions();
  }

  void md_enter_block(const MD_BLOCKTYPE block_type, void *detail)
  {
    switch (block_type) {
      case MD_BLOCK_QUOTE: {
        block_stack_.append(QuoteBlock{});
        break;
      }
      case MD_BLOCK_UL: {
        block_stack_.append(UnorderedListBlock{});
        break;
      }
      case MD_BLOCK_OL: {
        const auto *ol_detail = static_cast<const MD_BLOCK_OL_DETAIL *>(detail);
        block_stack_.append(OrderedListBlock{bool(ol_detail->is_tight), int(ol_detail->start)});
        break;
      }
      case MD_BLOCK_LI: {
        block_stack_.append(ListItemBlock{});
        break;
      }
      case MD_BLOCK_H: {
        const auto *h_detail = static_cast<const MD_BLOCK_H_DETAIL *>(detail);
        block_stack_.append(HeaderBlock{std::clamp<int>(h_detail->level, 1, 6)});
        break;
      }
      case MD_BLOCK_CODE: {
        block_stack_.append(CodeBlock{});
        break;
      }
      case MD_BLOCK_P: {
        block_stack_.append(ParagraphBlock{});
        break;
      }
      case MD_BLOCK_DOC: {
        block_stack_.append(DocBlock{});
        break;
      }
      case MD_BLOCK_HR:
      case MD_BLOCK_HTML:
      case MD_BLOCK_TABLE:
      case MD_BLOCK_THEAD:
      case MD_BLOCK_TBODY:
      case MD_BLOCK_TR:
      case MD_BLOCK_TH:
      case MD_BLOCK_TD: {
        /* Not implemented. */
        block_stack_.append(UnknownBlock{});
        break;
      }
    }
    const float margin = this->get_space_above_new_block();
    y_ -= margin;
    x_ = this->context_block_start_x();
  }

  float get_space_above_new_block() const
  {
    if (!last_left_block_) {
      return 0.0f;
    }
    const MdBlock &new_block = block_stack_.last();
    // const MdBlock &last_block = *last_left_block_;

    if (const auto *md_header = std::get_if<HeaderBlock>(&new_block)) {
      const MdHeaderStyle &style = header_styles[md_header->level - 1];
      return main_font_.default_line_height * style.size_factor;
    }
    if (std::holds_alternative<ParagraphBlock>(new_block)) {
      return main_font_.default_line_height * 1.5f;
    }
    return main_font_.default_line_height * 1.5f;
  }

  void md_leave_block(const MD_BLOCKTYPE block_type, void * /*detail*/)
  {
    const MdBlock &md_block = block_stack_.last();
    switch (block_type) {
      case MD_BLOCK_QUOTE: {
        BLI_assert(std::holds_alternative<QuoteBlock>(md_block));
        break;
      }
      case MD_BLOCK_UL: {
        BLI_assert(std::holds_alternative<UnorderedListBlock>(md_block));
        break;
      }
      case MD_BLOCK_OL: {
        BLI_assert(std::holds_alternative<OrderedListBlock>(md_block));
        break;
      }
      case MD_BLOCK_LI: {
        BLI_assert(std::holds_alternative<ListItemBlock>(md_block));
        break;
      }
      case MD_BLOCK_H: {
        BLI_assert(std::holds_alternative<HeaderBlock>(md_block));
        break;
      }
      case MD_BLOCK_CODE: {
        BLI_assert(std::holds_alternative<CodeBlock>(md_block));
        break;
      }
      case MD_BLOCK_P: {
        BLI_assert(std::holds_alternative<ParagraphBlock>(md_block));
        break;
      }
      case MD_BLOCK_DOC: {
        BLI_assert(std::holds_alternative<DocBlock>(md_block));
        break;
      }
      case MD_BLOCK_HR:
      case MD_BLOCK_HTML:
      case MD_BLOCK_TABLE:
      case MD_BLOCK_THEAD:
      case MD_BLOCK_TBODY:
      case MD_BLOCK_TR:
      case MD_BLOCK_TH:
      case MD_BLOCK_TD: {
        BLI_assert(std::holds_alternative<UnknownBlock>(md_block));
        break;
      }
    }
    last_left_block_ = block_stack_.pop_last();
  }

  void md_enter_span(const MD_SPANTYPE span_type, void *detail)
  {
    switch (span_type) {
      case MD_SPAN_EM: {
        span_stack_.append(EmSpan{});
        break;
      }
      case MD_SPAN_STRONG: {
        span_stack_.append(StrongSpan{});
        break;
      }
      case MD_SPAN_A: {
        const auto *a_detail = static_cast<const MD_SPAN_A_DETAIL *>(detail);
        span_stack_.append(LinkSpan{StringRef(a_detail->href.text, a_detail->href.size)});
        break;
      }
      case MD_SPAN_CODE: {
        span_stack_.append(CodeSpan{});
        break;
      }
      case MD_SPAN_IMG:
      case MD_SPAN_DEL:
      case MD_SPAN_LATEXMATH:
      case MD_SPAN_LATEXMATH_DISPLAY:
      case MD_SPAN_WIKILINK:
      case MD_SPAN_U: {
        /* Not implemented. */
        span_stack_.append(UnknownSpan{});
        break;
      }
    }
  }

  void md_leave_span(const MD_SPANTYPE span_type, void * /*detail*/)
  {
    const MdSpan &md_span = span_stack_.last();
    switch (span_type) {
      case MD_SPAN_EM: {
        BLI_assert(std::holds_alternative<EmSpan>(md_span));
        break;
      }
      case MD_SPAN_STRONG: {
        BLI_assert(std::holds_alternative<StrongSpan>(md_span));
        break;
      }
      case MD_SPAN_A: {
        BLI_assert(std::holds_alternative<LinkSpan>(md_span));
        break;
      }
      case MD_SPAN_CODE: {
        BLI_assert(std::holds_alternative<CodeSpan>(md_span));
        break;
      }
      case MD_SPAN_IMG:
      case MD_SPAN_DEL:
      case MD_SPAN_LATEXMATH:
      case MD_SPAN_LATEXMATH_DISPLAY:
      case MD_SPAN_WIKILINK:
      case MD_SPAN_U: {
        BLI_assert(std::holds_alternative<UnknownSpan>(md_span));
        break;
      }
    }
    span_stack_.pop_last();
  }

  void md_text(const MD_TEXTTYPE text_type, const StringRef text)
  {
    switch (text_type) {
      case MD_TEXT_NORMAL: {
        this->md_text_normal(text);
        break;
      }
      case MD_TEXT_CODE: {
        this->md_text_code(text);
        break;
      }
      case MD_TEXT_BR:
      case MD_TEXT_SOFTBR: {
        x_ = this->context_block_start_x();
        y_ -= main_font_.default_line_height;
        break;
      }
      case MD_TEXT_NULLCHAR:
      case MD_TEXT_ENTITY:
      case MD_TEXT_HTML:
      case MD_TEXT_LATEXMATH: {
        /* Not implemented. */
        break;
      }
    }
  }

  void md_text_normal(const StringRef text)
  {
    const ContextFontSettings settings = this->get_context_font_settings();
    const float start_x = this->context_block_start_x();
    this->output_wrapped_text(text,
                              settings.size,
                              settings.fontid,
                              settings.weight,
                              settings.italic,
                              settings.url,
                              start_x);
  }

  void md_text_code(StringRef text)
  {
    const ContextFontSettings settings = this->get_context_font_settings();
    const float start_x = this->context_block_start_x();
    BLF_size(settings.fontid, settings.size);
    const float line_height = BLF_height_max(settings.fontid);
    while (!text.is_empty()) {
      /* Literal newlines should be taken into account in code. */
      if (text[0] == '\n') {
        text = text.drop_prefix(1);
        x_ = start_x;
        y_ -= line_height;
        continue;
      }
      const int64_t newline = text.find_first_of('\n');
      if (newline == StringRef::not_found) {
        this->output_wrapped_text(text,
                                  settings.size,
                                  settings.fontid,
                                  settings.weight,
                                  settings.italic,
                                  settings.url,
                                  start_x);
        break;
      }
      this->output_wrapped_text(text.substr(0, newline),
                                settings.size,
                                settings.fontid,
                                settings.weight,
                                settings.italic,
                                settings.url,
                                start_x);
      text = text.drop_prefix(newline);
    }
  }

  struct ContextFontSettings {
    int fontid;
    float weight;
    bool italic;
    float size;
    std::optional<StringRef> url;
  };

  ContextFontSettings get_context_font_settings() const
  {
    ContextFontSettings settings;
    settings.fontid = main_font_.fontid;
    settings.weight = main_font_.default_weight;
    settings.size = main_font_.default_size;
    settings.italic = false;
    for (const auto &md_block : block_stack_) {
      if (const auto *header_block = std::get_if<HeaderBlock>(&md_block)) {
        const MdHeaderStyle &style = header_styles[header_block->level - 1];
        settings.fontid = main_font_.fontid;
        settings.size = main_font_.default_size * style.size_factor;
        settings.weight = style.weight;
        settings.italic = style.italic;
      }
      else if (std::holds_alternative<CodeBlock>(md_block)) {
        settings.fontid = mono_font_.fontid;
        settings.size = mono_font_.default_size;
        settings.weight = mono_font_.default_weight;
        settings.italic = false;
      }
    }
    for (const auto &md_span : span_stack_) {
      if (std::holds_alternative<StrongSpan>(md_span)) {
        settings.weight = main_font_.bold_weight;
      }
      if (std::holds_alternative<EmSpan>(md_span)) {
        settings.italic = true;
      }
      if (const auto *link_span = std::get_if<LinkSpan>(&md_span)) {
        settings.url = link_span->url;
      }
    }
    return settings;
  }

  void output_wrapped_text(StringRef text,
                           const float size,
                           const int fontid,
                           const int weight,
                           const bool italic,
                           const std::optional<StringRef> &url,
                           const float start_x)
  {
    MarkdownItemText item_template;
    item_template.size = size;
    item_template.fontid = fontid;
    item_template.weight = weight;
    item_template.italic = italic;
    item_template.url = url;

    markdown_apply_item_font(item_template, cache_.key.aspect);
    BLI_SCOPED_DEFER([&]() { markdown_disable_item_font(item_template); });

    const int line_height = BLF_height_max(fontid);

    while (!text.is_empty()) {
      const float remaining_width = cache_.key.wrap_width - x_;
      float needed_width = 0.0f;
      const int64_t last_fit_char_i = int64_t(BLF_width_to_strlen(
          fontid, text.data(), size_t(text.size()), remaining_width, &needed_width));

      /* Number of bytes that should be drawn in the current line. */
      int64_t draw_bytes = last_fit_char_i;
      /* Number of bytes that should be skipped (this skips e.g. whitespace at the line end).*/
      int64_t drop_bytes = last_fit_char_i;
      const bool wrap_to_next_line = last_fit_char_i < text.size();

      if (wrap_to_next_line) {
        const int64_t last_space = (last_fit_char_i > 0) ?
                                       text.find_last_of(' ', int64_t(last_fit_char_i) - 1) :
                                       StringRef::not_found;
        if (last_space != StringRef::not_found) {
          draw_bytes = last_space;
          /* Skip the space. */
          drop_bytes = last_space + 1;
          needed_width = BLF_width(fontid, text.data(), draw_bytes);
        }
        else if (x_ > start_x) {
          /* Line has already started and the current word does not fit on it; retry on the next
           * line. */
          y_ -= line_height;
          x_ = start_x;
          continue;
        }
        else if (last_fit_char_i == 0) {
          /* Nothing fits on an empty line; advance one character to avoid an infinit loop. */
          draw_bytes = BLI_str_utf8_size_safe(text.data());
          drop_bytes = draw_bytes;
          needed_width = BLF_width(fontid, text.data(), draw_bytes);
        }
      }

      const StringRef fitting_text = text.substr(0, draw_bytes);
      text = text.drop_prefix(drop_bytes);

      if (draw_bytes > 0) {
        item_template.text = fitting_text;
        item_template.x = x_;
        item_template.y = y_;
        cache_.md_layout.items.append(item_template);
      }
      if (wrap_to_next_line) {
        y_ -= line_height;
        x_ = start_x;
      }
      else {
        x_ += needed_width;
      }
    }
  }

  float context_block_start_x() const
  {
    float indentation = 0.0f;
    for (const auto &md_block : block_stack_) {
      if (std::holds_alternative<QuoteBlock>(md_block)) {
        indentation += UI_UNIT_X;
      }
      if (std::holds_alternative<ListItemBlock>(md_block)) {
        indentation += UI_UNIT_X;
      }
    }
    return indentation;
  }

  void finalize_vertical_positions()
  {
    const float aspect = cache_.key.aspect;
    constexpr float first_line_y = 0.0f;
    float max_ascender = -FLT_MAX;
    for (const MarkdownItem &item : cache_.md_layout.items) {
      const auto *text_item = std::get_if<MarkdownItemText>(&item);
      if (!text_item) {
        continue;
      }
      if (text_item->y != first_line_y) {
        break;
      }
      markdown_apply_item_font(*text_item, aspect);
      const float ascender = BLF_ascender(text_item->fontid);
      max_ascender = std::max(max_ascender, ascender * aspect);
      markdown_disable_item_font(*text_item);
    }
    if (max_ascender != -FLT_MAX) {
      for (MarkdownItem &item : cache_.md_layout.items) {
        std::visit([&](auto &item) { item.y -= max_ascender; }, item);
      }
    }
    float bottom_y = FLT_MAX;
    for (const MarkdownItem &item : cache_.md_layout.items) {
      if (const auto *text_item = std::get_if<MarkdownItemText>(&item)) {
        markdown_apply_item_font(*text_item, aspect);
        bottom_y = std::min(bottom_y, text_item->y + BLF_descender(text_item->fontid) * aspect);
        markdown_disable_item_font(*text_item);
      }
      else if (const auto *bullet_item = std::get_if<MarkdownItemBullet>(&item)) {
        const int fontid = main_font_.fontid;
        BLF_aspect(main_font_.fontid, aspect, aspect, 1.0f);
        BLF_size(fontid, main_font_.default_size);
        BLF_character_weight(fontid, main_font_.default_weight);
        bottom_y = std::min(bottom_y, bullet_item->y + BLF_descender(fontid) * aspect);
      }
    }
    if (bottom_y != FLT_MAX) {
      cache_.md_layout.content_height = -bottom_y;
    }
    else {
      cache_.md_layout.content_height = UI_UNIT_Y;
    }
  }
};

// class MarkdownLayouter {
//  private:
//   struct ListState {
//     bool is_ordered = false;
//     bool is_tight = false;
//     int next_number = 1;
//     float marker_x = 0.0f;
//     float content_x = 0.0f;
//   };

//   const float aspect_;
//   const StringRef full_text_;
//   const float width_;
//   int main_fontid_;
//   int mono_fontid_;
//   float x_;
//   float y_;
//   /** Left edge for the current line (list content indent, or left edge). */
//   float line_indent_x_;

//   float default_size_;
//   float current_size_;
//   std::optional<StringRef> href_;
//   bool is_bold_ = false;
//   bool is_italic_ = false;
//   bool is_code_ = false;
//   Vector<ListState> list_stack_;
//   MarkdownLayoutCache &cache_;

//  public:
//   MarkdownLayouter(const float aspect,
//                    const StringRef full_text,
//                    const float width,
//                    MarkdownLayoutCache &cache)
//       : aspect_(aspect), full_text_(full_text), width_(width), cache_(cache)
//   {
//     main_fontid_ = style_get()->widget.uifont_id;
//     mono_fontid_ = blf_mono_font;
//     x_ = 0.0f;
//     line_indent_x_ = 0.0f;
//     /* Baseline starts at 0; finalized once the first line's ascenders are known. */
//     y_ = 0.0f;
//     default_size_ = style_get()->widget.points * UI_SCALE_FAC / aspect_;
//     current_size_ = default_size_;
//   }

//   void compute_layout()
//   {
//     MD_PARSER md_parser{};
//     md_parser.enter_block = [](MD_BLOCKTYPE block_type, void *detail, void *userdata) {
//       static_cast<MarkdownLayouter *>(userdata)->md_enter_block(block_type, detail);
//       return 0;
//     };
//     md_parser.leave_block = [](MD_BLOCKTYPE block_type, void *detail, void *userdata) {
//       static_cast<MarkdownLayouter *>(userdata)->md_leave_block(block_type, detail);
//       return 0;
//     };
//     md_parser.enter_span = [](MD_SPANTYPE span_type, void *detail, void *userdata) {
//       static_cast<MarkdownLayouter *>(userdata)->md_enter_span(span_type, detail);
//       return 0;
//     };
//     md_parser.leave_span = [](MD_SPANTYPE span_type, void *detail, void *userdata) {
//       static_cast<MarkdownLayouter *>(userdata)->md_leave_span(span_type, detail);
//       return 0;
//     };
//     md_parser.text = [](MD_TEXTTYPE text_type, const char *text, uint32_t length, void
//     *userdata) {
//       static_cast<MarkdownLayouter *>(userdata)->md_text(text_type, StringRef(text, length));
//       return 0;
//     };

//     BLF_enable(main_fontid_, BLF_ASPECT);
//     BLF_aspect(main_fontid_, aspect_, aspect_, 1.0f);
//     BLF_enable(mono_fontid_, BLF_ASPECT);
//     BLF_aspect(mono_fontid_, aspect_, aspect_, 1.0f);

//     md_parse(full_text_.data(), full_text_.size(), &md_parser, this);
//     this->finalize_vertical_positions();

//     BLF_disable(main_fontid_, BLF_ASPECT | BLF_ITALIC);
//     BLF_disable(mono_fontid_, BLF_ASPECT | BLF_ITALIC);
//   }

//   void finalize_vertical_positions()
//   {
//     if (cache_.spans.is_empty()) {
//       cache_.content_height = UI_UNIT_Y;
//       return;
//     }

//     /* First line has the largest baseline y (layout starts at y = 0 and moves downward). */
//     float first_line_y = cache_.spans.first().y;
//     for (const MarkdownSpan &span : cache_.spans) {
//       first_line_y = math::max(first_line_y, span.y);
//     }

//     float max_ascender = 0.0f;
//     for (const MarkdownSpan &span : cache_.spans) {
//       if (span.y != first_line_y) {
//         continue;
//       }
//       this->apply_span_font_style(span);
//       max_ascender = math::max(max_ascender, BLF_ascender(span.fontid) * aspect_);
//     }

//     /* Place the first line so its tallest glyph top sits on y = 0. */
//     const float y_offset = -max_ascender;
//     float min_bottom = 0.0f;
//     for (MarkdownSpan &span : cache_.spans) {
//       span.y += y_offset;
//       this->apply_span_font_style(span);
//       const float descender = BLF_descender(span.fontid) * aspect_;
//       min_bottom = math::min(min_bottom, span.y + descender);
//     }
//     cache_.content_height = math::max(-min_bottom, float(UI_UNIT_Y));
//   }

//   void md_enter_block(const MD_BLOCKTYPE block_type, void *detail)
//   {
//     switch (block_type) {
//       case MD_BLOCK_P: {
//         break;
//       }
//       case MD_BLOCK_H: {
//         const MD_BLOCK_H_DETAIL &detail_h = *static_cast<const MD_BLOCK_H_DETAIL *>(detail);
//         current_size_ = default_size_ * (1.0f / detail_h.level + 1.0f);
//         break;
//       }
//       case MD_BLOCK_CODE: {
//         is_code_ = true;
//         break;
//       }
//       case MD_BLOCK_UL: {
//         const MD_BLOCK_UL_DETAIL &detail_ul = *static_cast<const MD_BLOCK_UL_DETAIL *>(detail);
//         this->enter_list(false, detail_ul.is_tight != 0, 1);
//         break;
//       }
//       case MD_BLOCK_OL: {
//         const MD_BLOCK_OL_DETAIL &detail_ol = *static_cast<const MD_BLOCK_OL_DETAIL *>(detail);
//         this->enter_list(true, detail_ol.is_tight != 0, detail_ol.start);
//         break;
//       }
//       case MD_BLOCK_LI: {
//         this->enter_list_item();
//         break;
//       }
//       default: {
//         break;
//       }
//     }
//   }

//   void md_leave_block(const MD_BLOCKTYPE block_type, void * /*detail*/)
//   {
//     switch (block_type) {
//       case MD_BLOCK_P: {
//         y_ -= UI_UNIT_Y * 1.5f;
//         x_ = line_indent_x_;
//         break;
//       }
//       case MD_BLOCK_H: {
//         y_ -= UI_UNIT_Y * 1.3f;
//         current_size_ = default_size_;
//         x_ = line_indent_x_;
//         break;
//       }
//       case MD_BLOCK_CODE: {
//         is_code_ = false;
//         y_ -= UI_UNIT_Y * 1.5f;
//         x_ = line_indent_x_;
//         break;
//       }
//       case MD_BLOCK_UL:
//       case MD_BLOCK_OL: {
//         this->leave_list();
//         break;
//       }
//       case MD_BLOCK_LI: {
//         this->leave_list_item();
//         break;
//       }
//       default: {
//         break;
//       }
//     }
//   }

//   void md_enter_span(const MD_SPANTYPE span_type, void *detail)
//   {
//     switch (span_type) {
//       case MD_SPAN_A: {
//         const MD_SPAN_A_DETAIL &detail_a = *static_cast<const MD_SPAN_A_DETAIL *>(detail);
//         href_ = StringRef(detail_a.href.text, detail_a.href.size);
//         break;
//       }
//       case MD_SPAN_EM: {
//         is_italic_ = true;
//         break;
//       }
//       case MD_SPAN_STRONG: {
//         is_bold_ = true;
//         break;
//       }
//       case MD_SPAN_CODE: {
//         is_code_ = true;
//         break;
//       }
//       default: {
//         break;
//       }
//     }
//   }

//   void md_leave_span(const MD_SPANTYPE span_type, void * /*detail*/)
//   {
//     switch (span_type) {
//       case MD_SPAN_A: {
//         href_.reset();
//         break;
//       }
//       case MD_SPAN_EM: {
//         is_italic_ = false;
//         break;
//       }
//       case MD_SPAN_STRONG: {
//         is_bold_ = false;
//         break;
//       }
//       case MD_SPAN_CODE: {
//         is_code_ = false;
//         break;
//       }
//       default: {
//         break;
//       }
//     }
//   }

//   void md_text(const MD_TEXTTYPE text_type, const StringRef text)
//   {
//     switch (text_type) {
//       case MD_TEXT_NORMAL: {
//         this->append_wrapped_text(text);
//         break;
//       }
//       case MD_TEXT_CODE: {
//         StringRef remaining_text = text;
//         while (!remaining_text.is_empty()) {
//           /* Code blocks include literal newlines in the text. */
//           if (remaining_text[0] == '\n') {
//             remaining_text = remaining_text.drop_prefix(1);
//             this->advance_to_next_line();
//             continue;
//           }

//           const int64_t newline = remaining_text.find_first_of('\n');
//           if (newline == StringRef::not_found) {
//             this->append_wrapped_text(remaining_text);
//             break;
//           }

//           this->append_wrapped_text(remaining_text.substr(0, newline));
//           remaining_text = remaining_text.drop_prefix(newline);
//         }
//         break;
//       }
//       case MD_TEXT_SOFTBR: {
//         this->advance_to_next_line();
//         break;
//       }
//       default: {
//         break;
//       }
//     }
//   }

//   void enter_list(const bool is_ordered, const bool is_tight, const int start_number)
//   {
//     if (x_ > line_indent_x_) {
//       y_ -= UI_UNIT_Y * 0.8f;
//       x_ = line_indent_x_;
//     }
//     ListState state;
//     state.is_ordered = is_ordered;
//     state.is_tight = is_tight;
//     state.next_number = start_number;
//     state.marker_x = line_indent_x_;
//     list_stack_.append(state);
//   }

//   void leave_list()
//   {
//     list_stack_.pop_last();
//     if (list_stack_.is_empty()) {
//       line_indent_x_ = 0.0f;
//     }
//     else {
//       line_indent_x_ = list_stack_.last().content_x;
//     }
//     x_ = line_indent_x_;
//   }

//   void enter_list_item()
//   {
//     ListState &list = list_stack_.last();
//     if (x_ > list.marker_x) {
//       y_ -= UI_UNIT_Y * 0.8f;
//     }
//     x_ = list.marker_x;
//     this->append_list_marker(list);
//     list.content_x = x_;
//     line_indent_x_ = list.content_x;
//   }

//   void leave_list_item()
//   {
//     const ListState &list = list_stack_.last();
//     if (list.is_tight) {
//       y_ -= UI_UNIT_Y * 0.8f;
//     }
//     x_ = list.marker_x;
//     line_indent_x_ = list.marker_x;
//   }

//   void append_list_marker(ListState &list)
//   {
//     MarkdownSpan span;
//     span.x = x_;
//     span.y = y_;
//     span.size = current_size_;
//     span.fontid = main_fontid_;
//     span.weight = this->current_weight();
//     span.italic = false;

//     this->apply_font_style(main_fontid_);

//     std::string marker;
//     if (list.is_ordered) {
//       span.type = MarkdownSpanType::OrderedMarker;
//       span.list_number = list.next_number++;
//       marker = fmt::format("{}. ", span.list_number);
//     }
//     else {
//       span.type = MarkdownSpanType::Bullet;
//       marker = "\u2022 ";
//     }

//     x_ += BLF_width(main_fontid_, marker.c_str(), marker.size());
//     cache_.spans.append(std::move(span));
//   }

//   void append_wrapped_text(StringRef text)
//   {
//     const int fontid = this->current_fontid();
//     this->apply_font_style(fontid);

//     while (!text.is_empty()) {
//       const float remaining_width = width_ - x_;
//       float needed_width = 0.0f;
//       size_t last_fit_char_i = BLF_width_to_strlen(
//           fontid, text.data(), size_t(text.size()), remaining_width, &needed_width);

//       size_t draw_bytes = last_fit_char_i;
//       size_t drop_bytes = last_fit_char_i;
//       const bool wrap_to_next_line = last_fit_char_i < size_t(text.size());

//       if (wrap_to_next_line) {
//         const int64_t last_space = (last_fit_char_i > 0) ?
//                                        text.find_last_of(' ', int64_t(last_fit_char_i) - 1) :
//                                        StringRef::not_found;
//         if (last_space != StringRef::not_found) {
//           draw_bytes = size_t(last_space);
//           drop_bytes = size_t(last_space) + 1; /* Skip the space. */
//           needed_width = BLF_width(fontid, text.data(), draw_bytes);
//         }
//         else if (x_ > line_indent_x_) {
//           /* Word does not fit on this line; retry on the next. */
//           this->advance_to_next_line();
//           continue;
//         }
//         else if (last_fit_char_i == 0) {
//           /* Nothing fits on an empty line; advance one character to avoid an infinite loop. */
//           draw_bytes = drop_bytes = size_t(BLI_str_utf8_size_safe(text.data()));
//           needed_width = BLF_width(fontid, text.data(), draw_bytes);
//         }
//       }

//       const StringRef fitting_text = text.substr(0, draw_bytes);
//       text = text.drop_prefix(drop_bytes);

//       if (draw_bytes > 0) {
//         this->append_span(fitting_text);
//       }

//       if (wrap_to_next_line) {
//         this->advance_to_next_line();
//       }
//       else {
//         x_ += needed_width;
//       }
//     }
//   }

//   void append_span(const StringRef text)
//   {
//     MarkdownSpan span;
//     span.x = x_;
//     span.y = y_;
//     span.size = current_size_;
//     span.fontid = this->current_fontid();
//     span.weight = this->current_weight();
//     span.italic = is_italic_;
//     span.text = text;
//     if (href_) {
//       span.url = *href_;
//     }
//     cache_.spans.append(std::move(span));
//   }

//   void apply_span_font_style(const MarkdownSpan &span) const
//   {
//     BLF_character_weight(span.fontid, span.weight);
//     BLF_size(span.fontid, span.size);
//     if (span.italic) {
//       BLF_enable(span.fontid, BLF_ITALIC);
//     }
//     else {
//       BLF_disable(span.fontid, BLF_ITALIC);
//     }
//   }

//   void apply_font_style(const int fontid) const
//   {
//     BLF_character_weight(fontid, this->current_weight());
//     BLF_size(fontid, current_size_);
//     if (is_italic_) {
//       BLF_enable(fontid, BLF_ITALIC);
//     }
//     else {
//       BLF_disable(fontid, BLF_ITALIC);
//     }
//   }

//   int current_weight() const
//   {
//     const int fontid = this->current_fontid();
//     return is_bold_ ? 800 : BLF_default_weight(fontid);
//   }

//   int current_fontid() const
//   {
//     return is_code_ ? mono_fontid_ : main_fontid_;
//   }

//   void advance_to_next_line()
//   {
//     x_ = line_indent_x_;
//     y_ -= UI_UNIT_Y * 0.8f;
//   }
// };

static void create_link_buttons_for_label(ButtonLabel *button)
{
  if (!button->markdown_cache) {
    return;
  }
  const MarkdownLayoutCache &cache = *button->markdown_cache;
  if (cache.md_layout.items.is_empty()) {
    return;
  }

  Block *block = button->block;
  const float aspect = block->aspect;
  wmOperatorType *ot = WM_operatortype_find("WM_OT_url_open", false);
  if (!ot) {
    return;
  }

  for (const MarkdownItem &item : cache.md_layout.items) {
    const auto *text_item_ptr = std::get_if<MarkdownItemText>(&item);
    if (!text_item_ptr->url) {
      continue;
    }
    const MarkdownItemText &text_item = *text_item_ptr;

    markdown_apply_item_font(text_item, aspect);

    const StringRef text = text_item.text;
    const float descender = BLF_descender(text_item.fontid) * aspect;
    const float ascender = BLF_ascender(text_item.fontid) * aspect;
    const float font_height = ascender - descender;
    const float width = BLF_width(text_item.fontid, text.data(), text.size());

    const float x = button->rect.xmin + text_item.x;
    const float y = button->rect.ymax + text_item.y + descender;

    const EmbossType previous_emboss = block_emboss_get(block);
    block_emboss_set(block, EmbossType::None);
    Button *but = uiDefIconBut(block,
                               ButtonType::But,
                               ICON_NONE,
                               x,
                               y,
                               width,
                               font_height,
                               nullptr,
                               0.0,
                               0.0,
                               std::nullopt);
    block_emboss_set(block, previous_emboss);

    static_cast<ButtonPush *>(but)->draw_as_link = true;

    PointerRNA props = WM_operator_properties_create_ptr(ot);
    RNA_string_set(&props, "url", std::string(*text_item.url).c_str());
    button_operator_set(but, ot, wm::OpCallContext::InvokeDefault, &props);

    /* Invisible hit-target; markdown draw paints the link text. Show URL on hover. */
    button_func_tooltip_custom_set(
        but,
        [](bContext & /*C*/, TooltipData &data, Button *tip_but, void * /*argN*/) {
          tooltip_text_field_add(data,
                                 RNA_string_get(tip_but->opptr, "url"),
                                 {},
                                 TIP_STYLE_NORMAL,
                                 TIP_LC_NORMAL,
                                 false);
        },
        nullptr,
        nullptr);

    markdown_disable_item_font(text_item);
  }
}

void label_markdown_create_link_buttons(Block *block)
{
  Vector<ButtonLabel *> markdown_labels;
  for (Button &but : block->buttons()) {
    if (button_label_is_markdown(&but)) {
      markdown_labels.append(static_cast<ButtonLabel *>(&but));
    }
  }
  for (ButtonLabel *label : markdown_labels) {
    create_link_buttons_for_label(label);
  }
}

void label_markdown_resolve(ButtonLabel *button)
{
  const int wrap_width = std::max<int>(std::ceil(BLI_rctf_size_x(&button->rect)), 0);
  const float aspect = button->block->aspect;
  {
    MarkdownLayoutCacheKey cache_lookup_key;
    cache_lookup_key.text = button->str;
    cache_lookup_key.wrap_width = wrap_width;
    cache_lookup_key.aspect = aspect;

    /* Try to reuse an already attached cache. */
    if (button->markdown_cache) {
      const MarkdownLayoutCache &cache = *button->markdown_cache;
      if (cache.key != cache_lookup_key) {
        button->markdown_cache.reset();
      }
    }
    /* Try to take a matching cache from the previous block. */
    if (!button->markdown_cache && button->block->oldblock) {
      int i = 0;
      for (std::shared_ptr<MarkdownLayoutCache> &cache_ptr :
           button->block->oldblock->markdown_layout_cache)
      {
        MarkdownLayoutCache &cache = *cache_ptr;
        if (cache.key == cache_lookup_key) {
          button->markdown_cache = cache_ptr;
          break;
        }
        i++;
      }
      if (button->markdown_cache) {
        button->block->oldblock->markdown_layout_cache.remove(i);
      }
    }
    if (button->markdown_cache) {
      button->block->markdown_layout_cache.append(button->markdown_cache);
      button->rect.ymin = button->rect.ymax - button->markdown_cache->md_layout.content_height;
      return;
    }
  }

  button->markdown_cache = std::make_shared<MarkdownLayoutCache>();
  MarkdownLayoutCache &cache = *button->markdown_cache;
  cache.key.aspect = aspect;
  cache.key.wrap_width = wrap_width;
  /* Copy the string to the cache so that references into it can be preserved. */
  cache.key.text = cache.scope.allocator().copy_string(button->str);
  button->block->markdown_layout_cache.append(button->markdown_cache);

  MarkdownLayouter layouter(cache);
  layouter.compute_layout();

  button->rect.ymin = button->rect.ymax - cache.md_layout.content_height;
}

void label_markdown_draw(const ButtonLabel *button, const uchar color[4], const rcti *rect)
{
  if (!button->markdown_cache || button->markdown_cache->md_layout.items.is_empty()) {
    return;
  }

  const float aspect = button->block->aspect;
  const MarkdownLayoutCache &cache = *button->markdown_cache;

  uchar link_color[4];
  theme::get_color_4ubv(TH_LINK, link_color);
  link_color[3] = color[3];

  for (const MarkdownItem &item : cache.md_layout.items) {
    if (const auto *text_item = std::get_if<MarkdownItemText>(&item)) {
      markdown_apply_item_font(*text_item, cache.key.aspect);
      if (text_item->url) {
        BLF_color4ubv(text_item->fontid, link_color);
      }
      else {
        BLF_color4ubv(text_item->fontid, color);
      }
      const float x = rect->xmin + text_item->x / aspect;
      const float y = rect->ymax + text_item->y / aspect;
      BLF_position(text_item->fontid, x, y, 0.0f);
      BLF_draw(text_item->fontid, text_item->text.data(), text_item->text.size());
      markdown_disable_item_font(*text_item);
    }
    else if (const auto *bullet_item = std::get_if<MarkdownItemBullet>(&item)) {
      // TODO
    }
  }
}

}  // namespace blender::ui
