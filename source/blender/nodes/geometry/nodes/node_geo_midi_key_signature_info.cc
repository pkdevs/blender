/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"
#include "BKE_attribute.hh"
#include "BKE_pointcloud.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"
#include "RNA_enum_types.hh"
#include "NOD_rna_define.hh"

namespace blender::nodes::node_geo_midi_key_signature_info_cc {

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
struct NodeGeometryMidiKeySignatureInfo {
  uint8_t time_unit;
};
NODE_STORAGE_FUNCS(NodeGeometryMidiKeySignatureInfo)

/* ---------- NODE DECLARATION ---------- */
static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Key Signature Events"_ustr)
  .description("Point cloud from Read MIDI File (Key Signature Events output)");

  b.add_output<decl::Float>("Time"_ustr)
      .description("Time of the key signature event (unit controlled by the menu)");
  b.add_output<decl::Float>("Key"_ustr)
      .description("Key signature: negative = flats, positive = sharps");
  b.add_output<decl::Float>("Mode"_ustr)
      .description("0 = major, 1 = minor");
  b.add_output<decl::Int>("Track"_ustr)
      .description("Track index");
}

/* ---------- UI LAYOUT ---------- */
static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "time_unit", UI_ITEM_NONE, "", ICON_NONE);
}

/* ---------- INITIALIZATION ---------- */
static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeGeometryMidiKeySignatureInfo *data = MEM_new<NodeGeometryMidiKeySignatureInfo>(__func__);
  data->time_unit = uint8_t(TimeUnit::Seconds);
  node->storage = data;
}

/* ---------- EXECUTION ---------- */
static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Key Signature Events"_ustr);
  if (!geometry_set.has_pointcloud()) {
    params.set_default_remaining_outputs();
    return;
  }

  const PointCloud *points = geometry_set.get_pointcloud();
  if (!points) {
    params.set_default_remaining_outputs();
    return;
  }

  const NodeGeometryMidiKeySignatureInfo &storage = node_storage(params.node());
  const TimeUnit time_unit = TimeUnit(storage.time_unit);
  const char *time_attr_name = (time_unit == TimeUnit::Seconds) ? "time_s" : "time_qn";

  params.set_output("Time"_ustr,
                    AttributeFieldInput::from(time_attr_name, CPPType::get<float>()));
  params.set_output("Key"_ustr,
                    AttributeFieldInput::from("ks_key", CPPType::get<float>()));
  params.set_output("Mode"_ustr,
                    AttributeFieldInput::from("ks_mode", CPPType::get<float>()));
  params.set_output("Track"_ustr,
                    AttributeFieldInput::from("track", CPPType::get<int>()));
}

/* ---------- RNA ---------- */
static void node_rna(StructRNA *srna)
{
  RNA_def_node_enum(srna,
                    "time_unit",
                    "Time Unit",
                    "Unit for time output",
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
  bke::node_type_base(ntype, "GeometryNodeMidiKeySignatureInfo"_ustr, NODE_CLASS_GEOMETRY);
  ntype.ui_name = "MIDI Key Signature Info";
  ntype.ui_description = "Extract MIDI key signature events as fields";
  ntype.enum_name_legacy = "MIDI_KEY_SIGNATURE_INFO";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.gather_link_search_ops = node_gather_link_search_ops;
  bke::node_type_storage(ntype,
                         "NodeGeometryMidiKeySignatureInfo",
                         node_free_standard_storage,
                         node_copy_standard_storage);
  bke::node_register_type(ntype);
  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_midi_key_signature_info_cc
