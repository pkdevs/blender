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

namespace blender::nodes::node_geo_midi_seconds_to_quarter_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Tempo Events"_ustr)
  .description("Point cloud from Read MIDI File (Tempo Events output)");

  b.add_input<decl::Float>("Time (s)"_ustr)
      .default_value(0.0f)
      .description("Time in seconds to convert to quarter notes");

  b.add_output<decl::Float>("Quarter Notes"_ustr)
      .description("Number of quarter notes elapsed at the given time");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Tempo Events"_ustr);
  const float input_time = params.extract_input<float>("Time (s)"_ustr);

  float result_quarter = 0.0f;

  if (geometry_set.has_pointcloud()) {
    const PointCloud *points = geometry_set.get_pointcloud();
    if (points && points->totpoint > 0) {
      const bke::AttributeAccessor attributes = points->attributes();

      VArray<float> times = *attributes.lookup_or_default<float>("time_s", AttrDomain::Point, 0.0f);
      VArray<float> bpms = *attributes.lookup_or_default<float>("bpm", AttrDomain::Point, 0.0f);

      if (!times.is_empty() && !bpms.is_empty()) {
        /* Build arrays for processing. */
        Vector<float> time_values;
        Vector<float> bpm_values;
        time_values.reserve(points->totpoint);
        bpm_values.reserve(points->totpoint);

        for (const int i : IndexRange(points->totpoint)) {
          time_values.append(times[i]);
          bpm_values.append(bpms[i]);
        }

        /* If input_time is before first tempo event, use the first tempo for extrapolation? */
        if (input_time <= time_values[0]) {
          /* From time 0 to first tempo event, use the first BPM. */
          float seconds_to_first = input_time;
          float bpm = bpm_values[0];
          float quarter = seconds_to_first * (bpm / 60.0f);
          result_quarter = quarter;
        }
        else {
          /* Accumulate quarters up to input_time. */
          float accumulated_quarter = 0.0f;
          float last_time = 0.0f;
          float current_bpm = bpm_values[0]; // assume first tempo from start

          for (int i = 0; i < time_values.size(); i++) {
            float event_time = time_values[i];
            float event_bpm = bpm_values[i];

            /* If input_time is before this event, accumulate up to input_time and stop */
            if (input_time <= event_time) {
              float duration_seconds = input_time - last_time;
              if (duration_seconds > 0) {
                accumulated_quarter += duration_seconds * (current_bpm / 60.0f);
              }
              result_quarter = accumulated_quarter;
              break;
            }

            /* Accumulate from last_time to event_time using current_bpm. */
            float duration_seconds = event_time - last_time;
            if (duration_seconds > 0) {
              accumulated_quarter += duration_seconds * (current_bpm / 60.0f);
            }

            /* Update last_time and current_bpm for next segment. */
            last_time = event_time;
            current_bpm = event_bpm;

            /* If this was the last event and input_time is beyond it, accumulate remainder. */
            if (i == time_values.size() - 1 && input_time > event_time) {
              float remainder = input_time - event_time;
              accumulated_quarter += remainder * (current_bpm / 60.0f);
              result_quarter = accumulated_quarter;
              break;
            }
          }
        }
      }
    }
  }

  params.set_output("Quarter Notes"_ustr, result_quarter);
}

static void node_gather_link_search_ops(GatherLinkSearchOpParams &params)
{
  const NodeDeclaration &declaration = *params.node_type().static_declaration;
  search_link_ops_for_declarations(params, declaration.inputs);
}

static void node_register()
{
  static bke::bNodeType ntype;
  bke::node_type_base(ntype, "GeometryNodeMidiSecondsToQuarter"_ustr, NODE_CLASS_GEOMETRY);
  ntype.ui_name = "MIDI Seconds to Quarter";
  ntype.ui_description = "Convert a time in seconds to quarter notes using tempo events";
  ntype.enum_name_legacy = "MIDI_SECONDS_TO_QUARTER";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.gather_link_search_ops = node_gather_link_search_ops;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_midi_seconds_to_quarter_cc
