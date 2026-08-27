/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

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

static StringRef markdown_span_draw_text(const MarkdownSpan &span, std::string &owned_marker)
{
  switch (span.type) {
    case MarkdownSpanType::Text: {
      return span.text;
    }
    case MarkdownSpanType::Bullet: {
      owned_marker = "\u2022 ";
      return owned_marker;
    }
    case MarkdownSpanType::OrderedMarker: {
      owned_marker = fmt::format("{}. ", span.list_number);
      return owned_marker;
    }
  }
  return {};
}

static void markdown_apply_span_font(const MarkdownSpan &span, const float aspect)
{
  BLF_enable(span.fontid, BLF_ASPECT);
  BLF_aspect(span.fontid, aspect, aspect, 1.0f);
  BLF_size(span.fontid, span.size);
  BLF_character_weight(span.fontid, span.weight);
  if (span.italic) {
    BLF_enable(span.fontid, BLF_ITALIC);
  }
  else {
    BLF_disable(span.fontid, BLF_ITALIC);
  }
}

static void markdown_disable_span_font(const MarkdownSpan &span)
{
  BLF_disable(span.fontid, BLF_ASPECT | BLF_ITALIC);
}

class MarkdownLayouter {
 private:
  struct ListState {
    bool is_ordered = false;
    bool is_tight = false;
    int next_number = 1;
    float marker_x = 0.0f;
    float content_x = 0.0f;
  };

  const float aspect_;
  const StringRef full_text_;
  const float width_;
  int main_fontid_;
  int mono_fontid_;
  float x_;
  float y_;
  /** Left edge for the current line (list content indent, or left edge). */
  float line_indent_x_;

  float default_size_;
  float current_size_;
  std::optional<StringRef> href_;
  bool is_bold_ = false;
  bool is_italic_ = false;
  bool is_code_ = false;
  Vector<ListState> list_stack_;
  MarkdownLayoutCache &cache_;

 public:
  MarkdownLayouter(const float aspect,
                   const StringRef full_text,
                   const float width,
                   MarkdownLayoutCache &cache)
      : aspect_(aspect), full_text_(full_text), width_(width), cache_(cache)
  {
    main_fontid_ = style_get()->widget.uifont_id;
    mono_fontid_ = blf_mono_font;
    x_ = 0.0f;
    line_indent_x_ = 0.0f;
    /* Baseline starts at 0; finalized once the first line's ascenders are known. */
    y_ = 0.0f;
    default_size_ = style_get()->widget.points * UI_SCALE_FAC / aspect_;
    current_size_ = default_size_;
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

    BLF_enable(main_fontid_, BLF_ASPECT);
    BLF_aspect(main_fontid_, aspect_, aspect_, 1.0f);
    BLF_enable(mono_fontid_, BLF_ASPECT);
    BLF_aspect(mono_fontid_, aspect_, aspect_, 1.0f);

    md_parse(full_text_.data(), full_text_.size(), &md_parser, this);
    this->finalize_vertical_positions();

    BLF_disable(main_fontid_, BLF_ASPECT | BLF_ITALIC);
    BLF_disable(mono_fontid_, BLF_ASPECT | BLF_ITALIC);
  }

  void finalize_vertical_positions()
  {
    if (cache_.spans.is_empty()) {
      cache_.content_height = UI_UNIT_Y;
      return;
    }

    /* First line has the largest baseline y (layout starts at y = 0 and moves downward). */
    float first_line_y = cache_.spans.first().y;
    for (const MarkdownSpan &span : cache_.spans) {
      first_line_y = math::max(first_line_y, span.y);
    }

    float max_ascender = 0.0f;
    for (const MarkdownSpan &span : cache_.spans) {
      if (span.y != first_line_y) {
        continue;
      }
      this->apply_span_font_style(span);
      max_ascender = math::max(max_ascender, BLF_ascender(span.fontid) * aspect_);
    }

    /* Place the first line so its tallest glyph top sits on y = 0. */
    const float y_offset = -max_ascender;
    float min_bottom = 0.0f;
    for (MarkdownSpan &span : cache_.spans) {
      span.y += y_offset;
      this->apply_span_font_style(span);
      const float descender = BLF_descender(span.fontid) * aspect_;
      min_bottom = math::min(min_bottom, span.y + descender);
    }
    cache_.content_height = math::max(-min_bottom, float(UI_UNIT_Y));
  }

  void md_enter_block(const MD_BLOCKTYPE block_type, void *detail)
  {
    switch (block_type) {
      case MD_BLOCK_P: {
        break;
      }
      case MD_BLOCK_H: {
        const MD_BLOCK_H_DETAIL &detail_h = *static_cast<const MD_BLOCK_H_DETAIL *>(detail);
        current_size_ = default_size_ * (1.0f / detail_h.level + 1.0f);
        break;
      }
      case MD_BLOCK_CODE: {
        is_code_ = true;
        break;
      }
      case MD_BLOCK_UL: {
        const MD_BLOCK_UL_DETAIL &detail_ul = *static_cast<const MD_BLOCK_UL_DETAIL *>(detail);
        this->enter_list(false, detail_ul.is_tight != 0, 1);
        break;
      }
      case MD_BLOCK_OL: {
        const MD_BLOCK_OL_DETAIL &detail_ol = *static_cast<const MD_BLOCK_OL_DETAIL *>(detail);
        this->enter_list(true, detail_ol.is_tight != 0, detail_ol.start);
        break;
      }
      case MD_BLOCK_LI: {
        this->enter_list_item();
        break;
      }
      default: {
        break;
      }
    }
  }

  void md_leave_block(const MD_BLOCKTYPE block_type, void * /*detail*/)
  {
    switch (block_type) {
      case MD_BLOCK_P: {
        y_ -= UI_UNIT_Y * 1.5f;
        x_ = line_indent_x_;
        break;
      }
      case MD_BLOCK_H: {
        y_ -= UI_UNIT_Y * 1.3f;
        current_size_ = default_size_;
        x_ = line_indent_x_;
        break;
      }
      case MD_BLOCK_CODE: {
        is_code_ = false;
        y_ -= UI_UNIT_Y * 1.5f;
        x_ = line_indent_x_;
        break;
      }
      case MD_BLOCK_UL:
      case MD_BLOCK_OL: {
        this->leave_list();
        break;
      }
      case MD_BLOCK_LI: {
        this->leave_list_item();
        break;
      }
      default: {
        break;
      }
    }
  }

  void md_enter_span(const MD_SPANTYPE span_type, void *detail)
  {
    switch (span_type) {
      case MD_SPAN_A: {
        const MD_SPAN_A_DETAIL &detail_a = *static_cast<const MD_SPAN_A_DETAIL *>(detail);
        href_ = StringRef(detail_a.href.text, detail_a.href.size);
        break;
      }
      case MD_SPAN_EM: {
        is_italic_ = true;
        break;
      }
      case MD_SPAN_STRONG: {
        is_bold_ = true;
        break;
      }
      case MD_SPAN_CODE: {
        is_code_ = true;
        break;
      }
      default: {
        break;
      }
    }
  }

  void md_leave_span(const MD_SPANTYPE span_type, void * /*detail*/)
  {
    switch (span_type) {
      case MD_SPAN_A: {
        href_.reset();
        break;
      }
      case MD_SPAN_EM: {
        is_italic_ = false;
        break;
      }
      case MD_SPAN_STRONG: {
        is_bold_ = false;
        break;
      }
      case MD_SPAN_CODE: {
        is_code_ = false;
        break;
      }
      default: {
        break;
      }
    }
  }

  void md_text(const MD_TEXTTYPE text_type, const StringRef text)
  {
    switch (text_type) {
      case MD_TEXT_NORMAL: {
        this->append_wrapped_text(text);
        break;
      }
      case MD_TEXT_CODE: {
        StringRef remaining_text = text;
        while (!remaining_text.is_empty()) {
          /* Code blocks include literal newlines in the text. */
          if (remaining_text[0] == '\n') {
            remaining_text = remaining_text.drop_prefix(1);
            this->advance_to_next_line();
            continue;
          }

          const int64_t newline = remaining_text.find_first_of('\n');
          if (newline == StringRef::not_found) {
            this->append_wrapped_text(remaining_text);
            break;
          }

          this->append_wrapped_text(remaining_text.substr(0, newline));
          remaining_text = remaining_text.drop_prefix(newline);
        }
        break;
      }
      case MD_TEXT_SOFTBR: {
        this->advance_to_next_line();
        break;
      }
      default: {
        break;
      }
    }
  }

  void enter_list(const bool is_ordered, const bool is_tight, const int start_number)
  {
    if (x_ > line_indent_x_) {
      y_ -= UI_UNIT_Y * 0.8f;
      x_ = line_indent_x_;
    }
    ListState state;
    state.is_ordered = is_ordered;
    state.is_tight = is_tight;
    state.next_number = start_number;
    state.marker_x = line_indent_x_;
    list_stack_.append(state);
  }

  void leave_list()
  {
    list_stack_.pop_last();
    if (list_stack_.is_empty()) {
      line_indent_x_ = 0.0f;
    }
    else {
      line_indent_x_ = list_stack_.last().content_x;
    }
    x_ = line_indent_x_;
  }

  void enter_list_item()
  {
    ListState &list = list_stack_.last();
    if (x_ > list.marker_x) {
      y_ -= UI_UNIT_Y * 0.8f;
    }
    x_ = list.marker_x;
    this->append_list_marker(list);
    list.content_x = x_;
    line_indent_x_ = list.content_x;
  }

  void leave_list_item()
  {
    const ListState &list = list_stack_.last();
    if (list.is_tight) {
      y_ -= UI_UNIT_Y * 0.8f;
    }
    x_ = list.marker_x;
    line_indent_x_ = list.marker_x;
  }

  void append_list_marker(ListState &list)
  {
    MarkdownSpan span;
    span.x = x_;
    span.y = y_;
    span.size = current_size_;
    span.fontid = main_fontid_;
    span.weight = this->current_weight();
    span.italic = false;

    this->apply_font_style(main_fontid_);

    std::string marker;
    if (list.is_ordered) {
      span.type = MarkdownSpanType::OrderedMarker;
      span.list_number = list.next_number++;
      marker = fmt::format("{}. ", span.list_number);
    }
    else {
      span.type = MarkdownSpanType::Bullet;
      marker = "\u2022 ";
    }

    x_ += BLF_width(main_fontid_, marker.c_str(), marker.size());
    cache_.spans.append(std::move(span));
  }

  void append_wrapped_text(StringRef text)
  {
    const int fontid = this->current_fontid();
    this->apply_font_style(fontid);

    while (!text.is_empty()) {
      const float remaining_width = width_ - x_;
      float needed_width = 0.0f;
      size_t last_fit_char_i = BLF_width_to_strlen(
          fontid, text.data(), size_t(text.size()), remaining_width, &needed_width);

      size_t draw_bytes = last_fit_char_i;
      size_t drop_bytes = last_fit_char_i;
      const bool wrap_to_next_line = last_fit_char_i < size_t(text.size());

      if (wrap_to_next_line) {
        const int64_t last_space = (last_fit_char_i > 0) ?
                                       text.find_last_of(' ', int64_t(last_fit_char_i) - 1) :
                                       StringRef::not_found;
        if (last_space != StringRef::not_found) {
          draw_bytes = size_t(last_space);
          drop_bytes = size_t(last_space) + 1; /* Skip the space. */
          needed_width = BLF_width(fontid, text.data(), draw_bytes);
        }
        else if (x_ > line_indent_x_) {
          /* Word does not fit on this line; retry on the next. */
          this->advance_to_next_line();
          continue;
        }
        else if (last_fit_char_i == 0) {
          /* Nothing fits on an empty line; advance one character to avoid an infinite loop. */
          draw_bytes = drop_bytes = size_t(BLI_str_utf8_size_safe(text.data()));
          needed_width = BLF_width(fontid, text.data(), draw_bytes);
        }
      }

      const StringRef fitting_text = text.substr(0, draw_bytes);
      text = text.drop_prefix(drop_bytes);

      if (draw_bytes > 0) {
        this->append_span(fitting_text);
      }

      if (wrap_to_next_line) {
        this->advance_to_next_line();
      }
      else {
        x_ += needed_width;
      }
    }
  }

  void append_span(const StringRef text)
  {
    MarkdownSpan span;
    span.x = x_;
    span.y = y_;
    span.size = current_size_;
    span.fontid = this->current_fontid();
    span.weight = this->current_weight();
    span.italic = is_italic_;
    span.text = text;
    if (href_) {
      span.url = *href_;
    }
    cache_.spans.append(std::move(span));
  }

  void apply_span_font_style(const MarkdownSpan &span) const
  {
    BLF_character_weight(span.fontid, span.weight);
    BLF_size(span.fontid, span.size);
    if (span.italic) {
      BLF_enable(span.fontid, BLF_ITALIC);
    }
    else {
      BLF_disable(span.fontid, BLF_ITALIC);
    }
  }

  void apply_font_style(const int fontid) const
  {
    BLF_character_weight(fontid, this->current_weight());
    BLF_size(fontid, current_size_);
    if (is_italic_) {
      BLF_enable(fontid, BLF_ITALIC);
    }
    else {
      BLF_disable(fontid, BLF_ITALIC);
    }
  }

  int current_weight() const
  {
    const int fontid = this->current_fontid();
    return is_bold_ ? 800 : BLF_default_weight(fontid);
  }

  int current_fontid() const
  {
    return is_code_ ? mono_fontid_ : main_fontid_;
  }

  void advance_to_next_line()
  {
    x_ = line_indent_x_;
    y_ -= UI_UNIT_Y * 0.8f;
  }
};

static bool markdown_link_identity_compare(const Button *a, const Button *b)
{
  if (!a->opptr || !b->opptr) {
    return false;
  }
  if (RNA_string_get(a->opptr, "url") != RNA_string_get(b->opptr, "url")) {
    return false;
  }
  /* Distinguish multiple links to the same URL by position. */
  return a->rect.xmin == b->rect.xmin && a->rect.ymin == b->rect.ymin;
}

static void create_link_buttons_for_label(ButtonLabel *button)
{
  if (!button->markdown_cache) {
    return;
  }
  const MarkdownLayoutCache &cache = *button->markdown_cache;
  if (cache.spans.is_empty()) {
    return;
  }

  Block *block = button->block;
  const float aspect = block->aspect;
  wmOperatorType *ot = WM_operatortype_find("WM_OT_url_open", false);
  if (!ot) {
    return;
  }

  for (const MarkdownSpan &span : cache.spans) {
    if (!span.url) {
      continue;
    }

    markdown_apply_span_font(span, aspect);

    std::string owned_marker;
    const StringRef text = markdown_span_draw_text(span, owned_marker);
    const float descender = BLF_descender(span.fontid) * aspect;
    const float ascender = BLF_ascender(span.fontid) * aspect;
    const float font_height = ascender - descender;
    const float width = BLF_width(span.fontid, text.data(), text.size());

    const float x = button->rect.xmin + span.x;
    const float y = button->rect.ymax + span.y + descender;

    const EmbossType previous_emboss = block_emboss_get(block);
    block_emboss_set(block, EmbossType::None);
    Button *but = uiDefIconBut(
        block, ButtonType::But, ICON_NONE, x, y, width, font_height, nullptr, 0.0, 0.0, std::nullopt);
    block_emboss_set(block, previous_emboss);

    static_cast<ButtonPush *>(but)->draw_as_link = true;
    button_func_identity_compare_set(but, markdown_link_identity_compare);

    PointerRNA props = WM_operator_properties_create_ptr(ot);
    RNA_string_set(&props, "url", std::string(*span.url).c_str());
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

    markdown_disable_span_font(span);
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
  const float aspect = button->block->aspect;
  const int width = std::max<int>(std::ceil(BLI_rctf_size_x(&button->rect)), 0);
  StringRef text = button->str;

  /* Try to reuse an already attached cache. */
  if (button->markdown_cache) {
    const MarkdownLayoutCache &cache = *button->markdown_cache;
    if (!(cache.aspect == aspect && cache.wrap_width == width && cache.text == text)) {
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
      if (cache.aspect == aspect && cache.wrap_width == width && cache.text == text) {
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
    button->rect.ymin = button->rect.ymax - button->markdown_cache->content_height;
    return;
  }

  button->markdown_cache = std::make_shared<MarkdownLayoutCache>();
  MarkdownLayoutCache &cache = *button->markdown_cache;
  cache.text = text;
  cache.wrap_width = width;
  cache.aspect = aspect;

  button->block->markdown_layout_cache.append(button->markdown_cache);

  MarkdownLayouter layouter(aspect, cache.text, float(width), cache);
  layouter.compute_layout();

  button->rect.ymin = button->rect.ymax - cache.content_height;
}

void label_markdown_draw(const ButtonLabel *button, const uchar color[4], const rcti *rect)
{
  if (!button->markdown_cache || button->markdown_cache->spans.is_empty()) {
    return;
  }

  const float aspect = button->block->aspect;
  const MarkdownLayoutCache &cache = *button->markdown_cache;

  uchar link_color[4];
  theme::get_color_4ubv(TH_LINK, link_color);
  link_color[3] = color[3];

  for (const MarkdownSpan &span : cache.spans) {
    BLF_size(span.fontid, span.size * aspect);
    BLF_character_weight(span.fontid, span.weight);
    if (span.italic) {
      BLF_enable(span.fontid, BLF_ITALIC);
    }
    else {
      BLF_disable(span.fontid, BLF_ITALIC);
    }

    if (span.url) {
      BLF_color4ubv(span.fontid, link_color);
    }
    else {
      BLF_color4ubv(span.fontid, color);
    }

    std::string owned_marker;
    const StringRef text = markdown_span_draw_text(span, owned_marker);

    const float x = rect->xmin + span.x / aspect;
    const float y = rect->ymax + span.y / aspect;
    BLF_position(span.fontid, x, y, 0.0f);
    BLF_draw(span.fontid, text.data(), text.size());

    BLF_disable(span.fontid, BLF_ITALIC);
  }
}

}  // namespace blender::ui
