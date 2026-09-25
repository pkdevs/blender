/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>

#include "BLI_index_range.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_set.hh"
#include "BLI_span.hh"
#include "BLI_straight_skeleton.hh"

namespace blender::straightskeleton::test {

inline bool &get_debug_new_file()
{
  static bool debug_new_file = true;
  return debug_new_file;
}

inline void write_debug_svg(const char *test_name,
                            const StraightSkeletonInput &input,
                            const StraightSkeletonOutput &output,
                            const char *html_filepath = "/tmp/straight_skeleton.html")
{
  const OffsetIndices<int> face_offsets = output.face_offsets_data.is_empty() ?
                                              output.face_offsets :
                                              OffsetIndices<int>(
                                                  output.face_offsets_data.as_span());

  /* Build the inline SVG content into a string first, then append it to the HTML file. */

  /* Compute bounding box of input vertices. */
  double in_xmin = 1e30, in_xmax = -1e30, in_ymin = 1e30, in_ymax = -1e30;
  for (const double2 &v : input.vert) {
    in_xmin = std::min(in_xmin, v.x);
    in_xmax = std::max(in_xmax, v.x);
    in_ymin = std::min(in_ymin, v.y);
    in_ymax = std::max(in_ymax, v.y);
  }
  if (in_xmin > in_xmax) {
    in_xmin = 0.0;
    in_xmax = 1.0;
    in_ymin = 0.0;
    in_ymax = 1.0;
  }
  double in_dx = in_xmax - in_xmin;
  double in_dy = in_ymax - in_ymin;
  if (in_dx < 1e-4) {
    in_dx = 1.0;
  }
  if (in_dy < 1e-4) {
    in_dy = 1.0;
  }

  /* Expand bounding box to include nearby output vertices, filtering out distant ray
   * intersections. */
  double xmin = in_xmin - 0.15 * in_dx;
  double xmax = in_xmax + 0.15 * in_dx;
  double ymin = in_ymin - 0.15 * in_dy;
  double ymax = in_ymax + 0.15 * in_dy;
  for (const double2 &v : output.vert) {
    if (v.x >= in_xmin - 1.0 * in_dx && v.x <= in_xmax + 1.0 * in_dx &&
        v.y >= in_ymin - 1.0 * in_dy && v.y <= in_ymax + 1.0 * in_dy)
    {
      xmin = std::min(xmin, v.x);
      xmax = std::max(xmax, v.x);
      ymin = std::min(ymin, v.y);
      ymax = std::max(ymax, v.y);
    }
  }

  double dx = xmax - xmin;
  double dy = ymax - ymin;
  const double pad = std::max(dx, dy) * 0.1;
  xmin -= pad;
  xmax += pad;
  ymin -= pad;
  ymax += pad;
  dx = xmax - xmin;
  dy = ymax - ymin;

  const double base_size = 500.0;
  const double scale = std::min(base_size / dx, base_size / dy);
  const double width = dx * scale + 100.0;
  const double height = dy * scale + 120.0;

  auto tx = [&](double x) { return 50.0 + (x - xmin) * scale; };
  auto ty = [&](double y) { return height - 50.0 - (y - ymin) * scale; };

  std::ostringstream svg;

  svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 " << width << " " << height
      << "\" width=\"" << width << "\" height=\"" << height
      << "\" style=\"background-color:#ffffff; overflow:hidden; font-family: -apple-system, "
         "BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif;\">\n";

  /* Definitions & styles. */
  svg << "<defs>\n"
      << "  <filter id=\"shadow\" x=\"-10%\" y=\"-10%\" width=\"120%\" height=\"120%\">\n"
      << "    <feDropShadow dx=\"1\" dy=\"1\" stdDeviation=\"1.5\" flood-opacity=\"0.2\"/>\n"
      << "  </filter>\n"
      << "</defs>\n";

  /* Title. */
  svg << "<text x=\"30\" y=\"40\" font-size=\"20\" font-weight=\"bold\" fill=\"#2c3e50\">"
      << (test_name ? test_name : "Straight Skeleton") << "</text>\n";

  /* Pastel face color palette. */
  static const char *face_colors[] = {
      "#d4e6f1",
      "#d5f5e3",
      "#fdebd0",
      "#e8daef",
      "#fcf3cf",
      "#fadbd8",
      "#d0ece7",
      "#ebf5fb",
      "#eafaf1",
      "#fef9e7",
  };
  const int num_colors = int(sizeof(face_colors) / sizeof(face_colors[0]));
  const bool draw_labels = []() {
    if (const char *env = std::getenv("STRAIGHT_SKELETON_LABELS")) {
      return (std::string_view(env) == "1" || std::string_view(env) == "true");
    }
    return false;
  }();

  /* Draw faces. */
  const Span<int> fvi = output.face_vert_indices;
  for (const int fi : face_offsets.index_range()) {
    const IndexRange r = face_offsets[fi];
    const int n = int(r.size());
    if (n < 3) {
      continue;
    }
    const char *col = face_colors[fi % num_colors];
    svg << "<polygon points=\"";
    double cx = 0.0, cy = 0.0;
    for (const int idx : r) {
      const double2 p = output.vert[fvi[idx]];
      svg << tx(p.x) << "," << ty(p.y) << " ";
      cx += p.x;
      cy += p.y;
    }
    cx /= double(n);
    cy /= double(n);
    svg << "\" fill=\"" << col << "\" stroke=\"#7f8c8d\" stroke-width=\"1\" opacity=\"0.85\"/>\n";

    /* Face label at centroid if vert count is reasonable. */
    if (draw_labels) {
      svg << "<text x=\"" << tx(cx) << "\" y=\"" << ty(cy)
          << "\" font-size=\"14\" font-weight=\"bold\" fill=\"#34495e\" text-anchor=\"middle\" "
             "dominant-baseline=\"middle\" filter=\"url(#shadow)\">f"
          << fi << "</text>\n";
    }
  }

  /* Identify input boundary edges to distinguish from straight skeleton internal edges. */
  Set<std::pair<int, int>> input_boundary_edges;
  const int n_outer = int(input.outer_polygon.size());
  for (int k = 0; k < n_outer; k++) {
    const int u = input.outer_polygon[k];
    const int v = input.outer_polygon[(k + 1) % n_outer];
    input_boundary_edges.add({std::min(u, v), std::max(u, v)});
  }
  for (const int h : IndexRange(input.hole_offsets.size())) {
    const IndexRange r = input.hole_offsets[h];
    const int n_h = int(r.size());
    for (int k = 0; k < n_h; k++) {
      const int u = input.hole_vert_indices[r[k]];
      const int v = input.hole_vert_indices[r[(k + 1) % n_h]];
      input_boundary_edges.add({std::min(u, v), std::max(u, v)});
    }
  }

  /* Draw straight skeleton edges (non-boundary internal arcs in crimson red). */
  for (const int2 &e : output.edge) {
    const std::pair<int, int> pair_e = {std::min(e[0], e[1]), std::max(e[0], e[1])};
    if (input_boundary_edges.contains(pair_e)) {
      continue;
    }
    const double2 p1 = output.vert[e[0]];
    const double2 p2 = output.vert[e[1]];
    svg << "<line x1=\"" << tx(p1.x) << "\" y1=\"" << ty(p1.y) << "\" x2=\"" << tx(p2.x)
        << "\" y2=\"" << ty(p2.y) << "\" stroke=\"#e74c3c\" stroke-width=\""
        << (draw_labels ? "2.5" : "1.5") << "\" stroke-linecap=\"round\"/>\n";
  }

  /* Draw input boundary edges in bold dark blue. */
  for (const auto &e : input_boundary_edges) {
    if (e.first < output.vert.size() && e.second < output.vert.size()) {
      const double2 p1 = output.vert[e.first];
      const double2 p2 = output.vert[e.second];
      svg << "<line x1=\"" << tx(p1.x) << "\" y1=\"" << ty(p1.y) << "\" x2=\"" << tx(p2.x)
          << "\" y2=\"" << ty(p2.y) << "\" stroke=\"#2980b9\" stroke-width=\""
          << (draw_labels ? "3.5" : "2.0") << "\" stroke-linecap=\"round\"/>\n";
    }
  }

  /* Draw vertices and vertex labels if reasonable count. */
  if (draw_labels) {
    for (const int vi : output.vert.index_range()) {
      const double2 p = output.vert[vi];
      const bool is_input_vert = (vi < input.vert.size());
      const char *circle_color = is_input_vert ? "#2980b9" : "#e74c3c";
      const double r = is_input_vert ? 5.0 : 4.0;

      svg << "<circle cx=\"" << tx(p.x) << "\" cy=\"" << ty(p.y) << "\" r=\"" << r << "\" fill=\""
          << circle_color
          << "\" stroke=\"#ffffff\" stroke-width=\"1.5\" filter=\"url(#shadow)\"/>\n";

      /* Offset label away from vertex. */
      svg << "<text x=\"" << (tx(p.x) + 7.0) << "\" y=\"" << (ty(p.y) - 6.0)
          << "\" font-size=\"11\" font-weight=\"bold\" fill=\""
          << (is_input_vert ? "#1a5276" : "#922b21") << "\">v" << vi << "</text>\n";
    }
  }

  /* Legend in top-right corner. */
  const double leg_x = width - 200.0;
  svg << "<g transform=\"translate(" << leg_x << ", 15)\">\n"
      << "  <rect width=\"180\" height=\"75\" fill=\"#f8f9fa\" stroke=\"#dee2e6\" rx=\"6\"/>\n"
      << "  <line x1=\"15\" y1=\"20\" x2=\"45\" y2=\"20\" stroke=\"#2980b9\" "
         "stroke-width=\"3.5\"/>\n"
      << "  <text x=\"55\" y=\"24\" font-size=\"12\" fill=\"#2c3e50\">Input Boundary</text>\n"
      << "  <line x1=\"15\" y1=\"40\" x2=\"45\" y2=\"40\" stroke=\"#e74c3c\" "
         "stroke-width=\"2.5\"/>\n"
      << "  <text x=\"55\" y=\"44\" font-size=\"12\" fill=\"#2c3e50\">Skeleton Edge</text>\n"
      << "  <rect x=\"15\" y=\"52\" width=\"30\" height=\"12\" fill=\"#d4e6f1\" "
         "stroke=\"#7f8c8d\" "
         "stroke-width=\"1\"/>\n"
      << "  <text x=\"55\" y=\"63\" font-size=\"12\" fill=\"#2c3e50\">Output Face</text>\n"
      << "</g>\n";

  svg << "</svg>\n";

  /* Also write standalone SVG file to /tmp/<test_name>.svg if test_name is non-empty. */
  if (test_name && test_name[0] != '\0') {
    std::string safe_name;
    for (const char *c = test_name; *c != '\0'; c++) {
      if (*c == ' ' || *c == '/' || *c == '=' || *c == '(' || *c == ')') {
        safe_name += '_';
      }
      else {
        safe_name += *c;
      }
    }
    std::string svg_path = std::string("/tmp/") + safe_name + ".svg";
    std::ofstream sf(svg_path);
    if (sf.is_open()) {
      sf << svg.str();
      sf.close();
      std::cout << "Standalone SVG written to: " << svg_path << "\n";
    }
  }

  /* Append (or create) the HTML debug file. */
  const char *target_html = std::getenv("STRAIGHT_SKELETON_HTML_FILE");
  if (!target_html || target_html[0] == '\0') {
    target_html = html_filepath;
  }

  bool &debug_new_file = get_debug_new_file();
  if (const char *app_env = std::getenv("STRAIGHT_SKELETON_APPEND_HTML")) {
    if (std::string_view(app_env) == "1" || std::string_view(app_env) == "true") {
      debug_new_file = false;
    }
  }
  const std::ios::openmode mode = debug_new_file ? (std::ios::out | std::ios::trunc) :
                                                   (std::ios::out | std::ios::app);

  std::ofstream f(target_html, mode);
  if (!f.is_open()) {
    std::cout << "Could not open debug HTML file: " << target_html << "\n";
    return;
  }

  if (debug_new_file) {
    /* Write the HTML document header on first open. */
    f << "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n"
      << "<meta charset=\"utf-8\">\n"
      << "<title>Straight Skeleton Drawings</title>\n"
      << "<style>\n"
      << "  body { font-family: sans-serif; background: #1a1a2e; color: #e0e0e0;"
         " margin: 0; padding: 20px; }\n"
      << "  h1 { color: #a0c4ff; border-bottom: 2px solid #a0c4ff; padding-bottom: 8px; }\n"
      << "  .card { display: inline-block; background: #16213e; border: 1px solid #3a3a5c;"
         " border-radius: 8px; padding: 12px; margin: 12px; vertical-align: top; }\n"
      << "  .card svg { display: block; background: #fff; border-radius: 4px; }\n"
      << "</style>\n"
      << "</head>\n<body>\n"
      << "<h1>Straight Skeleton Drawings</h1>\n";
    debug_new_file = false;
  }

  /* Wrap the SVG in a labelled card div. */
  f << "<div class=\"card\">\n" << svg.str() << "</div>\n";
  f.close();

  std::cout << "SVG appended to: " << target_html << "\n";
}

}  // namespace blender::straightskeleton::test
