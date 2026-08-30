/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <cfloat>
#include <md4c.h>

#include "BLF_api.hh"

#include "BLI_math_base.hh"
#include "BLI_rect.hh"
#include "BLI_string_utf8.hh"

#include "GPU_immediate.hh"
#include "GPU_state.hh"

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

static void markdown_apply_font(const int fontid,
                                const float size_px,
                                const int weight,
                                const bool italic)
{
  BLF_disable(fontid, BLF_ASPECT);
  BLF_size(fontid, size_px);
  BLF_character_weight(fontid, weight);
  if (italic) {
    BLF_enable(fontid, BLF_ITALIC);
  }
  else {
    BLF_disable(fontid, BLF_ITALIC);
  }
}

static void markdown_apply_item_font(const MarkdownItemText &item)
{
  markdown_apply_font(item.fontid, item.size_px, item.weight, item.italic);
}

static void markdown_disable_item_font(const MarkdownItemText &item)
{
  BLF_disable(item.fontid, BLF_ITALIC);
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
    int default_weight;
    int bold_weight;
    /** Line height in pixels. */
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
    char mark_delimiter = '.';
  };

  struct ListItemBlock {
    /** Set for ordered list items; unset for unordered. */
    std::optional<int> number;
  };

  struct ParagraphBlock {};

  struct CodeBlock {};

  struct QuoteBlock {};

  struct HorizontalRuleBlock {};

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
                               HorizontalRuleBlock,
                               UnknownBlock>;
  using MdSpan = std::variant<EmSpan, StrongSpan, LinkSpan, CodeSpan, UnknownSpan>;

  Vector<MdBlock, 16> block_stack_;
  Vector<MdSpan, 16> span_stack_;
  std::optional<MdBlock> last_left_block_;

 public:
  MarkdownLayouter(MarkdownLayoutCache &cache_) : cache_(cache_)
  {
    const uiFontStyle &widget_style = style_get()->widget;
    main_font_ = this->make_font_config(
        widget_style.uifont_id, widget_style.points, widget_style.character_weight, 700);
    mono_font_ = this->make_font_config(
        blf_mono_font, widget_style.points, widget_style.character_weight, 700);
    x_ = 0.0f;
    y_ = 0.0f;
  }

  FontConfig make_font_config(const int fontid,
                              const float default_size,
                              const int default_weight,
                              const int bold_weight)
  {
    FontConfig config;
    config.fontid = fontid;
    config.default_size = default_size;
    config.default_weight = default_weight;
    config.bold_weight = bold_weight;
    markdown_apply_font(fontid, default_size * cache_.key.pixels_per_point, default_weight, false);
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
        const auto *ul_detail = static_cast<const MD_BLOCK_UL_DETAIL *>(detail);
        block_stack_.append(UnorderedListBlock{bool(ul_detail->is_tight)});
        break;
      }
      case MD_BLOCK_OL: {
        const auto *ol_detail = static_cast<const MD_BLOCK_OL_DETAIL *>(detail);
        block_stack_.append(OrderedListBlock{
            bool(ol_detail->is_tight), int(ol_detail->start), char(ol_detail->mark_delimiter)});
        break;
      }
      case MD_BLOCK_LI: {
        ListItemBlock list_item;
        if (!block_stack_.is_empty()) {
          if (auto *ol = std::get_if<OrderedListBlock>(&block_stack_.last())) {
            list_item.number = ol->next_number++;
          }
        }
        block_stack_.append(list_item);
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
      case MD_BLOCK_HR: {
        block_stack_.append(HorizontalRuleBlock{});
        break;
      }
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
    if (block_type == MD_BLOCK_LI) {
      this->append_list_marker();
    }
    else if (block_type == MD_BLOCK_HR) {
      this->append_horizontal_rule();
    }
  }

  float list_indent_px() const
  {
    return 18.0f * cache_.key.pixels_per_point;
  }

  int get_unordered_list_level() const
  {
    int level = 0;
    for (const auto &md_block : block_stack_) {
      if (std::holds_alternative<UnorderedListBlock>(md_block)) {
        level++;
      }
    }
    return std::max(level - 1, 0);
  }

  static StringRef unordered_bullet_for_level(const int level)
  {
    switch (level % 3) {
      case 1:
        return "◦";
      case 2:
        return "▪";
      default:
        return "•";
    }
  }

  void append_list_marker()
  {
    const auto *list_item = std::get_if<ListItemBlock>(&block_stack_.last());
    BLI_assert(list_item);
    if (!list_item) {
      return;
    }

    MarkdownItemText item;
    item.size_px = main_font_.default_size * cache_.key.pixels_per_point;
    item.fontid = main_font_.fontid;
    item.weight = main_font_.default_weight;
    item.italic = false;
    item.y = y_;

    if (list_item->number) {
      char delimiter = '.';
      if (block_stack_.size() >= 2) {
        if (const auto *ol = std::get_if<OrderedListBlock>(&block_stack_[block_stack_.size() - 2]))
        {
          delimiter = ol->mark_delimiter;
        }
      }
      item.text = cache_.scope.allocator().copy_string(
          fmt::format("{}{}", *list_item->number, delimiter));
    }
    else {
      item.text = unordered_bullet_for_level(this->get_unordered_list_level());
    }

    markdown_apply_item_font(item);
    const float marker_width = BLF_width(item.fontid, item.text.data(), item.text.size());
    markdown_disable_item_font(item);

    if (list_item->number) {
      /* Right-align the number towards the content. */
      item.x = x_ - marker_width - 4.0f * cache_.key.pixels_per_point;
    }
    else {
      /* Center the bullet in the list-item indent gutter. */
      item.x = x_ - this->list_indent_px() * 0.5f - marker_width * 0.5f;
    }
    cache_.md_layout.items.append(item);
  }

  void append_horizontal_rule()
  {
    const float half_space = 0.5f * main_font_.default_line_height;
    y_ -= half_space;
    cache_.md_layout.items.append(MarkdownItemHorizontalRule{y_});
    y_ -= half_space;
  }

  float get_space_above_new_block() const
  {
    if (!last_left_block_) {
      return 0.0f;
    }
    const MdBlock &new_block = block_stack_.last();
    // const MdBlock &last_block = *last_left_block_;

    if (std::holds_alternative<UnorderedListBlock>(new_block) ||
        std::holds_alternative<OrderedListBlock>(new_block))
    {
      /* The spacing for lists is done around the individual list items, not the list itself.*/
      return 0.0f;
    }

    if (const auto *md_header = std::get_if<HeaderBlock>(&new_block)) {
      const MdHeaderStyle &style = header_styles[md_header->level - 1];
      return main_font_.default_line_height * style.size_factor;
    }
    if (std::holds_alternative<ParagraphBlock>(new_block) ||
        std::holds_alternative<HorizontalRuleBlock>(new_block))
    {
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
      case MD_BLOCK_HR: {
        BLI_assert(std::holds_alternative<HorizontalRuleBlock>(md_block));
        break;
      }
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
    markdown_apply_font(settings.fontid,
                        settings.size * cache_.key.pixels_per_point,
                        settings.weight,
                        settings.italic);
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
    int weight;
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
    item_template.size_px = size * cache_.key.pixels_per_point;
    item_template.fontid = fontid;
    item_template.weight = weight;
    item_template.italic = italic;
    item_template.url = url;

    markdown_apply_item_font(item_template);
    BLI_SCOPED_DEFER([&]() { markdown_disable_item_font(item_template); });

    const float line_height = BLF_height_max(fontid);

    while (!text.is_empty()) {
      const float remaining_width = cache_.key.wrap_width_px - x_;
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
    const float indent = this->list_indent_px();
    for (const auto &md_block : block_stack_) {
      if (std::holds_alternative<QuoteBlock>(md_block)) {
        indentation += indent;
      }
      if (std::holds_alternative<ListItemBlock>(md_block)) {
        indentation += indent;
      }
    }
    return indentation;
  }

  void finalize_vertical_positions()
  {
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
      markdown_apply_item_font(*text_item);
      max_ascender = std::max(max_ascender, float(BLF_ascender(text_item->fontid)));
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
        markdown_apply_item_font(*text_item);
        bottom_y = std::min(bottom_y, text_item->y + BLF_descender(text_item->fontid));
        markdown_disable_item_font(*text_item);
      }
      else if (const auto *rule_item = std::get_if<MarkdownItemHorizontalRule>(&item)) {
        bottom_y = std::min(bottom_y, rule_item->y - 0.5f * main_font_.default_line_height);
      }
    }
    if (bottom_y != FLT_MAX) {
      cache_.md_layout.content_height = -bottom_y;
    }
    else {
      cache_.md_layout.content_height = 18.0f * cache_.key.pixels_per_point;
    }
  }
};

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
    if (!text_item_ptr || !text_item_ptr->url) {
      continue;
    }
    const MarkdownItemText &text_item = *text_item_ptr;

    markdown_apply_item_font(text_item);

    const StringRef text = text_item.text;
    const float descender_px = BLF_descender(text_item.fontid);
    const float ascender_px = BLF_ascender(text_item.fontid);
    const float font_height = (ascender_px - descender_px) * aspect;
    const float width = BLF_width(text_item.fontid, text.data(), text.size()) * aspect;

    const float x = button->rect.xmin + text_item.x * aspect;
    const float y = button->rect.ymax + (text_item.y + descender_px) * aspect;

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

static MarkdownLayoutCacheKey markdown_layout_key_for_button(const ButtonLabel &button)
{
  const float aspect = button.block->aspect;
  MarkdownLayoutCacheKey key;
  key.text = button.str;
  key.wrap_width_px = std::max<int>(std::ceil(BLI_rctf_size_x(&button.rect) / aspect), 0);
  key.pixels_per_point = UI_SCALE_FAC / aspect;
  return key;
}

static void markdown_apply_button_height(ButtonLabel &button)
{
  button.rect.ymin = button.rect.ymax -
                     button.markdown_cache->md_layout.content_height * button.block->aspect;
}

void label_markdown_resolve(ButtonLabel *button)
{
  const MarkdownLayoutCacheKey cache_lookup_key = markdown_layout_key_for_button(*button);
  {
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
      markdown_apply_button_height(*button);
      return;
    }
  }

  button->markdown_cache = std::make_shared<MarkdownLayoutCache>();
  MarkdownLayoutCache &cache = *button->markdown_cache;
  cache.key = cache_lookup_key;
  /* Copy the string to the cache so that references into it can be preserved. */
  cache.key.text = cache.scope.allocator().copy_string(cache.key.text);
  button->block->markdown_layout_cache.append(button->markdown_cache);

  MarkdownLayouter layouter(cache);
  layouter.compute_layout();

  markdown_apply_button_height(*button);
}

void label_markdown_draw(const ButtonLabel *button, const uchar color[4], const rcti *rect)
{
  if (!button->markdown_cache || button->markdown_cache->md_layout.items.is_empty()) {
    return;
  }

  const MarkdownLayoutCache &cache = *button->markdown_cache;

  uchar link_color[4];
  theme::get_color_4ubv(TH_LINK, link_color);
  link_color[3] = color[3];

  for (const MarkdownItem &item : cache.md_layout.items) {
    if (const auto *text_item = std::get_if<MarkdownItemText>(&item)) {
      markdown_apply_item_font(*text_item);
      if (text_item->url) {
        BLF_color4ubv(text_item->fontid, link_color);
      }
      else {
        BLF_color4ubv(text_item->fontid, color);
      }
      const float x = rect->xmin + text_item->x;
      const float y = rect->ymax + text_item->y;
      BLF_position(text_item->fontid, x, y, 0.0f);
      BLF_draw(text_item->fontid, text_item->text.data(), text_item->text.size());
      markdown_disable_item_font(*text_item);
    }
    else if (const auto *rule_item = std::get_if<MarkdownItemHorizontalRule>(&item)) {
      const uchar line_color[4] = {color[0], color[1], color[2], 30};
      const float y = rect->ymax + rule_item->y;

      const uint pos = GPU_vertformat_attr_add(
          immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);
      immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
      GPU_blend(GPU_BLEND_ALPHA);
      immUniformColor4ubv(line_color);
      GPU_line_width(1.0f);
      immBegin(GPU_PRIM_LINES, 2);
      immVertex2f(pos, rect->xmin, y);
      immVertex2f(pos, rect->xmax, y);
      immEnd();
      GPU_blend(GPU_BLEND_NONE);
      immUnbindProgram();
    }
  }
}

}  // namespace blender::ui
