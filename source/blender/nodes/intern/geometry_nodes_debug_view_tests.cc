/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "MEM_guardedalloc.h"

#include "BLI_listbase.hh"
#include "BLI_string.hh"

#include "BKE_compute_contexts.hh"
#include "BKE_gtest_setup.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_modifier.hh"
#include "BKE_node.hh"

#include "DNA_modifier_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"

#include "MOD_nodes.hh"

#include "NOD_eval_log.hh"
#include "NOD_geometry_nodes_debug_view.hh"

namespace blender::nodes::debug_view::tests {

static Identifier identifier(const uint64_t context,
                             const int32_t node,
                             const uint32_t tree_session_uid = 1)
{
  Identifier identifier;
  identifier.compute_context_hash.v1 = context;
  identifier.compute_context_hash.v2 = context * 17;
  identifier.node_tree_lineage_hash = node_tree_lineage_hash({&tree_session_uid, 1});
  identifier.viewer_node_id = node;
  return identifier;
}

static Candidate candidate(const uint64_t context,
                           const int32_t node,
                           const std::initializer_list<int> sort_order,
                           const std::initializer_list<const char *> context_names,
                           const StringRef viewer_name)
{
  Candidate candidate;
  candidate.identifier = identifier(context, node);
  candidate.sort_order.extend(sort_order);
  for (const char *context_name : context_names) {
    candidate.context_names.append(context_name);
  }
  candidate.viewer_name = viewer_name;
  return candidate;
}

static Vector<CandidatePresentation> fallback_presentations(const Span<Candidate> candidates,
                                                            const StringRef default_viewer_name)
{
  Main *bmain = BKE_main_new();
  Vector<CandidatePresentation> presentations = candidate_presentations(
      *bmain, candidates, default_viewer_name);
  BKE_main_free(bmain);
  return presentations;
}

TEST(GeometryNodesDebugView, MeaningfulViewerName)
{
  EXPECT_EQ(viewer_name_from_node("Surface", "Viewer.001"), "Surface");
  EXPECT_EQ(viewer_name_from_node("", "Topology"), "Topology");
  EXPECT_EQ(viewer_name_from_node("", "Viewer"), "");
  EXPECT_EQ(viewer_name_from_node("", "Viewer.001"), "");
}

TEST(GeometryNodesDebugView, DeterministicTreeOrder)
{
  Vector<Candidate> candidates;
  candidates.append(candidate(30, 3, {2}, {}, "Third"));
  candidates.append(candidate(10, 1, {1, 5}, {"Group"}, "Second"));
  candidates.append(candidate(20, 2, {1, 3}, {"Group"}, "First"));

  finalize_candidates(candidates);

  EXPECT_EQ(candidates[0].viewer_name, "First");
  EXPECT_EQ(candidates[1].viewer_name, "Second");
  EXPECT_EQ(candidates[2].viewer_name, "Third");
}

TEST(GeometryNodesDebugView, DuplicateNamesUseContextThenSuffix)
{
  Vector<Candidate> candidates;
  candidates.append(candidate(10, 1, {1}, {}, "Surface"));
  candidates.append(candidate(20, 2, {2, 1}, {"Inner Group"}, "Surface"));
  candidates.append(candidate(30, 3, {3, 1}, {"Inner Group"}, "Surface"));

  finalize_candidates(candidates);

  const Vector<CandidatePresentation> presentations = fallback_presentations(candidates, "Viewer");
  EXPECT_EQ(presentations[0].display_name, "Surface");
  EXPECT_EQ(presentations[1].display_name, "Inner Group / Surface");
  EXPECT_EQ(presentations[2].display_name, "Inner Group / Surface (2)");
  EXPECT_EQ(presentations[1].full_name, "Inner Group / Surface");
}

TEST(GeometryNodesDebugView, DefaultNameIsTranslatedAtPresentationTime)
{
  Vector<Candidate> candidates;
  candidates.append(candidate(10, 1, {1}, {"Default Group"}, ""));
  candidates.append(candidate(20, 2, {2}, {"Custom Group"}, "Viewer"));
  finalize_candidates(candidates);

  EXPECT_TRUE(candidates[0].viewer_name.empty());
  const Vector<CandidatePresentation> translated = fallback_presentations(candidates,
                                                                          "Translated Viewer");
  EXPECT_EQ(translated[0].display_name, "Translated Viewer");
  EXPECT_EQ(translated[1].display_name, "Viewer");

  const Vector<CandidatePresentation> untranslated = fallback_presentations(candidates, "Viewer");
  EXPECT_EQ(untranslated[0].display_name, "Default Group / Viewer");
  EXPECT_EQ(untranslated[1].display_name, "Custom Group / Viewer");
}

TEST(GeometryNodesDebugView, DuplicateIdentityIsRemoved)
{
  Vector<Candidate> candidates;
  candidates.append(candidate(10, 1, {1}, {}, "Surface"));
  candidates.append(candidate(10, 1, {1}, {}, "Surface"));

  finalize_candidates(candidates);

  EXPECT_EQ(candidates.size(), 1);
}

TEST(GeometryNodesDebugView, NodeTreeLineageIsPartOfIdentity)
{
  const Identifier original = identifier(10, 20, 100);
  const Identifier replacement = identifier(10, 20, 200);
  EXPECT_NE(original, replacement);
  EXPECT_NE(original.hash(), replacement.hash());

  State state;
  state.initialized = true;
  state.enabled.add(original);
  Vector<Candidate> replacement_candidates = {candidate(10, 20, {1}, {}, "Replacement")};
  replacement_candidates[0].identifier = replacement;
  EXPECT_TRUE(enabled_candidate_indices(replacement_candidates, state).is_empty());
  EXPECT_FALSE(state.enabled.contains(replacement));
}

TEST(GeometryNodesDebugView, CompleteOuterToInnerNodeTreeLineageIsHashed)
{
  const uint32_t root_a_nested_a[] = {10, 20};
  const uint32_t root_b_nested_a[] = {11, 20};
  const uint32_t root_a_nested_b[] = {10, 21};
  const uint32_t root_a_nested_a_again[] = {10, 20};

  EXPECT_NE(node_tree_lineage_hash({root_a_nested_a, 2}),
            node_tree_lineage_hash({root_b_nested_a, 2}));
  EXPECT_NE(node_tree_lineage_hash({root_a_nested_a, 2}),
            node_tree_lineage_hash({root_a_nested_b, 2}));
  EXPECT_EQ(node_tree_lineage_hash({root_a_nested_a, 2}),
            node_tree_lineage_hash({root_a_nested_a_again, 2}));

  /* Repeated instances of one nested tree retain the same lineage but remain distinct through
   * their compute contexts. */
  const Identifier first_instance = identifier(100, 30);
  Identifier second_instance = first_instance;
  second_instance.compute_context_hash.v1++;
  EXPECT_EQ(first_instance.node_tree_lineage_hash, second_instance.node_tree_lineage_hash);
  EXPECT_NE(first_instance, second_instance);
}

TEST(GeometryNodesDebugView, GeometryEligibility)
{
  EXPECT_TRUE(is_geometry_candidate(true, true, true));
  EXPECT_FALSE(is_geometry_candidate(true, false, true));
  EXPECT_FALSE(is_geometry_candidate(true, true, false));
  EXPECT_FALSE(is_geometry_candidate(false, true, true));
}

TEST(GeometryNodesDebugView, FirstInitialization)
{
  Vector<Candidate> candidates;
  candidates.append(candidate(10, 1, {1}, {}, "First"));
  candidates.append(candidate(20, 2, {2}, {}, "Second"));
  finalize_candidates(candidates);

  State state;
  initialize_state({}, state);
  EXPECT_FALSE(state.initialized);
  EXPECT_TRUE(state.enabled.is_empty());

  initialize_state(candidates, state);
  EXPECT_TRUE(state.initialized);
  EXPECT_EQ(state.enabled.size(), 1);
  EXPECT_TRUE(state.enabled.contains(candidates[0].identifier));

  state.enabled.add(candidates[1].identifier);
  initialize_state(candidates.as_span().take_front(1), state);
  EXPECT_EQ(state.enabled.size(), 2);
}

TEST(GeometryNodesDebugView, ZeroOneAndManyEnabledCandidates)
{
  Vector<Candidate> candidates;
  candidates.append(candidate(10, 1, {1}, {}, "First"));
  candidates.append(candidate(20, 2, {2}, {}, "Second"));
  candidates.append(candidate(30, 3, {3}, {}, "Third"));
  finalize_candidates(candidates);

  State state;
  state.initialized = true;
  EXPECT_TRUE(enabled_candidate_indices(candidates, state).is_empty());

  state.enabled.add(candidates[1].identifier);
  EXPECT_EQ(enabled_candidate_indices(candidates, state), Vector<int>({1}));

  state.enabled.add(candidates[0].identifier);
  state.enabled.add(candidates[2].identifier);
  EXPECT_EQ(enabled_candidate_indices(candidates, state), Vector<int>({0, 1, 2}));
}

TEST(GeometryNodesDebugView, IndependentTogglingAndDisableAll)
{
  Vector<Candidate> candidates;
  candidates.append(candidate(10, 1, {1}, {}, "First"));
  candidates.append(candidate(20, 2, {2}, {}, "Second"));
  finalize_candidates(candidates);

  State state;
  initialize_state(candidates, state);
  EXPECT_TRUE(toggle_candidate(candidates, candidates[1].identifier, state));
  EXPECT_TRUE(state.enabled.contains(candidates[0].identifier));
  EXPECT_TRUE(state.enabled.contains(candidates[1].identifier));

  EXPECT_TRUE(toggle_candidate(candidates, candidates[0].identifier, state));
  EXPECT_FALSE(state.enabled.contains(candidates[0].identifier));
  EXPECT_TRUE(state.enabled.contains(candidates[1].identifier));

  EXPECT_TRUE(toggle_candidate(candidates, candidates[1].identifier, state));
  EXPECT_TRUE(state.enabled.is_empty());
  EXPECT_TRUE(state.initialized);
  EXPECT_FALSE(toggle_candidate(candidates, identifier(99, 99), state));
  EXPECT_TRUE(state.enabled.is_empty());
}

TEST(GeometryNodesDebugView, StaleIdentityRestorationAndNewCandidatesDefaultOff)
{
  Vector<Candidate> candidates;
  candidates.append(candidate(10, 1, {1}, {}, "First"));
  candidates.append(candidate(20, 2, {2}, {}, "Second"));
  finalize_candidates(candidates);

  State state;
  initialize_state(candidates, state);
  toggle_candidate(candidates, candidates[1].identifier, state);
  EXPECT_EQ(state.enabled.size(), 2);

  Vector<Candidate> changed_candidates = {candidates[1], candidate(30, 3, {3}, {}, "New")};
  finalize_candidates(changed_candidates);
  EXPECT_EQ(enabled_candidate_indices(changed_candidates, state), Vector<int>({0}));
  EXPECT_EQ(state.enabled.size(), 2);
  EXPECT_FALSE(state.enabled.contains(changed_candidates[1].identifier));

  changed_candidates.append(candidates[0]);
  finalize_candidates(changed_candidates);
  EXPECT_EQ(enabled_candidate_indices(changed_candidates, state), Vector<int>({0, 1}));
  EXPECT_EQ(state.enabled.size(), 2);
}

TEST(GeometryNodesDebugView, RemovingCandidateDoesNotChangeOtherStates)
{
  Vector<Candidate> candidates;
  candidates.append(candidate(10, 1, {1}, {}, "First"));
  candidates.append(candidate(20, 2, {2}, {}, "Second"));
  candidates.append(candidate(30, 3, {3}, {}, "Third"));
  finalize_candidates(candidates);

  State state;
  state.initialized = true;
  state.enabled.add(candidates[0].identifier);
  state.enabled.add(candidates[2].identifier);
  candidates.remove(1);

  EXPECT_EQ(enabled_candidate_indices(candidates, state), Vector<int>({0, 1}));
  EXPECT_EQ(state.enabled.size(), 2);
}

TEST(GeometryNodesDebugView, StateIsIndependentBetweenModifiersAndObjectsSharingNodeGroup)
{
  Vector<Candidate> candidates;
  candidates.append(candidate(10, 1, {1}, {}, "First"));
  candidates.append(candidate(20, 2, {2}, {}, "Second"));
  finalize_candidates(candidates);

  State modifier_a;
  State modifier_b;
  initialize_state(candidates, modifier_a);
  initialize_state(candidates, modifier_b);
  toggle_candidate(candidates, candidates[1].identifier, modifier_a);
  toggle_candidate(candidates, candidates[0].identifier, modifier_b);

  EXPECT_EQ(enabled_candidate_indices(candidates, modifier_a), Vector<int>({0, 1}));
  EXPECT_TRUE(enabled_candidate_indices(candidates, modifier_b).is_empty());
}

TEST(GeometryNodesDebugView, DeterministicCandidateIntersection)
{
  Vector<Candidate> candidates;
  candidates.append(candidate(30, 3, {3}, {}, "Third"));
  candidates.append(candidate(10, 1, {1}, {}, "First"));
  candidates.append(candidate(20, 2, {2}, {}, "Second"));

  State state;
  state.initialized = true;
  state.enabled.add(candidates[0].identifier);
  state.enabled.add(candidates[2].identifier);
  finalize_candidates(candidates);

  EXPECT_EQ(enabled_candidate_indices(candidates, state), Vector<int>({1, 2}));
}

TEST(GeometryNodesDebugView, PreserveStateAcrossObjectUndo)
{
  Object old_object = {};
  Object new_object = {};
  NodesModifierData old_modifier = {};
  NodesModifierData new_modifier = {};
  old_modifier.modifier.type = eModifierType_Nodes;
  new_modifier.modifier.type = eModifierType_Nodes;
  old_modifier.modifier.persistent_uid = 42;
  new_modifier.modifier.persistent_uid = 42;
  old_modifier.runtime = MEM_new<NodesModifierRuntime>(__func__);
  new_modifier.runtime = MEM_new<NodesModifierRuntime>(__func__);
  BLI_addtail(&old_object.modifiers, &old_modifier);
  BLI_addtail(&new_object.modifiers, &new_modifier);

  const Identifier enabled_identifier = identifier(10, 30);
  old_modifier.runtime->debug_view_state.enabled.add(enabled_identifier);
  old_modifier.runtime->debug_view_state.initialized = true;

  MOD_nodes_preserve_runtime_state_on_undo(old_object, new_object);

  EXPECT_TRUE(new_modifier.runtime->debug_view_state.initialized);
  EXPECT_TRUE(new_modifier.runtime->debug_view_state.enabled.contains(enabled_identifier));
  EXPECT_NE(&old_modifier.runtime->debug_view_state, &new_modifier.runtime->debug_view_state);

  new_modifier.runtime->debug_view_state.enabled.clear();
  EXPECT_TRUE(old_modifier.runtime->debug_view_state.enabled.contains(enabled_identifier));

  MEM_delete(old_modifier.runtime);
  MEM_delete(new_modifier.runtime);
}

TEST(GeometryNodesDebugView, PreserveStateAcrossUndoMatchesObjectLocalPersistentUid)
{
  Object old_object = {};
  Object new_object = {};
  NodesModifierData old_first = {};
  NodesModifierData old_second = {};
  NodesModifierData new_second = {};
  NodesModifierData new_replacement = {};
  for (NodesModifierData *nmd : {&old_first, &old_second, &new_second, &new_replacement}) {
    nmd->modifier.type = eModifierType_Nodes;
    nmd->runtime = MEM_new<NodesModifierRuntime>(__func__);
  }
  old_first.modifier.persistent_uid = 10;
  old_second.modifier.persistent_uid = 20;
  new_second.modifier.persistent_uid = 20;
  new_replacement.modifier.persistent_uid = 30;
  BLI_addtail(&old_object.modifiers, &old_first);
  BLI_addtail(&old_object.modifiers, &old_second);
  /* Reordered matching modifier, followed by an unrelated replacement. */
  BLI_addtail(&new_object.modifiers, &new_second);
  BLI_addtail(&new_object.modifiers, &new_replacement);

  old_first.runtime->debug_view_state.initialized = true;
  old_first.runtime->debug_view_state.enabled.add(identifier(1, 1));
  old_second.runtime->debug_view_state.initialized = true;
  old_second.runtime->debug_view_state.enabled.add(identifier(2, 2));

  MOD_nodes_preserve_runtime_state_on_undo(old_object, new_object);

  EXPECT_TRUE(new_second.runtime->debug_view_state.enabled.contains(identifier(2, 2)));
  EXPECT_FALSE(new_second.runtime->debug_view_state.enabled.contains(identifier(1, 1)));
  EXPECT_FALSE(new_replacement.runtime->debug_view_state.initialized);
  EXPECT_TRUE(new_replacement.runtime->debug_view_state.enabled.is_empty());

  for (NodesModifierData *nmd : {&old_first, &old_second, &new_second, &new_replacement}) {
    MEM_delete(nmd->runtime);
  }
}

TEST(GeometryNodesDebugView, CorrespondingDuplisUseDistinctTopLevelPersistentIds)
{
  Vector<Candidate> candidates = {candidate(10, 1, {1}, {}, "Same"),
                                  candidate(20, 1, {1}, {}, "Same")};
  finalize_candidates(candidates);
  State state;
  state.initialized = true;
  state.enabled.add(candidates[0].identifier);
  state.enabled.add(candidates[1].identifier);
  ASSERT_EQ(enabled_candidate_indices(candidates, state), Vector<int>({0, 1}));

  Set<int> used_namespaces;
  const int first_namespace = unique_persistent_id_namespace(candidates[0].identifier,
                                                             used_namespaces);
  const int second_namespace = unique_persistent_id_namespace(candidates[1].identifier,
                                                              used_namespaces);
  EXPECT_NE(first_namespace, second_namespace);
  EXPECT_NE(first_namespace, INT_MAX);
  EXPECT_NE(second_namespace, INT_MAX);

  /* The component or instance ID precedes the source namespace in the persistent ID. */
  const int first_component_persistent_id[2] = {0, first_namespace};
  const int second_component_persistent_id[2] = {0, second_namespace};
  EXPECT_EQ(first_component_persistent_id[0], second_component_persistent_id[0]);
  EXPECT_NE(first_component_persistent_id[1], second_component_persistent_id[1]);

  const int first_instance_persistent_id[2] = {42, first_namespace};
  const int second_instance_persistent_id[2] = {42, second_namespace};
  EXPECT_EQ(first_instance_persistent_id[0], second_instance_persistent_id[0]);
  EXPECT_NE(first_instance_persistent_id[1], second_instance_persistent_id[1]);

  Set<int> forced_collision;
  forced_collision.add(first_namespace);
  const int probed_namespace = unique_persistent_id_namespace(candidates[0].identifier,
                                                              forced_collision);
  EXPECT_NE(probed_namespace, first_namespace);
  EXPECT_NE(probed_namespace, INT_MAX);

  candidates[0].viewer_name = "Renamed";
  candidates[0].sort_order = {100};
  EXPECT_EQ(persistent_id_namespace(candidates[0].identifier), first_namespace);
}

TEST(GeometryNodesDebugView, ProductionNamespaceCollisionProbingIsDeterministic)
{
  /* Found once with the production hash. Keep the full-strength identities as a fixture so this
   * test exercises the real collision path without a search on every test run. */
  const Identifier first = {{100, 1700}, {1198274633936249892ULL, 16238052728921080834ULL}, 200};
  const Identifier second = {{100, 1700}, {2905449761900578234ULL, 6204400446275641994ULL}, 200};
  EXPECT_NE(first, second);
  EXPECT_EQ(persistent_id_namespace(first), persistent_id_namespace(second));

  Set<int> first_run;
  const int first_namespace = unique_persistent_id_namespace(first, first_run);
  const int second_namespace = unique_persistent_id_namespace(second, first_run);
  EXPECT_NE(first_namespace, second_namespace);
  EXPECT_NE(first_namespace, INT_MAX);
  EXPECT_NE(second_namespace, INT_MAX);

  Set<int> second_run;
  EXPECT_EQ(unique_persistent_id_namespace(first, second_run), first_namespace);
  EXPECT_EQ(unique_persistent_id_namespace(second, second_run), second_namespace);
}

static void add_viewer_log(eval_log::NodeTreeLogger &tree_logger,
                           const int32_t node_id,
                           const int node_order,
                           const StringRef name)
{
  destruct_ptr<eval_log::ViewerNodeLog> viewer_log =
      tree_logger.allocator->construct<eval_log::ViewerNodeLog>();
  viewer_log->is_debug_view = true;
  viewer_log->node_order = node_order;
  viewer_log->debug_view_name = name;
  viewer_log->items.add_new({0, "Geometry", bke::SocketValueVariant::From(bke::GeometrySet())});
  tree_logger.viewer_node_logs.append(*tree_logger.allocator, {node_id, std::move(viewer_log)});
}

class CandidateCollectionTest : public testing::Test {
 public:
  static void SetUpTestSuite()
  {
    bke::gtest_setup();
  }

  static void TearDownTestSuite()
  {
    bke::gtest_teardown();
  }
};

TEST_F(CandidateCollectionTest, PresentationLabelsRefreshWithoutChangingIdentityOrState)
{
  Main *bmain = BKE_main_new();
  bNodeTree &root = *bke::node_tree_add_tree(bmain, "Root", "GeometryNodeTree");
  bNodeTree &nested = *bke::node_tree_add_tree(bmain, "Nested", "GeometryNodeTree");
  bNode &group_node = *bke::node_add_node(nullptr, root, "GeometryNodeGroup"_ustr);
  group_node.id = &nested.id;
  id_us_plus(group_node.id);
  bNode &viewer_node = *bke::node_add_node(nullptr, nested, "GeometryNodeViewer"_ustr);
  STRNCPY(group_node.label, "Prepared Group");
  STRNCPY(viewer_node.label, "Prepared Viewer");
  Candidate candidate;
  candidate.identifier = identifier(10, viewer_node.identifier);
  const std::array<uint32_t, 2> lineage = {root.id.session_uid, nested.id.session_uid};
  candidate.identifier.node_tree_lineage_hash = node_tree_lineage_hash(lineage);
  candidate.tree_session_uids = {root.id.session_uid, nested.id.session_uid};
  candidate.group_node_ids = {group_node.identifier};
  candidate.context_names = {"Logged Group"};
  candidate.viewer_name = "Logged Viewer";
  const Vector<Candidate> candidates = {candidate};
  State state;
  state.initialized = true;
  state.enabled.add(candidate.identifier);

  Vector<CandidatePresentation> presentations = candidate_presentations(
      *bmain, candidates, "Viewer");
  EXPECT_EQ(presentations[0].full_name, "Prepared Group / Prepared Viewer");

  STRNCPY(group_node.label, "Renamed Group");
  STRNCPY(viewer_node.label, "Renamed Viewer");
  presentations = candidate_presentations(*bmain, candidates, "Viewer");
  EXPECT_EQ(presentations[0].full_name, "Renamed Group / Renamed Viewer");
  EXPECT_TRUE(state.enabled.contains(candidate.identifier));
  EXPECT_EQ(candidate.identifier, candidates[0].identifier);

  group_node.label[0] = '\0';
  BKE_id_rename(*bmain, nested.id, "Renamed Nested Tree");
  presentations = candidate_presentations(*bmain, candidates, "Viewer");
  EXPECT_EQ(presentations[0].full_name, "Renamed Nested Tree / Renamed Viewer");

  BKE_main_free(bmain);
}

TEST_F(CandidateCollectionTest, ModifierAndObjectCopyPathCreatesFreshRuntimeState)
{
  ModifierData *source_md = BKE_modifier_new(eModifierType_Nodes);
  auto &source = *reinterpret_cast<NodesModifierData *>(source_md);
  source.runtime->debug_view_state.initialized = true;
  source.runtime->debug_view_state.enabled.add(identifier(10, 20));

  ModifierData *copy_md = BKE_modifier_copy_ex(source_md, 0);
  auto &copy = *reinterpret_cast<NodesModifierData *>(copy_md);
  EXPECT_NE(source.runtime, copy.runtime);
  EXPECT_FALSE(copy.runtime->debug_view_state.initialized);
  EXPECT_TRUE(copy.runtime->debug_view_state.enabled.is_empty());

  BKE_modifier_free(source_md);
  BKE_modifier_free(copy_md);
}

static Vector<Identifier> identifiers_for_tree_lineage(const uint32_t root_tree_uid,
                                                       const uint32_t nested_tree_uid)
{
  eval_log::NodesEvalLog root_log;
  NodesModifierData nmd = {};
  bke::DataBlockComputeContext object_context(nullptr, 1);
  bke::GeometryNodesModifierComputeContext modifier_context(&object_context, nmd);
  bke::GroupNodeComputeContext nested_context(&modifier_context, 50);
  eval_log::NodeTreeLogger &root_logger = root_log.get_local_tree_logger(modifier_context);
  root_logger.tree_orig_session_uid = root_tree_uid;
  eval_log::NodeTreeLogger &nested_logger = root_log.get_local_tree_logger(nested_context);
  nested_logger.tree_orig_session_uid = nested_tree_uid;
  nested_logger.context_order = 1;
  add_viewer_log(nested_logger, 60, 1, "Viewer");
  Vector<Identifier> identifiers;
  for (const Candidate &candidate : root_log.debug_view_candidates()) {
    identifiers.append(candidate.identifier);
  }
  return identifiers;
}

static Vector<Candidate> candidates_from_identifiers(const Span<Identifier> identifiers)
{
  Vector<Candidate> candidates;
  for (const Identifier &identifier : identifiers) {
    Candidate candidate;
    candidate.identifier = identifier;
    candidates.append(std::move(candidate));
  }
  return candidates;
}

TEST_F(CandidateCollectionTest, UnrelatedNodeGroupReplacementDoesNotInheritEnabledState)
{
  const Vector<Identifier> original_identifiers = identifiers_for_tree_lineage(100, 200);
  const Vector<Identifier> replacement_identifiers = identifiers_for_tree_lineage(101, 201);
  const Vector<Candidate> original_candidates = candidates_from_identifiers(original_identifiers);
  const Vector<Candidate> replacement_candidates = candidates_from_identifiers(
      replacement_identifiers);
  ASSERT_EQ(original_candidates.size(), 1);
  ASSERT_EQ(replacement_candidates.size(), 1);
  /* Both evaluations deliberately reuse the same context structure and Viewer node identifier. */
  EXPECT_EQ(original_candidates[0].identifier.compute_context_hash,
            replacement_candidates[0].identifier.compute_context_hash);
  EXPECT_EQ(original_candidates[0].identifier.viewer_node_id,
            replacement_candidates[0].identifier.viewer_node_id);
  EXPECT_NE(original_candidates[0].identifier.node_tree_lineage_hash,
            replacement_candidates[0].identifier.node_tree_lineage_hash);

  State state;
  initialize_state(original_candidates, state);
  EXPECT_EQ(enabled_candidate_indices(original_candidates, state), Vector<int>({0}));
  EXPECT_TRUE(enabled_candidate_indices(replacement_candidates, state).is_empty());
  EXPECT_FALSE(toggle_candidate(replacement_candidates, original_candidates[0].identifier, state));
}

TEST_F(CandidateCollectionTest, NodeGroupReplacementUndoRedoKeepsOnlyMatchingLineageEnabled)
{
  const Vector<Identifier> original_identifiers = identifiers_for_tree_lineage(100, 200);
  const Vector<Identifier> replacement_identifiers = identifiers_for_tree_lineage(101, 201);
  const Vector<Candidate> original_candidates = candidates_from_identifiers(original_identifiers);
  const Vector<Candidate> replacement_candidates = candidates_from_identifiers(
      replacement_identifiers);
  Object before_replacement = {};
  Object after_replacement = {};
  NodesModifierData before_modifier = {};
  NodesModifierData after_modifier = {};
  for (NodesModifierData *nmd : {&before_modifier, &after_modifier}) {
    nmd->modifier.type = eModifierType_Nodes;
    nmd->modifier.persistent_uid = 42;
    nmd->runtime = MEM_new<NodesModifierRuntime>(__func__);
  }
  BLI_addtail(&before_replacement.modifiers, &before_modifier);
  BLI_addtail(&after_replacement.modifiers, &after_modifier);
  initialize_state(original_candidates, before_modifier.runtime->debug_view_state);

  /* Redo/current-state transfer keeps the old intent, but it cannot match the unrelated tree. */
  MOD_nodes_preserve_runtime_state_on_undo(before_replacement, after_replacement);
  EXPECT_TRUE(
      enabled_candidate_indices(replacement_candidates, after_modifier.runtime->debug_view_state)
          .is_empty());

  /* Undo transfers the same runtime intent back to the restored datablocks and session UIDs. */
  before_modifier.runtime->debug_view_state = {};
  MOD_nodes_preserve_runtime_state_on_undo(after_replacement, before_replacement);
  EXPECT_EQ(
      enabled_candidate_indices(original_candidates, before_modifier.runtime->debug_view_state),
      Vector<int>({0}));

  MEM_delete(before_modifier.runtime);
  MEM_delete(after_modifier.runtime);
}

TEST_F(CandidateCollectionTest, MissingNodeTreeLineageDoesNotCreateIncompleteIdentity)
{
  eval_log::NodesEvalLog root_log;
  bke::DataBlockComputeContext compute_context(nullptr, 1);
  eval_log::NodeTreeLogger &tree_logger = root_log.get_local_tree_logger(compute_context);
  add_viewer_log(tree_logger, 10, 1, "Viewer");
  EXPECT_TRUE(root_log.debug_view_candidates().is_empty());
}

TEST_F(CandidateCollectionTest, IsCached)
{
  eval_log::NodesEvalLog root_log;
  NodesModifierData nmd = {};
  bke::DataBlockComputeContext object_context(nullptr, 1);
  bke::GeometryNodesModifierComputeContext modifier_context(&object_context, nmd);
  eval_log::NodeTreeLogger &tree_logger = root_log.get_local_tree_logger(modifier_context);
  tree_logger.tree_orig_session_uid = 100;
  add_viewer_log(tree_logger, 20, 2, "Second");
  add_viewer_log(tree_logger, 10, 1, "First");

  const Span<Candidate> first_candidates = root_log.debug_view_candidates();
  ASSERT_EQ(first_candidates.size(), 2);
  const Vector<CandidatePresentation> presentations = fallback_presentations(first_candidates,
                                                                             "Viewer");
  EXPECT_EQ(presentations[0].display_name, "First");
  EXPECT_EQ(presentations[1].display_name, "Second");

  const Span<Candidate> second_candidates = root_log.debug_view_candidates();
  EXPECT_EQ(second_candidates.data(), first_candidates.data());
  EXPECT_EQ(second_candidates[0].identifier, first_candidates[0].identifier);
  EXPECT_EQ(second_candidates[1].identifier, first_candidates[1].identifier);
}

}  // namespace blender::nodes::debug_view::tests
