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

namespace blender::nodes::node_geo_midi_time_signature_at_cc {

/* ---------- ENUM FOR TIME UNIT ---------- */
enum class TimeUnit : int8_t {
  Seconds = 0,
  QuarterNotes = 1,
};

static const EnumPropertyItem time_unit_items[] = {
    {int(TimeUnit::Seconds), "SECONDS", 0, "Seconds", "Time in seconds"},
    {int(TimeUnit::QuarterNotes), "QUARTER_NOTES", 0, "Quarter Notes", "Time in quarter notes"},
    {},
    };

/* ---------- STORAGE ---------- */
NODE_STORAGE_FUNCS(NodeGeometryMidiTimeSignatureAt)

/* ---------- NODE DECLARATION ---------- */
static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Time Signature Events"_ustr)
  .description("Point cloud from Read MIDI File (Time Signature Events output)");

  b.add_input<decl::Float>("Time"_ustr)
      .default_value(0.0f)
      .description("Time at which to sample the time signature (unit controlled by the menu)");

  b.add_output<decl::Int>("Numerator"_ustr)
      .description("Numerator of the time signature at the given time");
  b.add_output<decl::Int>("Denominator"_ustr)
      .description("Denominator of the time signature at the given time");
  b.add_output<decl::Int>("Track"_ustr)
      .description("Track of the last time signature event before or at the given time");
}

/* ---------- UI LAYOUT ---------- */
static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "time_unit", UI_ITEM_NONE, "", ICON_NONE);
}

/* ---------- INITIALIZATION ---------- */
static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeGeometryMidiTimeSignatureAt *data = MEM_new<NodeGeometryMidiTimeSignatureAt>(__func__);
  data->time_unit = uint8_t(TimeUnit::Seconds); /* default */
  node->storage = data;
}

/* ---------- EXECUTION ---------- */
static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Time Signature Events"_ustr);
  const float input_time = params.extract_input<float>("Time"_ustr);

  /* Default values. */
  int result_num = 4;   // 4/4 is a common default (aka. c in many music scores ;) )
  int result_den = 4;
  int result_track = -1;

  /* Read storage to get the selected time unit: */
  const NodeGeometryMidiTimeSignatureAt &storage = node_storage(params.node());
  const TimeUnit time_unit = TimeUnit(storage.time_unit);
  const char *time_attr_name = (time_unit == TimeUnit::Seconds) ? "time_s" : "time_qn";

  if (geometry_set.has_pointcloud()) {
    const PointCloud *points = geometry_set.get_pointcloud();
    if (points && points->totpoint > 0) {
      const bke::AttributeAccessor attributes = points->attributes();

      VArray<float> times = *attributes.lookup_or_default<float>(time_attr_name, AttrDomain::Point, 0.0f);
      VArray<int> numerators = *attributes.lookup_or_default<int>("ts_numerator", AttrDomain::Point, 4);
      VArray<int> denominators = *attributes.lookup_or_default<int>("ts_denominator", AttrDomain::Point, 4);
      VArray<int> tracks = *attributes.lookup_or_default<int>("track", AttrDomain::Point, -1);

      if (!times.is_empty() && !numerators.is_empty() && !denominators.is_empty()) {
        Vector<float> time_values;
        Vector<int> num_values;
        Vector<int> den_values;
        Vector<int> track_values;
        time_values.reserve(points->totpoint);
        num_values.reserve(points->totpoint);
        den_values.reserve(points->totpoint);
        track_values.reserve(points->totpoint);

        for (const int i : IndexRange(points->totpoint)) {
          time_values.append(times[i]);
          num_values.append(numerators[i]);
          den_values.append(denominators[i]);
          track_values.append(tracks[i]);
        }

        int low = 0, high = time_values.size() - 1;
        int best_idx = -1;
        while (low <= high) {
          int mid = (low + high) / 2;
          if (time_values[mid] <= input_time) {
            best_idx = mid;
            low = mid + 1;
          }
          else {
            high = mid - 1;
          }
        }

        if (best_idx >= 0) {
          result_num = num_values[best_idx];
          result_den = den_values[best_idx];
          result_track = track_values[best_idx];
        }
        else {
          /* No event before input_time; use the first event. */
          if (!num_values.is_empty()) {
            result_num = num_values[0];
            result_den = den_values[0];
            result_track = track_values[0];
          }
        }
      }
    }
  }

  params.set_output("Numerator"_ustr, result_num);
  params.set_output("Denominator"_ustr, result_den);
  params.set_output("Track"_ustr, result_track);
}

/* ---------- RNA ---------- */
static void node_rna(StructRNA *srna)
{
  RNA_def_node_enum(srna,
                    "time_unit",
                    "Time Unit",
                    "Unit for the input time",
                    time_unit_items,
                    NOD_storage_enum_accessors(time_unit),
                    int(TimeUnit::Seconds));
}

// if ones drags the Note Events output from Read MIDI file and drops in
// a blank area from Geometry Nodes, a dialogue appears to search for the
// next node. This register is needed so this node is found there.
static void node_gather_link_search_ops(GatherLinkSearchOpParams &params)
{
  const NodeDeclaration &declaration = *params.node_type().static_declaration;
  search_link_ops_for_declarations(params, declaration.inputs);
}

/* ---------- REGISTER ---------- */
static void node_register()
{
  static bke::bNodeType ntype;
  bke::node_type_base(ntype, "GeometryNodeMidiTimeSignatureAt"_ustr, NODE_CLASS_GEOMETRY);
  ntype.ui_name = "MIDI Time Signature At";
  ntype.ui_description = "Get the time signature at a specific time from MIDI time signature events";
  ntype.enum_name_legacy = "MIDI_TIME_SIGNATURE_AT";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.gather_link_search_ops = node_gather_link_search_ops;
  bke::node_type_storage(ntype,
                         "NodeGeometryMidiTimeSignatureAt",
                         node_free_standard_storage,
                         node_copy_standard_storage);
  bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_midi_time_signature_at_cc
