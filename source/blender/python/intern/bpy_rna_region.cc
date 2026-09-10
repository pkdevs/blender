/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup pythonintern
 *
 * This file extends the region with C/Python API methods.
 */

#include <Python.h>

#include <optional>
#include <string>

#include "DNA_screen_types.h"

#include "BKE_screen.hh"

#include "ED_screen.hh"

#include "UI_interface.hh"

#include "../generic/py_capi_utils.hh"

#include "bpy_rna.hh"
#include "bpy_rna_region.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Active Item
 *
 * Access the active item of lists that store it in the region, see #uiActiveItemState.
 *
 * NOTE: these could be RNA functions, however RNA integers can't represent "no active item",
 * -1 would have to be used which is a valid Python index (the last item), so a script that
 * forgets to check for it silently uses the wrong item. Use None instead.
 * \{ */

PyDoc_STRVAR(
    /* Wrap. */
    bpy_rna_region_active_item_index_get_doc,
    ".. method:: active_item_index_get(listtype_name, list_id, /)\n"
    "\n"
    "   Get the active item of a list drawn in this region, for lists that store their active\n"
    "   item in the region instead of the data they display (``template_list`` without an\n"
    "   active property & tree views).\n"
    "\n"
    "   :param listtype_name: Identifier of the list type: the ``UIList`` subclass, or the name\n"
    "      of the template function for tree views (for custom properties use\n"
    "      :meth:`bpy.types.bpy_struct.id_properties_active_get` instead).\n"
    "   :type listtype_name: str\n"
    "   :param list_id: Identifier of the list, the ``list_id`` of ``template_list``.\n"
    "   :type list_id: str\n"
    "   :return: Index of the active item in the order of the data the list displays.\n"
    "      0 when nothing is stored yet, as the first item is active by default.\n"
    "      None when the active item was cleared.\n"
    "      Callers must check the index is within the range of the data.\n"
    "   :rtype: int | None\n");
static PyObject *bpy_rna_region_active_item_index_get(PyObject *self, PyObject *args)
{
  BPy_StructRNA *pyrna = reinterpret_cast<BPy_StructRNA *>(self);
  ARegion *region = static_cast<ARegion *>(pyrna->ptr->data);

  const char *listtype_name;
  const char *list_id;
  if (!PyArg_ParseTuple(args,
                        "s" /* `listtype_name` */
                        "s" /* `list_id` */
                        ":active_item_index_get",
                        &listtype_name,
                        &list_id))
  {
    return nullptr;
  }

  const std::string full_idname = ui::list_idname_compose(listtype_name, list_id);
  const uiActiveItemState *state = BKE_region_active_item_state_find(region, full_idname);
  const int index = state ? state->active_index : 0;
  if (index < 0) {
    Py_RETURN_NONE;
  }
  return PyLong_FromLong(index);
}

PyDoc_STRVAR(
    /* Wrap. */
    bpy_rna_region_active_item_index_set_doc,
    ".. method:: active_item_index_set(listtype_name, list_id, index, /)\n"
    "\n"
    "   Set the active item of a list drawn in this region, see\n"
    "   :meth:`bpy.types.Region.active_item_index_get`.\n"
    "\n"
    "   :param listtype_name: Identifier of the list type.\n"
    "   :type listtype_name: str\n"
    "   :param list_id: Identifier of the list.\n"
    "   :type list_id: str\n"
    "   :param index: Index of the active item in the order of the data the list displays,\n"
    "      None for no active item.\n"
    "   :type index: int | None\n");
static PyObject *bpy_rna_region_active_item_index_set(PyObject *self, PyObject *args)
{
  BPy_StructRNA *pyrna = reinterpret_cast<BPy_StructRNA *>(self);
  ARegion *region = static_cast<ARegion *>(pyrna->ptr->data);

  const char *listtype_name;
  const char *list_id;
  std::optional<uint> index_opt;
  if (!PyArg_ParseTuple(args,
                        "s"  /* `listtype_name` */
                        "s"  /* `list_id` */
                        "O&" /* `index` */
                        ":active_item_index_set",
                        &listtype_name,
                        &list_id,
                        PyC_ParseOptionalUInt,
                        &index_opt))
  {
    return nullptr;
  }
  /* -1 for no active item, see #uiActiveItemState::active_index.
   * An index above INT_MAX wraps negative, harmless as the list clamps it. Unlikely. */
  const int index = index_opt ? int(*index_opt) : -1;

  const std::string full_idname = ui::list_idname_compose(listtype_name, list_id);
  uiActiveItemState *state = BKE_region_active_item_state_ensure(region, full_idname);
  state->active_index = index;
  ED_region_tag_redraw(region);

  Py_RETURN_NONE;
}

PyMethodDef BPY_rna_region_active_item_index_get_method_def = {
    "active_item_index_get",
    static_cast<PyCFunction>(bpy_rna_region_active_item_index_get),
    METH_VARARGS,
    bpy_rna_region_active_item_index_get_doc,
};

PyMethodDef BPY_rna_region_active_item_index_set_method_def = {
    "active_item_index_set",
    static_cast<PyCFunction>(bpy_rna_region_active_item_index_set),
    METH_VARARGS,
    bpy_rna_region_active_item_index_set_doc,
};

/** \} */

}  // namespace blender
