/* SPDX-FileCopyrightText: 2024 NVIDIA Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "usd_reader_utils.hh"
#include "usd_reader_material.hh"
#include "usd_reader_prim.hh"

#include "BLI_string_ref.hh"

#include "BKE_idprop.hh"
#include "BKE_material.hh"
#include "BKE_object.hh"

#include "DNA_material_types.h"
#include "DNA_object_types.h"

#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/tokens.h>
#include <pxr/usd/usdUI/accessibilityAPI.h>

#include "CLG_log.h"

namespace blender {

static CLG_LogRef LOG = {"io.usd"};

namespace {

template<typename VECT>
void set_array_prop(IDProperty *idgroup,
                    const StringRefNull prop_name,
                    const pxr::UsdAttribute &attr,
                    const pxr::UsdTimeCode time)
{
  if (!idgroup || !attr) {
    return;
  }

  VECT vec;
  if (!attr.Get<VECT>(&vec, time)) {
    return;
  }

  IDPropertyTemplate val = {0};
  val.array.len = int(vec.dimension);

  if (val.array.len <= 0) {
    CLOG_WARN(&LOG, "Invalid array length for prop %s", prop_name.c_str());
    return;
  }

  if (std::is_same<float, typename VECT::ScalarType>()) {
    val.array.type = IDP_FLOAT;
  }
  else if (std::is_same<pxr::GfHalf, typename VECT::ScalarType>()) {
    val.array.type = IDP_FLOAT;
  }
  else if (std::is_same<double, typename VECT::ScalarType>()) {
    val.array.type = IDP_DOUBLE;
  }
  else if (std::is_same<int, typename VECT::ScalarType>()) {
    val.array.type = IDP_INT;
  }
  else {
    CLOG_WARN(&LOG, "Couldn't determine array type for prop %s", prop_name.c_str());
    return;
  }

  IDProperty *prop = IDP_New(IDP_ARRAY, &val, prop_name);

  if (!prop) {
    CLOG_WARN(&LOG, "Couldn't create array prop %s", prop_name.c_str());
    return;
  }

  if (std::is_same<pxr::GfHalf, typename VECT::ScalarType>()) {
    float *prop_data = static_cast<float *>(prop->data.pointer);
    for (int i = 0; i < val.array.len; ++i) {
      prop_data[i] = vec[i];
    }
  }
  else {
    std::memcpy(prop->data.pointer, vec.data(), prop->len * sizeof(typename VECT::ScalarType));
  }

  IDP_AddToGroup(idgroup, prop);
}

bool equivalent(const pxr::SdfValueTypeName &type_name1, const pxr::SdfValueTypeName &type_name2)
{
  return type_name1.GetType().IsA(type_name2.GetType());
}

}  // anonymous namespace

namespace io::usd {

static void set_string_prop(IDProperty *idgroup,
                            const StringRefNull prop_name,
                            const StringRefNull str_val)
{
  if (!idgroup) {
    return;
  }

  IDPropertyTemplate val = {0};
  val.string.str = str_val.data();
  /* Note length includes null terminator. */
  val.string.len = str_val.size() + 1;
  val.string.subtype = IDP_STRING_SUB_UTF8;

  IDProperty *prop = IDP_New(IDP_STRING, &val, prop_name);

  IDP_AddToGroup(idgroup, prop);
}

static void set_int_prop(IDProperty *idgroup, const StringRefNull prop_name, const int ival)
{
  if (!idgroup) {
    return;
  }

  IDPropertyTemplate val = {0};
  val.i = ival;
  IDProperty *prop = IDP_New(IDP_INT, &val, prop_name);

  IDP_AddToGroup(idgroup, prop);
}

static void set_bool_prop(IDProperty *idgroup, const StringRefNull prop_name, const bool bval)
{
  if (!idgroup) {
    return;
  }

  IDPropertyTemplate val = {0};
  val.i = bval;
  IDProperty *prop = IDP_New(IDP_BOOLEAN, &val, prop_name);

  IDP_AddToGroup(idgroup, prop);
}

static void set_float_prop(IDProperty *idgroup, const StringRefNull prop_name, const float fval)
{
  if (!idgroup) {
    return;
  }

  IDPropertyTemplate val = {0};
  val.f = fval;
  IDProperty *prop = IDP_New(IDP_FLOAT, &val, prop_name);

  IDP_AddToGroup(idgroup, prop);
}

static void set_double_prop(IDProperty *idgroup, const StringRefNull prop_name, const double dval)
{
  if (!idgroup) {
    return;
  }

  IDPropertyTemplate val = {0};
  val.d = dval;
  IDProperty *prop = IDP_New(IDP_DOUBLE, &val, prop_name);

  IDP_AddToGroup(idgroup, prop);
}

static void set_accessibility_property(const pxr::UsdAttribute &attr,
                                       IDProperty *idgroup,
                                       const pxr::UsdTimeCode time_code)
{
  /* Only set the property if the attribute has an authored value. */
  if (!attr.IsAuthored()) {
    return;
  }

  /* Since STRING properties do not support keyframes, if there is already a value written
   * for this property, don't try to write it again. */
  std::string property_name = attr.GetName().GetString();
  if (IDP_GetPropertyFromGroup(idgroup, property_name)) {
    return;
  }

  pxr::SdfValueTypeName type_name = attr.GetTypeName();
  if (type_name == pxr::SdfValueTypeNames->String) {
    std::string value;
    if (attr.Get<std::string>(&value, time_code)) {
      set_string_prop(idgroup, property_name, value);
    }
  }
  else if (type_name == pxr::SdfValueTypeNames->Token) {
    pxr::TfToken value;
    if (attr.Get<pxr::TfToken>(&value, time_code)) {
      set_string_prop(idgroup, property_name, value.GetString());
    }
  }
}

void set_id_props_from_prim(ID *id,
                            const pxr::UsdPrim &prim,
                            const PropertyImportMode property_import_mode,
                            const pxr::UsdTimeCode time_code)
{
  for (const auto &api : pxr::UsdUIAccessibilityAPI::GetAll(prim)) {
    IDProperty *idgroup = IDP_EnsureProperties(id);
    set_accessibility_property(api.GetLabelAttr(), idgroup, time_code);
    set_accessibility_property(api.GetDescriptionAttr(), idgroup, time_code);
    set_accessibility_property(api.GetPriorityAttr(), idgroup, time_code);
  }

  pxr::UsdAttributeVector attribs = prim.GetAuthoredAttributes();
  if (attribs.empty()) {
    return;
  }

  bool all_custom_attrs = (property_import_mode == PropertyImportMode::All);

  for (const pxr::UsdAttribute &attr : attribs) {
    if (!attr.IsCustom()) {
      continue;
    }

    std::vector<std::string> attr_names = attr.SplitName();

    const bool is_user_prop = attr_names[0] == "userProperties";

    if (attr_names.size() > 2 && is_user_prop && attr_names[1] == "blender") {
      continue;
    }

    if (!all_custom_attrs && !is_user_prop) {
      continue;
    }

    IDProperty *idgroup = IDP_EnsureProperties(id);

    /* When importing user properties, strip the namespace. */
    pxr::TfToken attr_name;
    if (is_user_prop) {
      /* We strip the userProperties namespace, but leave others in case
       * someone's custom attribute namespace is important in their pipeline. */
      const StringRefNull token = "userProperties:";
      const StringRefNull name = attr.GetName().GetString();
      attr_name = pxr::TfToken(name.substr(token.size()));
    }
    else {
      attr_name = attr.GetName();
    }

    pxr::SdfValueTypeName type_name = attr.GetTypeName();

    if (type_name == pxr::SdfValueTypeNames->Int) {
      int ival = 0;
      if (attr.Get<int>(&ival, time_code)) {
        set_int_prop(idgroup, attr_name.GetString(), ival);
      }
    }
    else if (type_name == pxr::SdfValueTypeNames->Float) {
      float fval = 0.0f;
      if (attr.Get<float>(&fval, time_code)) {
        set_float_prop(idgroup, attr_name.GetString(), fval);
      }
    }
    else if (type_name == pxr::SdfValueTypeNames->Double) {
      double dval = 0.0;
      if (attr.Get<double>(&dval, time_code)) {
        set_double_prop(idgroup, attr_name.GetString(), dval);
      }
    }
    else if (type_name == pxr::SdfValueTypeNames->Half) {
      pxr::GfHalf hval = 0.0f;
      if (attr.Get<pxr::GfHalf>(&hval, time_code)) {
        set_float_prop(idgroup, attr_name.GetString(), hval);
      }
    }
    else if (type_name == pxr::SdfValueTypeNames->String) {
      std::string sval;
      if (attr.Get<std::string>(&sval, time_code)) {
        set_string_prop(idgroup, attr_name.GetString(), sval);
      }
    }
    else if (type_name == pxr::SdfValueTypeNames->Token) {
      pxr::TfToken tval;
      if (attr.Get<pxr::TfToken>(&tval, time_code)) {
        set_string_prop(idgroup, attr_name.GetString(), tval.GetString());
      }
    }
    else if (type_name == pxr::SdfValueTypeNames->Asset) {
      pxr::SdfAssetPath aval;
      if (attr.Get<pxr::SdfAssetPath>(&aval, time_code)) {
        set_string_prop(idgroup, attr_name.GetString(), aval.GetAssetPath());
      }
    }
    else if (type_name == pxr::SdfValueTypeNames->Bool) {
      bool bval = false;
      if (attr.Get<bool>(&bval, time_code)) {
        set_bool_prop(idgroup, attr_name.GetString(), bval);
      }
    }
    else if (equivalent(type_name, pxr::SdfValueTypeNames->Float2)) {
      set_array_prop<pxr::GfVec2f>(idgroup, attr_name.GetString(), attr, time_code);
    }
    else if (equivalent(type_name, pxr::SdfValueTypeNames->Float3)) {
      set_array_prop<pxr::GfVec3f>(idgroup, attr_name.GetString(), attr, time_code);
    }
    else if (equivalent(type_name, pxr::SdfValueTypeNames->Float4)) {
      set_array_prop<pxr::GfVec4f>(idgroup, attr_name.GetString(), attr, time_code);
    }
    else if (equivalent(type_name, pxr::SdfValueTypeNames->Double2)) {
      set_array_prop<pxr::GfVec2d>(idgroup, attr_name.GetString(), attr, time_code);
    }
    else if (equivalent(type_name, pxr::SdfValueTypeNames->Double3)) {
      set_array_prop<pxr::GfVec3d>(idgroup, attr_name.GetString(), attr, time_code);
    }
    else if (equivalent(type_name, pxr::SdfValueTypeNames->Double4)) {
      set_array_prop<pxr::GfVec4d>(idgroup, attr_name.GetString(), attr, time_code);
    }
    else if (equivalent(type_name, pxr::SdfValueTypeNames->Int2)) {
      set_array_prop<pxr::GfVec2i>(idgroup, attr_name.GetString(), attr, time_code);
    }
    else if (equivalent(type_name, pxr::SdfValueTypeNames->Int3)) {
      set_array_prop<pxr::GfVec3i>(idgroup, attr_name.GetString(), attr, time_code);
    }
    else if (equivalent(type_name, pxr::SdfValueTypeNames->Int4)) {
      set_array_prop<pxr::GfVec4i>(idgroup, attr_name.GetString(), attr, time_code);
    }
    else if (equivalent(type_name, pxr::SdfValueTypeNames->Half2)) {
      set_array_prop<pxr::GfVec2h>(idgroup, attr_name.GetString(), attr, time_code);
    }
    else if (equivalent(type_name, pxr::SdfValueTypeNames->Half3)) {
      set_array_prop<pxr::GfVec3h>(idgroup, attr_name.GetString(), attr, time_code);
    }
    else if (equivalent(type_name, pxr::SdfValueTypeNames->Half4)) {
      set_array_prop<pxr::GfVec4h>(idgroup, attr_name.GetString(), attr, time_code);
    }
  }
}

pxr::UsdShadeMaterial compute_bound_material(const pxr::UsdPrim &prim, MtlPurpose mtl_purpose)
{
  const pxr::UsdShadeMaterialBindingAPI api = pxr::UsdShadeMaterialBindingAPI(prim);

  /* See the following documentation for material resolution behavior:
   * https://openusd.org/release/api/class_usd_shade_material_binding_a_p_i.html#UsdShadeMaterialBindingAPI_MaterialResolution
   */

  pxr::UsdShadeMaterial mtl;
  switch (mtl_purpose) {
    case MtlPurpose::Full:
      mtl = api.ComputeBoundMaterial(pxr::UsdShadeTokens->full);
      if (!mtl) {
        /* Add an additional Blender-specific fallback to help with oddly authored USD files. */
        mtl = api.ComputeBoundMaterial(pxr::UsdShadeTokens->preview);
      }
      break;
    case MtlPurpose::Preview:
      mtl = api.ComputeBoundMaterial(pxr::UsdShadeTokens->preview);
      break;
    case MtlPurpose::All:
      mtl = api.ComputeBoundMaterial(pxr::UsdShadeTokens->allPurpose);
      break;
  }

  return mtl;
}

void assign_materials(Main *bmain,
                      Object *ob,
                      const Map<pxr::SdfPath, int> &mat_index_map,
                      const USDImportParams &params,
                      pxr::UsdStageRefPtr stage,
                      const ImportSettings &settings)
{
  if (!(stage && bmain && ob)) {
    return;
  }

  if (mat_index_map.size() > MAXMAT) {
    return;
  }

  USDMaterialReader mat_reader(params, *bmain);

  for (const auto item : mat_index_map.items()) {
    Material *assigned_mat = find_existing_material(
        item.key, params, settings.mat_name_to_mat, settings.usd_path_to_mat);
    if (!assigned_mat) {
      /* Blender material doesn't exist, so create it now. */

      /* Look up the USD material. */
      pxr::UsdPrim prim = stage->GetPrimAtPath(item.key);
      pxr::UsdShadeMaterial usd_mat(prim);

      if (!usd_mat) {
        CLOG_WARN(
            &LOG, "Couldn't construct USD material from prim %s", item.key.GetAsString().c_str());
        continue;
      }

      const bool have_import_hook = settings.mat_import_hook_sources.contains(item.key);

      /* Add the Blender material. If we have an import hook which can handle this material
       * we don't import USD Preview Surface shaders. */
      assigned_mat = mat_reader.add_material(usd_mat, !have_import_hook);

      if (!assigned_mat) {
        CLOG_WARN(&LOG,
                  "Couldn't create Blender material from USD material %s",
                  item.key.GetAsString().c_str());
        continue;
      }

      settings.mat_name_to_mat.add_new(assigned_mat->id.name + 2, assigned_mat);

      if (params.mtl_name_collision_mode == MtlNameCollisionMode::MakeUnique) {
        /* Record the Blender material we created for the USD material with the given path. */
        settings.usd_path_to_mat.add_new(item.key, assigned_mat);
      }

      if (have_import_hook) {
        /* Defer invoking the hook to convert the material till we can do so from
         * the main thread. */
        settings.usd_path_to_mat_for_hook.add_new(item.key, assigned_mat);
      }
    }

    if (assigned_mat) {
      BKE_object_material_assign_single_obdata(bmain, ob, assigned_mat, item.value);
    }
    else {
      /* This shouldn't happen. */
      CLOG_WARN(&LOG, "Couldn't assign material %s", item.key.GetAsString().c_str());
    }
  }
  if (ob->totcol > 0) {
    ob->actcol = 1;
  }
}

}  // namespace io::usd
}  // namespace blender
