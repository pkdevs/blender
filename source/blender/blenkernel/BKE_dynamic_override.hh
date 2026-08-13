/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 * SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

/** \file
 * \ingroup bke
 */

#include "BLI_multi_value_map.hh"
#include "BLI_span.hh"
#include "BLI_vector_set.hh"

#include <type_traits>
#include <variant>

namespace blender {

struct Depsgraph;
struct DynamicOverride;
struct DynamicOverrideRule;
struct DynamicOverrideRuleIDData;
struct DynamicOverrideRuleProperty;
struct ID;
struct Main;
struct ReportList;
struct RNAPath;
struct StructRNA;
struct Scene;
struct ViewLayer;

template<typename T> class Span;
namespace rna_path {
struct Member;
struct LookupIndex;
struct LookupKey;
using Item = std::variant<Member, LookupIndex, LookupKey>;
}  // namespace rna_path
using ParsedRNAPathRef = Span<rna_path::Item>;

namespace bke::dynoverride {

/* -------------------------------------------------------------------- */
/** \name Basic Rule Management.
 * \{ */

DynamicOverrideRuleIDData *rule_iddata_lookup_for_id(DynamicOverride &dynamic_override,
                                                     ID &id_owner);
DynamicOverrideRuleIDData *rule_iddata_lookup_for_id(
    Scene &scene, ID &owner_id, DynamicOverride **r_dynamic_override = nullptr);
DynamicOverrideRuleIDData &rule_iddata_ensure_for_id(Main &bmain,
                                                     DynamicOverride &dynamic_override,
                                                     ID &id_owner);

DynamicOverrideRule *rule_lookup_by_name(DynamicOverride &dynamic_override, StringRef rule_name);
void rule_name_set(DynamicOverride &dynamic_override,
                   DynamicOverrideRule &rule,
                   StringRef rule_name);
void rule_remove(Main &bmain,
                 DynamicOverride &dynamic_override,
                 DynamicOverrideRule *existing_rule);
void rule_remove_for_id(Main &bmain, DynamicOverride &dynamic_override, ID &owner_id);

DynamicOverrideRuleProperty *rule_rna_property_add(Main &bmain,
                                                   DynamicOverride &dynamic_override,
                                                   DynamicOverrideRule &rule,
                                                   RNAPath &rna_path);

DynamicOverrideRuleProperty *rule_rna_property_lookup(DynamicOverrideRule &rule,
                                                      RNAPath &rna_path);

void rule_property_remove(Main &bmain,
                          DynamicOverride &dynamic_override,
                          DynamicOverrideRule &rule,
                          DynamicOverrideRuleProperty *existing_property);

/** Reset override value to the original one. */
void rule_rna_property_reset(DynamicOverride &dynamic_override,
                             DynamicOverrideRule &rule,
                             DynamicOverrideRuleProperty &rule_property);
/** Reset override value to the original one. */
void rule_rna_property_reset(DynamicOverride &dynamic_override,
                             DynamicOverrideRule &rule,
                             RNAPath &rna_path);

/** Apply override value to the target data. */
void rule_rna_property_apply_to_target(DynamicOverride &dynamic_override,
                                       DynamicOverrideRule &rule,
                                       DynamicOverrideRuleProperty &rule_property);
/** Apply override value to the target data. */
void rule_rna_property_apply_to_target(DynamicOverride &dynamic_override,
                                       DynamicOverrideRule &rule,
                                       RNAPath &rna_path);

/** Update the override value from the target data. */
void rule_rna_property_update_from_target(DynamicOverride &dynamic_override,
                                          DynamicOverrideRule &rule,
                                          DynamicOverrideRuleProperty &rule_property);
/** Update the override value from the target data. */
void rule_rna_property_update_from_target(DynamicOverride &dynamic_override,
                                          DynamicOverrideRule &rule,
                                          RNAPath &rna_path);

/**
 * Set the rna path of the given `rule_property`.
 *
 * \return true on success, false otherwise (e.g. because given rna path is already used by another
 * property in the same dynoverride rule).
 */
bool rule_property_rna_path_set(DynamicOverrideRule &rule,
                                DynamicOverrideRuleProperty &rule_property,
                                StringRef rna_path);

/**
 * Retrieve a pre-parsed version of the rule property's RNA path, owned by the property itself. The
 * referenced memory is only valid as long as the rule property's path is not changed. Due to its
 * use of specialized RNA path storage types, this can be much more efficient to work with than the
 * string representation.
 * \note This may return empty when an invalid RNA path that cannot be parsed is set.
 */
ParsedRNAPathRef rule_property_rna_path_parsed(const blender::DynamicOverrideRuleProperty &rule_property);

/**
 * Return an identifier representing the RNA path of the property, that is usable as a RNA
 * property identifier.
 */
std::string rule_property_rna_identifier(const DynamicOverrideRuleProperty &rule_property);

/** \} */

/** \name Various utils.
 * \{ */

/**
 * Fix/update RNA paths, typically when renaming a sub-data item (a bone, modifier, ...) in the
 * given `target_id`.
 *
 * This version takes a a single dynamic_override ID, use the one taking a Main parameter instead
 * for global updates.
 */
void fix_rna_paths_rename(Main &bmain,
                          DynamicOverride &dynamic_override,
                          ID &target_id,
                          StringRef prefix,
                          StringRefNull old_infix,
                          StringRefNull new_infix,
                          int old_subscript,
                          int new_subscript,
                          bool verify_paths,
                          bool infix_is_name);

/**
 * Fix/update RNA paths, typically when renaming a sub-data item (a bone, modifier, ...) in the
 * given `target_id`.
 *
 * This version will update all dynamic overrides present in the given ID, that have rule(s)
 * affecting the given `target_id`.
 */
void fix_rna_paths_rename(Main &bmain,
                          ID &target_id,
                          StringRef prefix,
                          StringRefNull old_infix,
                          StringRefNull new_infix,
                          int old_subscript,
                          int new_subscript,
                          bool verify_paths,
                          bool infix_is_name);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Invariants and runtime data updates.
 * \{ */

void update(Main &bmain, std::optional<Span<DynamicOverride *>> modified_dynamic_overrides);

/** Return the runtime RNA struct for the override value storage for the given rule. */
StructRNA *rule_get_runtime_override_values_rna_struct(DynamicOverrideRuleIDData &iddata_rule);
/** Return the runtime RNA struct for the original value storage for the given rule. */
StructRNA *rule_get_runtime_original_values_rna_struct(DynamicOverrideRuleIDData &iddata_rule);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Runtime/depgraph building & evaluation context.
 * \{ */

struct RuleWithOwner {
  DynamicOverride *owner = nullptr;
  const DynamicOverrideRule *rule = nullptr;
  int rule_i = -1;
};

class DepsgraphCtx {
  Scene *scene_;
  ViewLayer *layer_;

  /**
   * All ID targets, i.e. all overridden IDs according to the active DynamicOverride
   * data-blocks in the evaluated scene & view layer, with all the DynamicOverrideRules affecting
   * them.
   */
  MultiValueMap<ID *, RuleWithOwner> id_targets_ = {};
  bool id_targets_are_gathered_ = false;

  /**
   * All active DynamicOverride ID targets, i.e. all overridden IDs according to the active
   * DynamicOverride data-blocks in the evaluated scene & view layer.
   */
  VectorSet<DynamicOverride *> dynamic_overrides_ = {};
  bool dynamic_overrides_are_gathered_ = false;

 public:
  DepsgraphCtx(Scene *scene, ViewLayer *layer) : scene_(scene), layer_(layer) {}
  virtual ~DepsgraphCtx() = default;

  /* ----------
   * Building/updating API.
   */

  /** Reset internal data, clear any cached/evaluated override info. */
  void reset_data(Scene *scene, ViewLayer *layer)
  {
    scene_ = scene;
    layer_ = layer;
    id_targets_.clear();
    id_targets_are_gathered_ = false;
    dynamic_overrides_.clear();
    dynamic_overrides_are_gathered_ = false;
  }

  /**
   * Gather all active DynamicOverride IDs for the current scene & view layer.
   */
  void gather_dynamic_overrides(bool force_reset);

  /**
   * Gather all affected (overridden) IDs from the list of active dynamic overrides.
   */
  void gather_id_targets(bool force_reset);

  /* ----------
   * Querying API.
   */

  /** Is there any dynamic overrides active in this this context. */
  bool has_overrides() const
  {
    BLI_assert(dynamic_overrides_are_gathered_);
    return !dynamic_overrides_.is_empty();
  }

  /**
   * Return the gathered list of rules affecting the given ID.
   */
  Span<RuleWithOwner> get_override_rules_for_id(ID &id) const;
};

/** \} */

}  // namespace bke::dynoverride

}  // namespace blender
