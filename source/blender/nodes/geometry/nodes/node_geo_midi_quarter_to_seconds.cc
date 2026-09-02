 /* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"
#include "BKE_pointcloud.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"
#include "RNA_enum_types.hh"
#include "NOD_rna_define.hh"
#include "DNA_node_types.h"
#include "NOD_socket_search_link.hh"

namespace blender::nodes::node_geo_midi_quarter_to_seconds_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Tempo Events"_ustr)
  .description("Point cloud from Read MIDI File (Tempo Events output)");

  b.add_input<decl::Float>("Time (qn)"_ustr)
      .default_value(0.0f)
      .description("Time in quarter notes");

  b.add_output<decl::Float>("Time (s)"_ustr)
      .description("Time in seconds");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Tempo Events"_ustr);
  const float input_qn = params.extract_input<float>("Time (qn)"_ustr);

  float result_seconds = 0.0f;

  if (geometry_set.has_pointcloud()) {
    const PointCloud *points = geometry_set.get_pointcloud();
    if (points && points->totpoint > 0) {
      const bke::AttributeAccessor attributes = points->attributes();

      VArray<float> time_qn_attr = *attributes.lookup_or_default<float>("time_qn", AttrDomain::Point, 0.0f);
      VArray<float> time_s_attr = *attributes.lookup_or_default<float>("time_s", AttrDomain::Point, 0.0f);
      VArray<float> bpm_attr = *attributes.lookup_or_default<float>("bpm", AttrDomain::Point, 0.0f);

      if (!time_qn_attr.is_empty() && !time_s_attr.is_empty() && !bpm_attr.is_empty()) {
        /* Construct arrays for binary search. */
        Vector<float> qn_values;
        Vector<float> s_values;
        Vector<float> bpm_values;
        qn_values.reserve(points->totpoint);
        s_values.reserve(points->totpoint);
        bpm_values.reserve(points->totpoint);

        for (const int i : IndexRange(points->totpoint)) {
          qn_values.append(time_qn_attr[i]);
          s_values.append(time_s_attr[i]);
          bpm_values.append(bpm_attr[i]);
        }

        /* Binary search: last event with time_qn <= input_qn. */
        int low = 0, high = qn_values.size() - 1;
        int best_idx = -1;

        while (low <= high) {
          int mid = (low + high) / 2;
          if (qn_values[mid] <= input_qn) {
            best_idx = mid;
            low = mid + 1;
          }
          else {
            high = mid - 1;
          }
        }

        if (best_idx >= 0) {
          /* Use BPMfrom found event */
          const float event_qn = qn_values[best_idx];
          const float event_s = s_values[best_idx];
          const float bpm = bpm_values[best_idx];

          if (bpm > 0.0f) {
            const float seconds_per_qn = 60.0f / bpm;
            result_seconds = event_s + (input_qn - event_qn) * seconds_per_qn;
          }
          else {
            result_seconds = event_s;
          }
        }
        else {
          /* in case input_qn comes before the first event: use first event. */
          if (!s_values.is_empty()) {
            result_seconds = s_values[0];
          }
        }
      }
    }
  }

  params.set_output("Time (s)"_ustr, result_seconds);
}

static void node_gather_link_search_ops(GatherLinkSearchOpParams &params)
{
  const NodeDeclaration &declaration = *params.node_type().static_declaration;
  search_link_ops_for_declarations(params, declaration.inputs);
}

static void node_register()
{
  static bke::bNodeType ntype;
  bke::node_type_base(ntype, "GeometryNodeMidiQuarterToSeconds"_ustr, NODE_CLASS_GEOMETRY);
  ntype.ui_name = "MIDI Quarter to Seconds";
  ntype.ui_description = "Convert quarter notes to seconds using tempo events";
  ntype.enum_name_legacy = "MIDI_QUARTER_TO_SECONDS";
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.gather_link_search_ops = node_gather_link_search_ops;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_midi_quarter_to_seconds_cc
