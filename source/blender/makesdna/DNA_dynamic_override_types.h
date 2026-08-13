/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
/** \file
 * \ingroup DNA
 */

#pragma once

#include "DNA_ID.h"
#include "DNA_listBase.h"

namespace blender {

struct IDProperty;
namespace bke::dynoverride {
struct Runtime;
struct RuleIDDataRuntime;
struct RulePropertyRuntime;
}  // namespace bke::dynoverride

/** Types of target filtering to select which data a given dynoverride rule applies to. */
enum class DynamicOverrideRuleTargetFilterType : int8_t {
  Unknown = 0,
  /** The rule applies to a single ID. */
  IDSingle = 1,
};

struct DynamicOverrideRuleTargetFilter {
  DynamicOverrideRuleTargetFilterType type = {};
  int8_t _pad[5] = {};

  /* Type-specific data. */

  /** Type of the `target_id`. */
  short id_type = ID_OB;

  /** For DynamicOverrideRuleTargetFilterType::IDSingle, the affected ID. */
  ID *target_id = nullptr;
};

enum class DynamicOverrideRuleType : int8_t {
  Unknown = 0,
  IDData = 1,
};

enum class DynamicOverrideRuleFlag : int16_t {
  /** The override rule is muted, it has no effect on evaluation results. */
  IsMuted = 1 << 0,
};
ENUM_OPERATORS(DynamicOverrideRuleFlag);

/**
 * Override rule, gathering a set of changes to apply to a same set of target IDs.
 */
struct DynamicOverrideRule {
  struct DynamicOverrideRule *next = nullptr, *prev = nullptr;

  /**
   * Name of this rule.
   *
   * NOTE: must be unique within a given Dynamic Override ID, so is also used as unique identifier.
   */
  char *name = nullptr;

  /** Type of rule, also defines the type of `rule_data`. */
  DynamicOverrideRuleType type = {};

  int8_t _pad[5] = {};

  DynamicOverrideRuleFlag flag = {};

  /** Define which ID(s) is/are affected by this rule. */
  DynamicOverrideRuleTargetFilter target_filter = {};
};

enum class DynamicOverrideRulePropertyFlag : int16_t {
  /** The override property is muted, it has no effect on evaluation results. */
  IsMuted = 1 << 0,
};
ENUM_OPERATORS(DynamicOverrideRulePropertyFlag);

struct DynamicOverrideRuleProperty {
  struct DynamicOverrideRuleProperty *next = nullptr, *prev = nullptr;

  /**
   * Reference to affected data (RNA path from owner ID).
   *
   * NOTE: Currently sub-data (e.g. collection item or array index) are not supported.
   *
   * NOTE: must be unique within a given IDData rule, so is also used as unique identifier.
   */
  char *rna_path = nullptr;

  DynamicOverrideRulePropertyFlag flag = {};
  int8_t _pad[6] = {};

  bke::dynoverride::RulePropertyRuntime *runtime = nullptr;
};

struct DynamicOverrideRuleIDData {
  DynamicOverrideRule base = {};

  /** List of overridden properties (based on RNA paths). */
  ListBaseT<DynamicOverrideRuleProperty> properties = {nullptr, nullptr};

  /**
   * New overridden values for all properties above.
   *
   * \note: Uses automatic data layout matching the runtime-generated RNA Srna data stored in
   * `runtime`.
   */
  IDProperty *override_values = nullptr;
  /** Original values for all properties above, follow samw layout as in `override_values`. */
  IDProperty *original_values = nullptr;

  bke::dynoverride::RuleIDDataRuntime *runtime = nullptr;
};

struct DynamicOverride {
#ifdef __cplusplus
  /** See #ID_Type comment for why this is here. */
  static constexpr ID_Type id_type = ID_OV;
#endif

  ID id;
  /** Animation data (must be immediately after id for utilities to use it). */
  struct AnimData *adt = nullptr;

  ListBaseT<DynamicOverrideRule> rules = {nullptr, nullptr};

  bke::dynoverride::Runtime *runtime = nullptr;
  void *_pad = nullptr;
};

}  // namespace blender
