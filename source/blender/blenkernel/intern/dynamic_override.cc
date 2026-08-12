/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 * SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <optional>

#include "MEM_guardedalloc.h"

#include "BLI_build_config.hh"
#include "BLI_enum_flags.hh"
#include "BLI_listbase.hh"
#include "BLI_resource_scope.hh"
#include "BLI_string.hh"
#include "BLI_string_utils.hh"

#include "BLT_translation.hh"

/* Allow using deprecated functionality for .blend file I/O. */
#define DNA_DEPRECATED_ALLOW

#include "DNA_ID.h"
#include "DNA_dynamic_override_types.h"
#include "DNA_scene_types.h"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_path.hh"
#include "RNA_prototypes.hh"

#include "BKE_dynamic_override.hh"
#include "BKE_global.hh"
#include "BKE_idprop.hh"
#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"
#include "BKE_lib_query.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_main_invariants.hh"

#include "WM_types.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "BLO_read_write.hh"

#include "CLG_log.h"

namespace blender {

namespace bke::dynoverride {

struct GeneratedRuleSrnaData;

struct Runtime {};

struct RuleIDDataRuntime {
  /** Contains RNA types generated for the ID data rule interface. */
  std::shared_ptr<GeneratedRuleSrnaData> rule_srna_data = {};
};

static void rule_copy(DynamicOverrideRule &dynoverride_rule_dst,
                      DynamicOverrideRule &dynoverride_rule_src,
                      const int flag);
static void rule_free(DynamicOverrideRule &dynoverride_rule);

static void rule_foreach_id(DynamicOverrideRule &dynoverride_rule, LibraryForeachIDData &data);

static void rule_write(BlendWriter &writer, DynamicOverrideRule &dynoverride_rule);

static void rule_read_data(BlendDataReader &reader, DynamicOverrideRule &dynoverride_rule);

static void init_data(ID *id)
{
  DynamicOverride *dynoverride = id_cast<DynamicOverride *>(id);

  dynoverride->runtime = MEM_new<Runtime>(__func__);
}

static void copy_data(Main * /*bmain*/,
                      std::optional<Library *> /*owner_library*/,
                      ID *id_dst,
                      const ID *id_src,
                      const int flag)
{
  DynamicOverride *dynoverride_dst = id_cast<DynamicOverride *>(id_dst);
  const DynamicOverride *dynoverride_src = id_cast<const DynamicOverride *>(id_src);

  BLI_duplicatelist(&dynoverride_dst->rules, &dynoverride_src->rules);
  for (DynamicOverrideRule *
           rule_dst = static_cast<DynamicOverrideRule *>(dynoverride_dst->rules.first),
          *rule_src = static_cast<DynamicOverrideRule *>(dynoverride_src->rules.first);
       rule_dst;
       rule_dst = rule_dst->next, rule_src = rule_src->next)
  {
    rule_copy(*rule_dst, *rule_src, flag);
  }

  dynoverride_dst->runtime = MEM_new<Runtime>(__func__);
}

static void free_data(ID *id)
{
  DynamicOverride *dynoverride = id_cast<DynamicOverride *>(id);

  for (DynamicOverrideRule &dynoverride_rule : dynoverride->rules) {
    rule_free(dynoverride_rule);
  }
  BLI_freelistN(&dynoverride->rules);

  MEM_delete(dynoverride->runtime);
}

static void foreach_id(ID *id, LibraryForeachIDData *data)
{
  DynamicOverride *dynoverride = id_cast<DynamicOverride *>(id);

  for (DynamicOverrideRule &dynoverride_rule : dynoverride->rules) {
    rule_foreach_id(dynoverride_rule, *data);
  }
}

static void blend_write(BlendWriter *writer, ID *id, const void *id_address)
{
  DynamicOverride *dynoverride = id_cast<DynamicOverride *>(id);

  /* Clean up, important in undo case to reduce false detection of changed datablocks. */
  dynoverride->runtime = nullptr;

  /* Write ID itself. */
  writer->write_id_struct(id_address, dynoverride);
  BKE_id_blend_write(writer, &dynoverride->id);

  for (DynamicOverrideRule &dynoverride_rule : dynoverride->rules) {
    rule_write(*writer, dynoverride_rule);
  }
}

static void blend_read_data(BlendDataReader *reader, ID *id)
{
  DynamicOverride *dynoverride = id_cast<DynamicOverride *>(id);

  dynoverride->runtime = MEM_new<Runtime>(__func__);

  BLO_read_struct_list(reader, DynamicOverrideRule, &dynoverride->rules);
  for (DynamicOverrideRule &dynoverride_rule : dynoverride->rules) {
    rule_read_data(*reader, dynoverride_rule);
  }
}

static void blend_read_after_liblink(BlendLibReader * /*reader*/, ID *id)
{
  DynamicOverride *dynoverride = id_cast<DynamicOverride *>(id);

  /* Cleanup invalid rules.
   *
   * The target ID is a weak link, so it may become nullptr, in which case the rule itself can also
   * be removed.
   *
   * TODO: likely move this into dedicated util, as 'removable' data logic will become more complex
   * once we add more filtering logic e.g. */
  for (DynamicOverrideRule &dynoverride_rule : dynoverride->rules.items_mutable()) {
    if (!dynoverride_rule.target_filter.target_id) {
      BLI_remlink(&dynoverride->rules, &dynoverride_rule);
      rule_free(dynoverride_rule);
      MEM_delete(&dynoverride_rule);
    }
  }
}

}  // namespace bke::dynoverride

IDTypeInfo IDType_ID_OV = {
    .id_code = DynamicOverride::id_type,
    .id_filter = FILTER_ID_OV,
    .dependencies_id_types = FILTER_ID_ALL,
    .main_listbase_index = INDEX_ID_OV,
    .struct_size = sizeof(DynamicOverride),
    .name = "Dynamic Override",
    .name_plural = N_("dynamic_overrides"),
    .translation_context = BLT_I18NCONTEXT_ID_DYNAMIC_OVERRIDE,
    .flags = 0,
    .asset_type_info = nullptr,

    .init_data = bke::dynoverride::init_data,
    .copy_data = bke::dynoverride::copy_data,
    .free_data = bke::dynoverride::free_data,
    .make_local = nullptr,
    .foreach_id = bke::dynoverride::foreach_id,
    .foreach_cache = nullptr,
    .foreach_path = nullptr,
    .foreach_working_space_color = nullptr,
    .owner_pointer_get = nullptr,

    .blend_write = bke::dynoverride::blend_write,
    .blend_read_data = bke::dynoverride::blend_read_data,
    .blend_read_after_liblink = bke::dynoverride::blend_read_after_liblink,

    .blend_read_undo_preserve = nullptr,

    .lib_override_apply_post = nullptr,
};

namespace bke::dynoverride {

/* -------------------------------------------------------------------- */
/** \name Helpers for IDTypeInfo callbacks.
 * \{ */

static void rule_property_copy(DynamicOverrideRuleProperty &dynoverride_rule_property_dst,
                               DynamicOverrideRuleProperty &dynoverride_rule_property_src,
                               const int /*flag*/)
{
  /* NOTE: A flat copy is assumed to have already happened before calling this function (e.g. by
   * using `BLI_duplicatelist`). */

  dynoverride_rule_property_dst.rna_path = MEM_dupalloc(dynoverride_rule_property_src.rna_path);
}

static void rule_copy(DynamicOverrideRule &dynoverride_rule_dst,
                      DynamicOverrideRule &dynoverride_rule_src,
                      const int flag)
{
  /* NOTE: A flat copy is assumed to have already happened before calling this function (e.g. by
   * using `BLI_duplicatelist`). */

  BLI_assert(dynoverride_rule_dst.type == dynoverride_rule_src.type);
  BLI_assert(dynoverride_rule_dst.type != DynamicOverrideRuleType::Unknown);

  dynoverride_rule_dst.name = MEM_dupalloc(dynoverride_rule_src.name);

  switch (dynoverride_rule_dst.type) {
    case DynamicOverrideRuleType::IDData: {
      DynamicOverrideRuleIDData &rule_dst = reinterpret_cast<DynamicOverrideRuleIDData &>(
          dynoverride_rule_dst);
      DynamicOverrideRuleIDData &rule_src = reinterpret_cast<DynamicOverrideRuleIDData &>(
          dynoverride_rule_src);
      rule_dst.runtime = MEM_new<RuleIDDataRuntime>(__func__);
      rule_dst.runtime->rule_srna_data = rule_src.runtime->rule_srna_data;

      if ((flag & LIB_ID_CREATE_NO_USER_REFCOUNT) == 0) {
        id_us_plus(rule_dst.base.target_filter.target_id);
      }

      if (rule_src.override_values) {
        rule_dst.override_values = IDP_CopyProperty_ex(rule_src.override_values, flag);
      }
      if (rule_src.original_values) {
        rule_dst.original_values = IDP_CopyProperty_ex(rule_src.original_values, flag);
      }

      BLI_duplicatelist(&rule_dst.properties, &rule_src.properties);
      for (DynamicOverrideRuleProperty *
               property_dst =
                  static_cast<DynamicOverrideRuleProperty *>(rule_dst.properties.first),
              *property_src =
                  static_cast<DynamicOverrideRuleProperty *>(rule_src.properties.first);
           property_dst;
           property_dst = property_dst->next, property_src = property_src->next)
      {
        rule_property_copy(*property_dst, *property_src, flag);
      }
      break;
    }
    default:
      BLI_assert_unreachable();
  }
}

static void rule_property_free(DynamicOverrideRuleProperty &dynoverride_rule_property)
{
  MEM_delete(dynoverride_rule_property.rna_path);
}

static void rule_free(DynamicOverrideRule &dynoverride_rule)
{
  if (dynoverride_rule.name) {
    MEM_delete(dynoverride_rule.name);
  }

  switch (dynoverride_rule.type) {
    case DynamicOverrideRuleType::IDData: {
      DynamicOverrideRuleIDData &rule = reinterpret_cast<DynamicOverrideRuleIDData &>(
          dynoverride_rule);

      for (DynamicOverrideRuleProperty &property : rule.properties) {
        rule_property_free(property);
      }
      BLI_freelistN(&rule.properties);

      if (rule.override_values) {
        IDP_FreeProperty(rule.override_values);
      }
      if (rule.original_values) {
        IDP_FreeProperty(rule.original_values);
      }

      MEM_delete(rule.runtime);
      break;
    }
    default:
      break;
  }
}

static void rule_foreach_id(DynamicOverrideRule &dynoverride_rule, LibraryForeachIDData &data)
{
  switch (dynoverride_rule.type) {
    case DynamicOverrideRuleType::IDData: {
      DynamicOverrideRuleIDData &rule = reinterpret_cast<DynamicOverrideRuleIDData &>(
          dynoverride_rule);

      /* Target IDs are not hard dependencies, they should not be linked only because a dynamic
       * override ID references them.
       * These usages are also not refcounting. */
      BKE_LIB_FOREACHID_PROCESS_ID(
          &data, rule.base.target_filter.target_id, IDWALK_CB_DIRECT_WEAK_LINK);

      IDP_foreach_property(rule.override_values, IDP_TYPE_FILTER_ID, [&](IDProperty *prop) {
        BKE_lib_query_idpropertiesForeachIDLink_callback(prop, &data);
      });
      IDP_foreach_property(rule.original_values, IDP_TYPE_FILTER_ID, [&](IDProperty *prop) {
        BKE_lib_query_idpropertiesForeachIDLink_callback(prop, &data);
      });
      break;
    }
    default:
      break;
  }
}

static void rule_property_write(BlendWriter &writer,
                                DynamicOverrideRuleProperty &dynoverride_rule_property)
{
  writer.write_struct(&dynoverride_rule_property);

  writer.write_string(dynoverride_rule_property.rna_path);
}

static void rule_write(BlendWriter &writer, DynamicOverrideRule &dynoverride_rule)
{
  writer.write_string(dynoverride_rule.name);

  switch (dynoverride_rule.type) {
    case DynamicOverrideRuleType::IDData: {
      DynamicOverrideRuleIDData &rule = reinterpret_cast<DynamicOverrideRuleIDData &>(
          dynoverride_rule);

      writer.write_struct(&rule, [](BlendStructWriter &struct_writer) -> void {
        struct_writer.runtime_ptr(offsetof(DynamicOverrideRuleIDData, runtime));
      });

      if (rule.override_values) {
        IDP_BlendWrite(&writer, rule.override_values);
      }
      if (rule.original_values) {
        IDP_BlendWrite(&writer, rule.original_values);
      }

      for (DynamicOverrideRuleProperty &property : rule.properties) {
        rule_property_write(writer, property);
      }
      break;
    }
    default:
      writer.write_struct(&dynoverride_rule);
      break;
  }
}

static void rule_property_read_data(BlendDataReader &reader,
                                    DynamicOverrideRuleProperty &dynoverride_rule_property)
{
  BLO_read_string(&reader, &dynoverride_rule_property.rna_path);
}

static void rule_read_data(BlendDataReader &reader, DynamicOverrideRule &dynoverride_rule)
{
  BLO_read_string(&reader, &dynoverride_rule.name);

  switch (dynoverride_rule.type) {
    case DynamicOverrideRuleType::IDData: {
      DynamicOverrideRuleIDData &rule = reinterpret_cast<DynamicOverrideRuleIDData &>(
          dynoverride_rule);
      rule.runtime = MEM_new<RuleIDDataRuntime>(__func__);

      BLO_read_struct(&reader, IDProperty, &rule.override_values);
      IDP_BlendDataRead(&reader, &rule.override_values);
      BLO_read_struct(&reader, IDProperty, &rule.original_values);
      IDP_BlendDataRead(&reader, &rule.original_values);

      BLO_read_struct_list(&reader, DynamicOverrideRuleProperty, &rule.properties);
      for (DynamicOverrideRuleProperty &property : rule.properties) {
        rule_property_read_data(reader, property);
      }
      break;
    }
    default:
      break;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Basic Rule Management.
 * \{ */

DynamicOverrideRuleIDData *rule_iddata_lookup_for_id(DynamicOverride &dynamic_override,
                                                     ID &owner_id)
{
  /* TODO: use runtime data for mappings etc. */
  for (DynamicOverrideRule &rule : dynamic_override.rules) {
    if (rule.type != DynamicOverrideRuleType::IDData) {
      continue;
    }
    DynamicOverrideRuleIDData &rule_id_data = reinterpret_cast<DynamicOverrideRuleIDData &>(rule);
    if (rule_id_data.base.target_filter.target_id == &owner_id) {
      return &rule_id_data;
    }
  }

  return nullptr;
}

DynamicOverrideRuleIDData *rule_iddata_lookup_for_id(Scene &scene,
                                                     ID &owner_id,
                                                     DynamicOverride **r_dynamic_override)
{
  DynamicOverride *dynamic_override = scene.dynamic_override;
  if (!dynamic_override) {
    return nullptr;
  }

  if (r_dynamic_override) {
    *r_dynamic_override = dynamic_override;
  }
  return rule_iddata_lookup_for_id(*dynamic_override, owner_id);
}

static DynamicOverrideRuleIDData &rule_iddata_add_for_id(Main &bmain,
                                                         DynamicOverride &dynamic_override,
                                                         ID &target_id)
{
  BLI_assert(!rule_iddata_lookup_for_id(dynamic_override, target_id));

  DynamicOverrideRuleIDData *rule_id_data = MEM_new<DynamicOverrideRuleIDData>(__func__);
  rule_id_data->runtime = MEM_new<bke::dynoverride::RuleIDDataRuntime>(__func__);
  rule_id_data->base.type = DynamicOverrideRuleType::IDData;
  rule_id_data->base.target_filter.type = DynamicOverrideRuleTargetFilterType::IDSingle;
  rule_id_data->base.target_filter.target_id = &target_id;

  /* Generate a unique name for this new rule. */
  const char *id_name = BKE_id_name(target_id);
  const IDTypeInfo *idtype = BKE_idtype_get_info_from_id(&target_id);
  std::string rule_name = fmt::format(
      fmt::runtime(CTX_DATA_(BLT_I18NCONTEXT_ID_DYNAMIC_OVERRIDE, "{} {}{} - Properties")),
      id_name,
      ID_IS_LINKED(&target_id) ? DATA_("linked ") : "",
      DATA_(idtype->name));
  rule_name_set(dynamic_override, rule_id_data->base, rule_name);

  BLI_addtail(&dynamic_override.rules, rule_id_data);

  DEG_id_tag_update(&dynamic_override.id, ID_RECALC_PARAMETERS);
  DEG_id_tag_update(&target_id, ID_RECALC_DYNAMIC_OVERRIDE);
  DEG_relations_tag_update(&bmain);

  return *rule_id_data;
}

DynamicOverrideRuleIDData &rule_iddata_ensure_for_id(Main &bmain,
                                                     DynamicOverride &dynamic_override,
                                                     ID &owner_id)
{
  DynamicOverrideRuleIDData *existing_rule = rule_iddata_lookup_for_id(dynamic_override, owner_id);

  if (existing_rule) {
    return *existing_rule;
  }
  return rule_iddata_add_for_id(bmain, dynamic_override, owner_id);
}

/**
 * Return a unique rule name whithin the given DynamicOverride ID, based on given initial
 * rule_name.
 */
static std::string rule_unique_name_get(DynamicOverride &dynamic_override,
                                        DynamicOverrideRule &target_rule,
                                        StringRef rule_name)
{
  Set<StringRef> rules_names;
  for (DynamicOverrideRule &rule : dynamic_override.rules) {
    if (&rule == &target_rule) {
      continue;
    }
    rules_names.add(rule.name);
  }
  return BLI_uniquename_cb(
      [&rules_names](const StringRef check_name) { return rules_names.contains(check_name); },
      '.',
      rule_name);
}

DynamicOverrideRule *rule_lookup_by_name(DynamicOverride &dynamic_override, StringRef rule_name)
{
  /* TODO: use runtime data for mappings etc. */
  for (DynamicOverrideRule &rule : dynamic_override.rules) {
    if (rule_name == rule.name) {
      return &rule;
    }
  }

  return nullptr;
}

void rule_name_set(DynamicOverride &dynamic_override,
                   DynamicOverrideRule &rule,
                   StringRef rule_name)
{
  MEM_SAFE_DELETE(rule.name);
  std::string rule_name_final = BLI_str_escape(rule_name);
  rule.name = BLI_strdup(rule_unique_name_get(dynamic_override, rule, rule_name_final).c_str());
}

void rule_remove(Main &bmain,
                 DynamicOverride &dynamic_override,
                 DynamicOverrideRule *existing_rule)
{
  BLI_assert(BLI_findindex(&dynamic_override.rules, existing_rule) != -1);
  BLI_remlink(&dynamic_override.rules, existing_rule);

  DEG_id_tag_update(&dynamic_override.id, ID_RECALC_PARAMETERS);
  DEG_id_tag_update(existing_rule->target_filter.target_id, ID_RECALC_DYNAMIC_OVERRIDE);
  DEG_relations_tag_update(&bmain);

  rule_free(*existing_rule);
  MEM_delete(existing_rule);
}

void rule_remove_for_id(Main &bmain, DynamicOverride &dynamic_override, ID &owner_id)
{
  DynamicOverrideRuleIDData *existing_rule = rule_iddata_lookup_for_id(dynamic_override, owner_id);

  if (existing_rule) {
    rule_remove(bmain, dynamic_override, &existing_rule->base);
  }
}

DynamicOverrideRuleProperty *rule_rna_property_add(Main &bmain,
                                                   DynamicOverride &dynamic_override,
                                                   DynamicOverrideRule &rule,
                                                   RNAPath &rna_path)
{
  if (rule.type != DynamicOverrideRuleType::IDData) {
    return nullptr;
  }

  DynamicOverrideRuleIDData &rule_iddata = reinterpret_cast<DynamicOverrideRuleIDData &>(rule);
  PointerRNA target_id_ptr, target_data_ptr;
  PropertyRNA *target_prop;

  target_id_ptr = RNA_id_pointer_create(rule_iddata.base.target_filter.target_id);
  RNA_path_resolve(&target_id_ptr, rna_path.path.c_str(), &target_data_ptr, &target_prop);

  if (!target_data_ptr.data || !target_prop) {
    return nullptr;
  }

  BLI_assert(rule_rna_property_lookup(rule, rna_path) == nullptr);
  DynamicOverrideRuleProperty *rule_property = MEM_new<DynamicOverrideRuleProperty>(__func__);
  rule_property->rna_path = BLI_strdup(rna_path.path.c_str());
  /* Sub-item data (e.g. collection item names or array indices) are not supported currently for
   * dynamic overrides. */
  BLI_assert(!rna_path.key);
  BLI_assert(!rna_path.index);
  BLI_addtail(&rule_iddata.properties, rule_property);

  BKE_main_ensure_invariants(bmain, dynamic_override.id);

  std::string prop_identifier = rule_property_rna_identifier(*rule_property);

  PointerRNA override_data_ptr = RNA_pointer_create_id_subdata(
      dynamic_override.id, RNA_DynamicOverrideRuleIDDataOverrideValues, &rule_iddata);
  PropertyRNA *override_rna_prop = RNA_struct_find_property(&override_data_ptr,
                                                            prop_identifier.c_str());

  if (!override_data_ptr.data || !override_rna_prop) {
    BLI_assert_unreachable();
    return nullptr;
  }

  PointerRNA original_data_ptr = RNA_pointer_create_id_subdata(
      dynamic_override.id, RNA_DynamicOverrideRuleIDDataOriginalValues, &rule_iddata);
  PropertyRNA *original_rna_prop = RNA_struct_find_property(&original_data_ptr,
                                                            prop_identifier.c_str());

  if (!original_data_ptr.data || !original_rna_prop) {
    BLI_assert_unreachable();
    return nullptr;
  }

  RNA_property_copy(nullptr, override_data_ptr, target_data_ptr, override_rna_prop, target_prop);
  RNA_property_copy(nullptr, original_data_ptr, target_data_ptr, original_rna_prop, target_prop);

  DEG_id_tag_update(&dynamic_override.id, ID_RECALC_PARAMETERS);
  DEG_id_tag_update(rule.target_filter.target_id, ID_RECALC_DYNAMIC_OVERRIDE);
  DEG_relations_tag_update(&bmain);

  return rule_property;
}

DynamicOverrideRuleProperty *rule_rna_property_lookup(DynamicOverrideRule &rule, RNAPath &rna_path)
{
  if (rule.type != DynamicOverrideRuleType::IDData) {
    return nullptr;
  }

  DynamicOverrideRuleIDData &rule_iddata = reinterpret_cast<DynamicOverrideRuleIDData &>(rule);

  DynamicOverrideRuleProperty *rule_property = static_cast<DynamicOverrideRuleProperty *>(
      BLI_findstring_ptr(&rule_iddata.properties,
                         rna_path.path.c_str(),
                         offsetof(DynamicOverrideRuleProperty, rna_path)));

  return rule_property;
}

void rule_property_remove(Main &bmain,
                          DynamicOverride &dynamic_override,
                          DynamicOverrideRule &rule,
                          DynamicOverrideRuleProperty *existing_property)
{
  BLI_assert(rule.type == DynamicOverrideRuleType::IDData);

  DynamicOverrideRuleIDData &rule_iddata = reinterpret_cast<DynamicOverrideRuleIDData &>(rule);
  BLI_assert(BLI_findindex(&rule_iddata.properties, existing_property) != -1);
  BLI_remlink(&rule_iddata.properties, existing_property);
  rule_property_free(*existing_property);
  MEM_delete(existing_property);

  DEG_id_tag_update(&dynamic_override.id, ID_RECALC_PARAMETERS);
  DEG_id_tag_update(rule.target_filter.target_id, ID_RECALC_DYNAMIC_OVERRIDE);
  DEG_relations_tag_update(&bmain);

  BKE_main_ensure_invariants(bmain, dynamic_override.id);
}

void rule_rna_property_reset(DynamicOverride &dynamic_override,
                             DynamicOverrideRule &rule,
                             DynamicOverrideRuleProperty &rule_property)
{
  if (rule.type != DynamicOverrideRuleType::IDData) {
    return;
  }

  DynamicOverrideRuleIDData &rule_iddata = reinterpret_cast<DynamicOverrideRuleIDData &>(rule);

  std::string prop_identifier = rule_property_rna_identifier(rule_property);

  PointerRNA override_data_ptr = RNA_pointer_create_id_subdata(
      dynamic_override.id, RNA_DynamicOverrideRuleIDDataOverrideValues, &rule_iddata);
  PropertyRNA *override_rna_prop = RNA_struct_find_property(&override_data_ptr,
                                                            prop_identifier.c_str());

  if (!override_data_ptr.data || !override_rna_prop) {
    BLI_assert_unreachable();
    return;
  }

  PointerRNA original_data_ptr = RNA_pointer_create_id_subdata(
      dynamic_override.id, RNA_DynamicOverrideRuleIDDataOriginalValues, &rule_iddata);
  PropertyRNA *original_rna_prop = RNA_struct_find_property(&original_data_ptr,
                                                            prop_identifier.c_str());

  if (!original_data_ptr.data || !original_rna_prop) {
    BLI_assert_unreachable();
    return;
  }

  RNA_property_copy(
      nullptr, override_data_ptr, original_data_ptr, override_rna_prop, original_rna_prop);
}

void rule_rna_property_reset(DynamicOverride &dynamic_override,
                             DynamicOverrideRule &rule,
                             RNAPath &rna_path)
{
  DynamicOverrideRuleProperty *rule_property = rule_rna_property_lookup(rule, rna_path);
  if (rule_property) {
    rule_rna_property_reset(dynamic_override, rule, *rule_property);
  }
}

void rule_rna_property_apply_to_target(DynamicOverride &dynamic_override,
                                       DynamicOverrideRule &rule,
                                       DynamicOverrideRuleProperty &rule_property)
{
  if (rule.type != DynamicOverrideRuleType::IDData) {
    return;
  }

  DynamicOverrideRuleIDData &rule_iddata = reinterpret_cast<DynamicOverrideRuleIDData &>(rule);
  PointerRNA target_id_ptr, target_data_ptr;
  PropertyRNA *target_prop;

  target_id_ptr = RNA_id_pointer_create(rule_iddata.base.target_filter.target_id);
  RNA_path_resolve(&target_id_ptr, rule_property.rna_path, &target_data_ptr, &target_prop);

  if (!target_data_ptr.data || !target_prop) {
    return;
  }

  std::string prop_identifier = rule_property_rna_identifier(rule_property);

  PointerRNA override_data_ptr = RNA_pointer_create_id_subdata(
      dynamic_override.id, RNA_DynamicOverrideRuleIDDataOverrideValues, &rule_iddata);
  PropertyRNA *override_rna_prop = RNA_struct_find_property(&override_data_ptr,
                                                            prop_identifier.c_str());

  if (!override_data_ptr.data || !override_rna_prop) {
    BLI_assert_unreachable();
    return;
  }

  RNA_property_copy(nullptr, target_data_ptr, override_data_ptr, target_prop, override_rna_prop);
}

void rule_rna_property_apply_to_target(DynamicOverride &dynamic_override,
                                       DynamicOverrideRule &rule,
                                       RNAPath &rna_path)
{
  DynamicOverrideRuleProperty *rule_property = rule_rna_property_lookup(rule, rna_path);
  if (rule_property) {
    rule_rna_property_apply_to_target(dynamic_override, rule, *rule_property);
  }
}

void rule_rna_property_update_from_target(DynamicOverride &dynamic_override,
                                          DynamicOverrideRule &rule,
                                          DynamicOverrideRuleProperty &rule_property)
{
  if (rule.type != DynamicOverrideRuleType::IDData) {
    return;
  }

  DynamicOverrideRuleIDData &rule_iddata = reinterpret_cast<DynamicOverrideRuleIDData &>(rule);
  PointerRNA target_id_ptr, target_data_ptr;
  PropertyRNA *target_prop;

  target_id_ptr = RNA_id_pointer_create(rule_iddata.base.target_filter.target_id);
  RNA_path_resolve(&target_id_ptr, rule_property.rna_path, &target_data_ptr, &target_prop);

  if (!target_data_ptr.data || !target_prop) {
    return;
  }

  std::string prop_identifier = rule_property_rna_identifier(rule_property);

  PointerRNA override_data_ptr = RNA_pointer_create_id_subdata(
      dynamic_override.id, RNA_DynamicOverrideRuleIDDataOverrideValues, &rule_iddata);
  PropertyRNA *override_rna_prop = RNA_struct_find_property(&override_data_ptr,
                                                            prop_identifier.c_str());

  if (!override_data_ptr.data || !override_rna_prop) {
    BLI_assert_unreachable();
    return;
  }

  RNA_property_copy(nullptr, override_data_ptr, target_data_ptr, override_rna_prop, target_prop);
}

void rule_rna_property_update_from_target(DynamicOverride &dynamic_override,
                                          DynamicOverrideRule &rule,
                                          RNAPath &rna_path)
{
  DynamicOverrideRuleProperty *rule_property = rule_rna_property_lookup(rule, rna_path);
  if (rule_property) {
    rule_rna_property_update_from_target(dynamic_override, rule, *rule_property);
  }
}

bool rule_rna_property_path_set(DynamicOverrideRule &rule,
                                DynamicOverrideRuleProperty &rule_property,
                                StringRef rna_path)
{
  if (rule.type != DynamicOverrideRuleType::IDData) {
    return false;
  }
  DynamicOverrideRuleIDData &rule_iddata = reinterpret_cast<DynamicOverrideRuleIDData &>(rule);
  for (DynamicOverrideRuleProperty &rule_prop : rule_iddata.properties) {
    if (rule_prop.rna_path == rna_path) {
      return false;
    }
  }

  std::string old_identifier = rule_property_rna_identifier(rule_property);
  MEM_SAFE_DELETE(rule_property.rna_path);
  rule_property.rna_path = BLI_strdupn(rna_path.data(), size_t(rna_path.size()));
  std::string new_identifier = rule_property_rna_identifier(rule_property);

  auto rename_idproperty = [&old_identifier, &new_identifier](IDProperty *group_container) {
    BLI_assert(group_container->type == IDP_GROUP);
    if (!group_container) {
      return;
    }
    if (IDP_GetPropertyFromGroup(group_container, new_identifier.c_str())) {
      /* Do not overwrite another existing property. Should never happen in practice though. */
      BLI_assert_unreachable();
      return;
    }
    IDProperty *id_prop = IDP_GetPropertyFromGroup(group_container, old_identifier.c_str());
    IDP_RemoveFromGroup(group_container, id_prop);
    STRNCPY(id_prop->name, new_identifier.c_str());
    const bool success = IDP_AddToGroup(group_container, id_prop);
    BLI_assert(success);
    UNUSED_VARS_NDEBUG(success);
  };
  rename_idproperty(rule_iddata.override_values);
  rename_idproperty(rule_iddata.original_values);

  return true;
}

std::string rule_property_rna_identifier(const DynamicOverrideRuleProperty &rule_property)
{
  std::string safe_property_name = rule_property.rna_path ? rule_property.rna_path : "";
  RNA_identifier_sanitize(safe_property_name, true);
  return safe_property_name;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Various utils.
 * \{ */

/* Path Validation -------------------------------------------- */

/**
 * Replace `old_infix` with `new_infix` which has to occur immediately after `prefix`.
 *
 * \param verify_paths: If true, the paths will be resolved, and only if it *doesn't* resolve to a
 * property before, but *does* resolve after, is the new path accepted.
 */
static std::optional<std::string> rna_path_rename_fix(ID &target_id,
                                                      const StringRef prefix,
                                                      const StringRef old_infix,
                                                      const StringRef new_infix,
                                                      const StringRefNull old_path,
                                                      const bool verify_paths)
{
  /* Make initial RNA pointer to start resolving from */
  PointerRNA target_id_ptr = verify_paths ? RNA_id_pointer_create(&target_id) : PointerRNA();
  auto rna_path_is_valid = [&target_id_ptr](StringRefNull rna_path) -> bool {
    PointerRNA ptr;
    PropertyRNA *prop = nullptr;
    return RNA_path_resolve_property(&target_id_ptr, rna_path.c_str(), &ptr, &prop);
  };

  const int64_t prefix_offset = old_path.find(prefix);
  if (prefix_offset == StringRefBase::not_found) {
    return std::nullopt;
  }

  const int64_t old_infix_offset = old_path.find(old_infix);
  if (old_infix_offset == StringRefBase::not_found) {
    return std::nullopt;
  }

  /* Only modify paths if the prefix and old_name feature in the path,
   * and prefix occurs immediately before old_name. */
  if (prefix_offset + prefix.size() != old_infix_offset) {
    return std::nullopt;
  }

  /* Only modify a path that is invalid. */
  if (verify_paths && rna_path_is_valid(old_path)) {
    return std::nullopt;
  }

  std::string modified_path;
  const int64_t postfix_offset = old_infix_offset + old_infix.size();

  /* Add the part of the string that goes up to the start of the prefix. */
  if (prefix_offset > 0) {
    modified_path.append(old_path.substr(0, prefix_offset));
  }

  modified_path.append(prefix);
  modified_path.append(new_infix);
  modified_path.append(old_path.substr(postfix_offset));

  /* If the new path is not valid, the renaming failed. */
  if (verify_paths && !rna_path_is_valid(modified_path)) {
    return std::nullopt;
  }

  return modified_path;
}

void fix_rna_paths_rename(Main &bmain,
                          DynamicOverride &dynamic_override,
                          ID &target_id,
                          StringRef prefix,
                          StringRefNull old_infix,
                          StringRefNull new_infix,
                          int old_subscript,
                          int new_subscript,
                          bool verify_paths,
                          bool infix_is_name)
{
  auto &&[old_key, new_key] = RNA_generate_keys_for_path_rename(
      old_infix, new_infix, old_subscript, new_subscript, infix_is_name);

  bool is_changed = false;

  for (DynamicOverrideRule &rule : dynamic_override.rules) {
    if (rule.type != DynamicOverrideRuleType::IDData) {
      continue;
    }
    if (rule.target_filter.type != DynamicOverrideRuleTargetFilterType::IDSingle) {
      continue;
    }
    if (rule.target_filter.target_id != &target_id) {
      continue;
    }
    DynamicOverrideRuleIDData &rule_iddata = reinterpret_cast<DynamicOverrideRuleIDData &>(rule);
    for (DynamicOverrideRuleProperty &rule_prop : rule_iddata.properties) {
      std::optional<std::string> new_path = rna_path_rename_fix(
          target_id, prefix, old_key, new_key, rule_prop.rna_path, verify_paths);
      if (!new_path.has_value()) {
        continue;
      }
      if (rule_rna_property_path_set(rule, rule_prop, *new_path)) {
        is_changed = true;
      }
    }
  }

  /* Tag dynoverride and target ID if needed. */
  if (is_changed) {
    DEG_id_tag_update(&target_id, ID_RECALC_DYNAMIC_OVERRIDE);
    DEG_id_tag_update(&dynamic_override.id, ID_RECALC_SYNC_TO_EVAL);
    BKE_main_ensure_invariants(bmain, dynamic_override.id);
  }
}

void fix_rna_paths_rename(Main &bmain,
                          ID &target_id,
                          StringRef prefix,
                          StringRefNull old_infix,
                          StringRefNull new_infix,
                          int old_subscript,
                          int new_subscript,
                          bool verify_paths,
                          bool infix_is_name)
{
  for (DynamicOverride &dynamic_override : bmain.dynamic_overrides) {
    fix_rna_paths_rename(bmain,
                         dynamic_override,
                         target_id,
                         prefix,
                         old_infix,
                         new_infix,
                         old_subscript,
                         new_subscript,
                         verify_paths,
                         infix_is_name);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Invariants and runtime data updates.
 * \{ */

/**
 * Contains a runtime registration of RNA types generated based on the DynamicOverrideRule's
 * current properties. These RNA types are not registered in the global list, but owned by the rule
 * so that RNA access to the properties works as long as the rule exists.
 */
struct GeneratedRuleSrnaData {
  ResourceScope scope;
  BlenderRNA *generated_rna;
  /** SRNA for the properties for override values. */
  StructRNA *override_values_struct;
  /**
   * SRNA for the properties for original values (values from target property at the time the
   * override was created).
   */
  StructRNA *original_values_struct;

  GeneratedRuleSrnaData()
  {
    generated_rna = RNA_create_runtime();
  }
  ~GeneratedRuleSrnaData()
  {
    RNA_free(generated_rna);
  }
};

static PropertyRNA *dynamic_override_copy_rna_property_definition(
    std::unique_ptr<GeneratedRuleSrnaData> &generated,
    StructRNA *srna,
    PointerRNA &ptr_orig,
    PropertyRNA *prop_orig,
    const StringRefNull property_identifier)
{
  const PropertyType prop_type = RNA_property_type(prop_orig);
  StructRNA *prop_ptr_type = RNA_property_pointer_type(&ptr_orig, prop_orig);

  const bool is_array = RNA_property_array_check(prop_orig);
  int array_dimension = 0;
  int array_tot_length = 0;
  int array_lengths[RNA_MAX_ARRAY_DIMENSION] = {0};

  PropertyRNA *prop = RNA_def_property(
      srna, property_identifier.c_str(), prop_type, RNA_property_subtype(prop_orig));

  /* TODO: Likely need more care here (clear some flags, etc.). */
  PropertyFlag prop_flag = PropertyFlag(RNA_property_flag(prop_orig));
  /* TODO: 'context update' properties will not work well with generic liboverrides Main-based
   * update callback. Not clear currently if:
   *   - These type of properties should be supported at all by dynoverride?
   *   - DynOverride should be able to generate a context update callback for these?
   *   - Something else?
   */
  prop_flag &= ~PropertyFlag(PROP_CONTEXT_UPDATE | PROP_CONTEXT_PROPERTY_UPDATE);
  RNA_def_property_flag(prop, prop_flag);
  RNA_def_property_override_flag(prop,
                                 PropertyOverrideFlag(RNA_property_override_flag(prop_orig)));
  if (prop_ptr_type != RNA_UnknownType) {
    RNA_def_property_struct_runtime(srna, prop, prop_ptr_type);
  }

  if (is_array) {
    array_tot_length = RNA_property_array_length(&ptr_orig, prop_orig);
    array_dimension = RNA_property_array_dimension(&ptr_orig, prop_orig, array_lengths);
    BLI_assert(array_dimension <= RNA_MAX_ARRAY_DIMENSION);
    RNA_def_property_multi_array(prop, array_dimension, array_lengths);
  }

  switch (prop_type) {
    case PROP_BOOLEAN: {
      if (is_array) {
        MutableSpan<bool> array_default = generated->scope.allocator().allocate_array<bool>(
            array_tot_length);
        RNA_property_boolean_get_default_array(&ptr_orig, prop_orig, array_default.data());
        RNA_def_property_boolean_array_default(prop, array_default.data());
      }
      else {
        RNA_def_property_boolean_default(prop,
                                         RNA_property_boolean_get_default(&ptr_orig, prop_orig));
      }
      break;
    }
    case PROP_INT: {
      int min, max;
      RNA_property_int_range(&ptr_orig, prop_orig, &min, &max);
      RNA_def_property_range(prop, double(min), double(max));

      int softmin, softmax, step;
      RNA_property_int_ui_range(&ptr_orig, prop_orig, &softmin, &softmax, &step);
      RNA_def_property_ui_range(prop, double(softmin), double(softmax), double(step), -1);

      if (is_array) {
        MutableSpan<int> array_default = generated->scope.allocator().allocate_array<int>(
            array_tot_length);
        RNA_property_int_get_default_array(&ptr_orig, prop_orig, array_default.data());
        RNA_def_property_int_array_default(prop, array_default.data());
      }
      else {
        RNA_def_property_int_default(prop, RNA_property_int_get_default(&ptr_orig, prop_orig));
      }
      break;
    }
    case PROP_FLOAT: {
      float min, max;
      RNA_property_float_range(&ptr_orig, prop_orig, &min, &max);
      RNA_def_property_range(prop, double(min), double(max));

      float softmin, softmax, step, precision;
      RNA_property_float_ui_range(&ptr_orig, prop_orig, &softmin, &softmax, &step, &precision);
      RNA_def_property_ui_range(
          prop, double(softmin), double(softmax), double(step), int(precision));

      if (is_array) {
        MutableSpan<float> array_default = generated->scope.allocator().allocate_array<float>(
            array_tot_length);
        RNA_property_float_get_default_array(&ptr_orig, prop_orig, array_default.data());
        RNA_def_property_float_array_default(prop, array_default.data());
      }
      else {
        RNA_def_property_float_default(prop, RNA_property_float_get_default(&ptr_orig, prop_orig));
      }
      break;
    }
    case PROP_ENUM: {
      const EnumPropertyItem *items;
      int tot_items;
      bool items_free;
      RNA_property_enum_items(nullptr, &ptr_orig, prop_orig, &items, &tot_items, &items_free);

      const EnumPropertyItem *items_iter = items;
      MutableSpan<EnumPropertyItem> array_items =
          generated->scope.allocator().allocate_array<EnumPropertyItem>(tot_items + 1);
      for (EnumPropertyItem &item : array_items) {
        item = *items_iter;
        items_iter++;
      }
      if (items_free) {
        MEM_delete(items);
      }

      RNA_def_property_enum_items(prop, array_items.data());

      RNA_def_property_enum_default(prop, RNA_property_enum_get_default(&ptr_orig, prop_orig));
      break;
    }
    case PROP_STRING:
    case PROP_POINTER:
    case PROP_COLLECTION:
      break;
  }

  const StringRefNull ui_name = generated->scope.allocator().copy_string(
      RNA_property_ui_name_raw(prop_orig, &ptr_orig));
  const StringRefNull ui_description = generated->scope.allocator().copy_string(
      RNA_property_ui_description_raw(prop_orig, &ptr_orig));
  RNA_def_property_ui_text(prop, ui_name.c_str(), ui_description.c_str());

  /* TODO generic update function may not work well in all cases. */
  RNA_def_property_update_runtime(prop, [](Main * /*bmain*/, Scene * /*scene*/, PointerRNA *ptr) {
    DynamicOverrideRule *rule = ptr->data_as<DynamicOverrideRule>();
    BLI_assert(rule->type == DynamicOverrideRuleType::IDData);

    DEG_id_tag_update(rule->target_filter.target_id, ID_RECALC_DYNAMIC_OVERRIDE);
  });
  RNA_def_property_update_notifier(prop, NC_ID | NA_EDITED);

  return prop;
}

static void dynamic_override_update_rules_srna(Main & /*bmain*/, DynamicOverride &dynamic_override)
{
  for (DynamicOverrideRule &rule : dynamic_override.rules) {
    if (rule.type != DynamicOverrideRuleType::IDData) {
      continue;
    }
    if (!rule.target_filter.target_id) {
      continue;
    }

    DynamicOverrideRuleIDData &iddata_rule = reinterpret_cast<DynamicOverrideRuleIDData &>(rule);
    auto generated = std::make_unique<GeneratedRuleSrnaData>();
    generated->override_values_struct = RNA_def_struct_ptr(
        generated->generated_rna,
        "DynamicOverrideRuleIDDataOverrideValuesRT",
        RNA_DynamicOverrideRuleIDDataOverrideValues);
    generated->original_values_struct = RNA_def_struct_ptr(
        generated->generated_rna,
        "DynamicOverrideRuleIDDataOriginalValuesRT",
        RNA_DynamicOverrideRuleIDDataOriginalValues);

    PointerRNA owner_id_ptr = RNA_id_pointer_create(rule.target_filter.target_id);
    for (DynamicOverrideRuleProperty &rule_property : iddata_rule.properties) {
      PointerRNA ptr;
      PropertyRNA *prop_orig;

      RNA_path_resolve(&owner_id_ptr, rule_property.rna_path, &ptr, &prop_orig);

      if (!ptr.data || !prop_orig) {
        continue;
      }

      const StringRefNull property_identifier = generated->scope.allocator().copy_string(
          rule_property_rna_identifier(rule_property));
      dynamic_override_copy_rna_property_definition(
          generated, generated->override_values_struct, ptr, prop_orig, property_identifier);
      /* The original values are read-only. */
      dynamic_override_copy_rna_property_definition(
          generated, generated->original_values_struct, ptr, prop_orig, property_identifier);
    }

    iddata_rule.runtime->rule_srna_data = std::move(generated);
  }
}

static void dynamic_override_update_rules_system_idprops(Main &bmain,
                                                         DynamicOverride &dynamic_override)
{
  for (DynamicOverrideRule &rule : dynamic_override.rules) {
    if (rule.type != DynamicOverrideRuleType::IDData) {
      continue;
    }
    if (!rule.target_filter.target_id) {
      continue;
    }

    DynamicOverrideRuleIDData &iddata_rule = reinterpret_cast<DynamicOverrideRuleIDData &>(rule);
    StructRNA *srna = iddata_rule.runtime->rule_srna_data->override_values_struct;
    if (iddata_rule.override_values) {
      PointerRNA properties_ptr = RNA_pointer_create_discrete(&dynamic_override.id, srna, &rule);
      RNA_sync_system_properties(bmain, properties_ptr, *iddata_rule.override_values);
    }

    DEG_id_tag_update(rule.target_filter.target_id, ID_RECALC_DYNAMIC_OVERRIDE);
  }
}

void update(Main &bmain, std::optional<Span<DynamicOverride *>> modified_dynamic_overrides)
{
  Vector<DynamicOverride *> changed_dynamic_overrides;
  if (!modified_dynamic_overrides.has_value()) {
    for (DynamicOverride &dynoverride : bmain.dynamic_overrides) {
      if (true) {
        changed_dynamic_overrides.append(&dynoverride);
      }
    }
    modified_dynamic_overrides = changed_dynamic_overrides;
  }

  for (DynamicOverride *dynamic_override : *modified_dynamic_overrides) {
    dynamic_override_update_rules_srna(bmain, *dynamic_override);
    dynamic_override_update_rules_system_idprops(bmain, *dynamic_override);
  }
}

StructRNA *rule_get_runtime_override_values_rna_struct(DynamicOverrideRuleIDData &iddata_rule)
{
  if (iddata_rule.runtime->rule_srna_data &&
      iddata_rule.runtime->rule_srna_data->override_values_struct)
  {
    return iddata_rule.runtime->rule_srna_data->override_values_struct;
  }
  return RNA_DynamicOverrideRuleIDDataOverrideValues;
}

StructRNA *rule_get_runtime_original_values_rna_struct(DynamicOverrideRuleIDData &iddata_rule)
{
  if (iddata_rule.runtime->rule_srna_data &&
      iddata_rule.runtime->rule_srna_data->original_values_struct)
  {
    return iddata_rule.runtime->rule_srna_data->original_values_struct;
  }
  return RNA_DynamicOverrideRuleIDDataOriginalValues;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Runtime/depgraph building & evaluation context.
 * \{ */

void DepsgraphCtx::gather_dynamic_overrides(const bool force_reset)
{
  if (force_reset) {
    dynamic_overrides_.clear();
    dynamic_overrides_are_gathered_ = false;
  }
  if (dynamic_overrides_are_gathered_) {
    return;
  }

  if (scene_->dynamic_override) {
    dynamic_overrides_.add_as(scene_->dynamic_override);
  }
  dynamic_overrides_are_gathered_ = true;
}

void DepsgraphCtx::gather_id_targets(const bool force_reset)
{
  if (force_reset) {
    id_targets_.clear();
    id_targets_are_gathered_ = false;
  }

  if (id_targets_are_gathered_) {
    return;
  }

  gather_dynamic_overrides(force_reset);
  for (DynamicOverride *dynoverride_iter : dynamic_overrides_) {
    for (const auto &&[rule_i, rule_iter] : dynoverride_iter->rules.enumerate()) {
      if (flag_is_set(rule_iter.flag, DynamicOverrideRuleFlag::IsMuted)) {
        continue;
      }
      if (rule_iter.type == DynamicOverrideRuleType::IDData) {
        const DynamicOverrideRuleIDData &id_rule =
            reinterpret_cast<const DynamicOverrideRuleIDData &>(rule_iter);
        if (id_rule.base.target_filter.target_id) {
          /* Dynamic overrides are not allowed to be overridden by other dynamic overrides!
           * NOTE: Once implemented, dynoverride imports will be a different case. */
          BLI_assert(GS(id_rule.base.target_filter.target_id->name) != ID_OV);
          id_targets_.add(id_rule.base.target_filter.target_id,
                          {dynoverride_iter, &rule_iter, rule_i});
        }
      }
    }
  }
  id_targets_are_gathered_ = true;
}

Span<RuleWithOwner> DepsgraphCtx::get_override_rules_for_id(ID &id) const
{
  BLI_assert(dynamic_overrides_are_gathered_ && id_targets_are_gathered_);
  return id_targets_.lookup(&id);
}

/** \} */

}  // namespace bke::dynoverride

}  // namespace blender
