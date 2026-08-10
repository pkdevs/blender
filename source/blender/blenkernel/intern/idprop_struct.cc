/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Owned DNA struct IDProperty types (#IDP_CURVE_MAPPING, #IDP_COLOR_BAND, ...) and their handlers.
 */

#include "BKE_colorband.hh"
#include "BKE_colortools.hh"
#include "BKE_idprop.hh"

#include "BLI_map.hh"
#include "BLI_string_ref.hh"

#include "BLO_read_write.hh"

#include "DNA_color_types.h"
#include "DNA_colorband_types.h"

#include "MEM_guardedalloc.h"

#include "BLI_strict_flags.hh" /* IWYU pragma: keep. Keep last. */

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Registry
 * \{ */

static const IDPropertyStructTypeInfo *g_idp_struct_types[IDP_NUMTYPES] = {};

static Map<StringRefNull, const IDPropertyStructTypeInfo *> &idp_struct_types_by_rna()
{
  static Map<StringRefNull, const IDPropertyStructTypeInfo *> map;
  return map;
}

void IDP_struct_type_register(const IDPropertyStructTypeInfo *type_info)
{
  BLI_assert(type_info);
  BLI_assert(uint(type_info->idp_type) < IDP_NUMTYPES);
  BLI_assert(type_info->dna_type_name && type_info->dna_type_name[0]);
  BLI_assert(type_info->rna_identifier && type_info->rna_identifier[0]);
  BLI_assert(type_info->create_default);
  BLI_assert(type_info->copy);
  BLI_assert(type_info->free);
  BLI_assert(type_info->blend_write);
  BLI_assert(type_info->blend_read);

  g_idp_struct_types[type_info->idp_type] = type_info;
  idp_struct_types_by_rna().add_overwrite(type_info->rna_identifier, type_info);
}

const IDPropertyStructTypeInfo *IDP_struct_type_info(const eIDPropertyType type)
{
  if (uint(type) >= IDP_NUMTYPES) {
    return nullptr;
  }
  return g_idp_struct_types[type];
}

const IDPropertyStructTypeInfo *IDP_struct_type_find_by_rna_identifier(
    const StringRef rna_identifier)
{
  return idp_struct_types_by_rna().lookup_default_as(rna_identifier, nullptr);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name CurveMapping
 * \{ */

static void *idp_struct_curvemapping_create()
{
  return BKE_curvemapping_add(4, 0.0f, 0.0f, 1.0f, 1.0f);
}

static void *idp_struct_curvemapping_copy(const void *src, const int /*flag*/)
{
  return BKE_curvemapping_copy(static_cast<const CurveMapping *>(src));
}

static void idp_struct_curvemapping_free(void *data)
{
  BKE_curvemapping_free(static_cast<CurveMapping *>(data));
}

static void idp_struct_curvemapping_blend_write(BlendWriter *writer, const void *data)
{
  BKE_curvemapping_blend_write(writer, static_cast<const CurveMapping *>(data));
}

static void idp_struct_curvemapping_blend_read(BlendDataReader *reader, void **data_p)
{
  *data_p = BLO_read_struct_by_name_array(reader, "CurveMapping", 1, *data_p);
  if (*data_p) {
    BKE_curvemapping_blend_read(reader, static_cast<CurveMapping *>(*data_p));
  }
}

static bool idp_struct_curvemapping_equals(const void *a_v, const void *b_v)
{
  const CurveMapping *a = static_cast<const CurveMapping *>(a_v);
  const CurveMapping *b = static_cast<const CurveMapping *>(b_v);
  if (a == b) {
    return true;
  }
  if (!a || !b) {
    return false;
  }
  if (a->flag != b->flag || a->tone != b->tone || a->preset != b->preset) {
    return false;
  }
  if (memcmp(a->black, b->black, sizeof(a->black)) != 0 ||
      memcmp(a->white, b->white, sizeof(a->white)) != 0)
  {
    return false;
  }
  for (int i = 0; i < CM_TOT; i++) {
    if (a->cm[i].totpoint != b->cm[i].totpoint) {
      return false;
    }
    if (a->cm[i].totpoint > 0) {
      if (memcmp(a->cm[i].curve,
                 b->cm[i].curve,
                 sizeof(CurveMapPoint) * size_t(a->cm[i].totpoint)) != 0)
      {
        return false;
      }
    }
  }
  return true;
}

static const IDPropertyStructTypeInfo idp_struct_type_curvemapping = {
    /*idp_type*/ IDP_CURVE_MAPPING,
    /*dna_type_name*/ "CurveMapping",
    /*rna_identifier*/ "CurveMapping",
    /*create_default*/ idp_struct_curvemapping_create,
    /*copy*/ idp_struct_curvemapping_copy,
    /*free*/ idp_struct_curvemapping_free,
    /*blend_write*/ idp_struct_curvemapping_blend_write,
    /*blend_read*/ idp_struct_curvemapping_blend_read,
    /*equals*/ idp_struct_curvemapping_equals,
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name ColorBand (RNA: ColorRamp)
 * \{ */

static void *idp_struct_colorband_create()
{
  return BKE_colorband_add(true);
}

static void *idp_struct_colorband_copy(const void *src, const int /*flag*/)
{
  ColorBand *dst = MEM_new<ColorBand>(__func__);
  *dst = *static_cast<const ColorBand *>(src);
  return dst;
}

static void idp_struct_colorband_free(void *data)
{
  MEM_delete(static_cast<ColorBand *>(data));
}

static void idp_struct_colorband_blend_write(BlendWriter *writer, const void *data)
{
  writer->write_struct(static_cast<const ColorBand *>(data));
}

static void idp_struct_colorband_blend_read(BlendDataReader *reader, void **data_p)
{
  *data_p = BLO_read_struct_by_name_array(reader, "ColorBand", 1, *data_p);
}

static bool idp_struct_colorband_equals(const void *a_v, const void *b_v)
{
  const ColorBand *a = static_cast<const ColorBand *>(a_v);
  const ColorBand *b = static_cast<const ColorBand *>(b_v);
  if (a == b) {
    return true;
  }
  if (!a || !b) {
    return false;
  }
  return memcmp(a, b, sizeof(ColorBand)) == 0;
}

static const IDPropertyStructTypeInfo idp_struct_type_colorband = {
    /*idp_type*/ IDP_COLOR_BAND,
    /*dna_type_name*/ "ColorBand",
    /*rna_identifier*/ "ColorRamp",
    /*create_default*/ idp_struct_colorband_create,
    /*copy*/ idp_struct_colorband_copy,
    /*free*/ idp_struct_colorband_free,
    /*blend_write*/ idp_struct_colorband_blend_write,
    /*blend_read*/ idp_struct_colorband_blend_read,
    /*equals*/ idp_struct_colorband_equals,
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Init / Create
 * \{ */

void IDP_struct_type_init()
{
  IDP_struct_type_register(&idp_struct_type_curvemapping);
  IDP_struct_type_register(&idp_struct_type_colorband);
}

void IDP_struct_type_exit()
{
  for (const IDPropertyStructTypeInfo *&info : g_idp_struct_types) {
    info = nullptr;
  }
  idp_struct_types_by_rna().clear();
}

IDProperty *IDP_NewStruct(const StringRef name,
                          const IDPropertyStructTypeInfo *type_info,
                          const eIDPropertyFlag flags)
{
  BLI_assert(type_info);
  BLI_assert(IDP_struct_type_info(type_info->idp_type) == type_info);

  IDProperty *prop = MEM_new<IDProperty>("IDProperty struct");
  prop->type = type_info->idp_type;
  prop->flag = flags;
  name.copy_utf8_truncated(prop->name);
  prop->data.pointer = type_info->create_default();
  return prop;
}

void *IDP_struct_data_get(const IDProperty *prop)
{
  if (prop == nullptr || IDP_struct_type_info(prop->type) == nullptr) {
    return nullptr;
  }
  return prop->data.pointer;
}

namespace bke::idprop {

std::unique_ptr<IDProperty, IDPropertyDeleter> create_struct(
    const StringRef prop_name,
    const IDPropertyStructTypeInfo *type_info,
    const eIDPropertyFlag flags)
{
  return std::unique_ptr<IDProperty, IDPropertyDeleter>(
      IDP_NewStruct(prop_name, type_info, flags));
}

}  // namespace bke::idprop

/** \} */

}  // namespace blender
