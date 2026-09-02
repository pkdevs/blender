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

namespace blender::nodes::node_geo_midi_tempo_at_cc {

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
NODE_STORAGE_FUNCS(NodeGeometryMidiTempoAt)

/* ---------- NODE DECLARATION ---------- */
static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Tempo Events"_ustr)
  .description("Point cloud from Read MIDI File (Tempo Events output)");

  b.add_input<decl::Float>("Time"_ustr)
      .default_value(0.0f)
      .description("Time at which to sample the tempo (unit controlled by the menu)");

  b.add_output<decl::Float>("BPM"_ustr)
      .description("BPM at the given time (last tempo event before or at that time)");
}

/* ---------- UI LAYOUT ---------- */
static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "time_unit", UI_ITEM_NONE, "", ICON_NONE);
}

/* ---------- INITIALIZATION ---------- */
static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeGeometryMidiTempoAt *data = MEM_new<NodeGeometryMidiTempoAt>(__func__);
  data->time_unit = uint8_t(TimeUnit::Seconds); /* default */
  node->storage = data;
}

/* ---------- EXECUTION ---------- */
static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Tempo Events"_ustr);
  const float input_time = params.extract_input<float>("Time"_ustr);

  /* Default BPM if no events or input_time is invalid. */
  float result_bpm = 0.0f;

  /* Read storage to get the selected time unit. */
  const NodeGeometryMidiTempoAt &storage = node_storage(params.node());
  const TimeUnit time_unit = TimeUnit(storage.time_unit);
  const char *time_attr_name = (time_unit == TimeUnit::Seconds) ? "time_s" : "time_qn";

  /* Check if we have a point cloud. */
  if (geometry_set.has_pointcloud()) {
    const PointCloud *points = geometry_set.get_pointcloud();
    if (points && points->totpoint > 0) {
      /* Get attribute data. */
      const bke::AttributeAccessor attributes = points->attributes();

      /* Read time attribute as float array. */
      VArray<float> times = *attributes.lookup_or_default<float>(time_attr_name, AttrDomain::Point, 0.0f);
      VArray<float> bpms = *attributes.lookup_or_default<float>("bpm", AttrDomain::Point, 0.0f);

      /* Ensure we have data. */
      if (!times.is_empty() && !bpms.is_empty()) {
        /* Build vectors for binary search. */
        Vector<float> time_values;
        Vector<float> bpm_values;
        time_values.reserve(points->totpoint);
        bpm_values.reserve(points->totpoint);

        for (const int i : IndexRange(points->totpoint)) {
          time_values.append(times[i]);
          bpm_values.append(bpms[i]);
        }

        /* Binary search to find the last tempo event with time <= input_time. */
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
          result_bpm = bpm_values[best_idx];
        }
        else {
          /* No tempo event before input_time; use the first tempo event. */
          if (!bpm_values.is_empty()) {
            result_bpm = bpm_values[0];
          }
        }
      }
      else {
        /* Missing attributes - fallback to 0. */
        result_bpm = 0.0f;
      }
    }
  }

  /* Set output. */
  params.set_output("BPM"_ustr, result_bpm);
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
  bke::node_type_base(ntype, "GeometryNodeMidiTempoAt"_ustr, NODE_CLASS_GEOMETRY);
  ntype.ui_name = "MIDI Tempo At";
  ntype.ui_description = "Get the BPM at a specific time from MIDI tempo events";
  ntype.enum_name_legacy = "MIDI_TEMPO_AT";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.gather_link_search_ops = node_gather_link_search_ops;
  bke::node_type_storage(ntype,
                         "NodeGeometryMidiTempoAt",
                         node_free_standard_storage,
                         node_copy_standard_storage);
  bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_midi_tempo_at_cc
