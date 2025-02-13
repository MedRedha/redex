/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RemoveUnusedArgs.h"

#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "CallGraph.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "Liveness.h"
#include "Match.h"
#include "OptData.h"
#include "OptDataDefs.h"
#include "TypeSystem.h"
#include "VirtualScope.h"
#include "Walkers.h"

using namespace opt_metadata;

/**
 * The RemoveUnusedArgsPass finds method arguments that are not live in the
 * method body, removes those unused arguments from the method signature, and
 * removes the corresponding argument registers from invocations of that
 * method.
 */
namespace remove_unused_args {

constexpr const char* METRIC_CALLSITE_ARGS_REMOVED = "callsite_args_removed";
constexpr const char* METRIC_METHOD_PARAMS_REMOVED = "method_params_removed";
constexpr const char* METRIC_METHODS_UPDATED = "method_signatures_updated";
constexpr const char* METRIC_METHOD_RESULTS_REMOVED = "method_results_removed";
constexpr const char* METRIC_DEAD_INSTRUCTION_COUNT =
    "num_local_dce_dead_instruction_count";
constexpr const char* METRIC_UNREACHABLE_INSTRUCTION_COUNT =
    "num_local_dce_unreachable_instruction_count";
constexpr const char* METRIC_ITERATIONS = "iterations";

static LocalDce::Stats add_dce_stats(LocalDce::Stats a, LocalDce::Stats b) {
  return {a.dead_instruction_count + b.dead_instruction_count,
          a.unreachable_instruction_count + b.unreachable_instruction_count};
};

/**
 * Returns metrics as listed above from running RemoveArgs:
 * run() removes unused params from method signatures and param loads, then
 * updates all affected callsites accordingly.
 */
RemoveArgs::PassStats RemoveArgs::run() {
  RemoveArgs::PassStats pass_stats;
  gather_results_used();
  auto method_stats = update_meths_with_unused_args_or_results();
  pass_stats.method_params_removed_count =
      method_stats.method_params_removed_count;
  pass_stats.methods_updated_count = method_stats.methods_updated_count;
  pass_stats.callsite_args_removed_count = update_callsites();
  pass_stats.method_results_removed_count =
      method_stats.method_results_removed_count;
  pass_stats.local_dce_stats = method_stats.local_dce_stats;
  return pass_stats;
}

/**
 * Inspects all invoke instructions, and whether they are followed by
 * move-result instructions, and record this information for each method.
 */
void RemoveArgs::gather_results_used() {
  walk::parallel::code(
      m_scope, [& result_used = m_result_used](DexMethod*, IRCode& code) {
        const auto ii = InstructionIterable(code);
        for (auto it = ii.begin(); it != ii.end(); it++) {
          auto insn = it->insn;
          if (!is_invoke(insn->opcode())) {
            continue;
          }
          const auto next = std::next(it);
          always_assert(next != ii.end());
          const auto peek = next->insn;
          if (!is_move_result(peek->opcode())) {
            continue;
          }
          auto method_ref = insn->get_method();
          if (!method_ref->is_def()) {
            // TODO: T31388603 -- Remove unused results for true virtuals.
            continue;
          }
          // Since is_def() is true, the following cast is safe and appropriate.
          DexMethod* method = static_cast<DexMethod*>(method_ref);
          result_used.insert(method);
        }
      });
}

/**
 * Returns a vector of live argument indices.
 * Updates dead_insns with the load_params that await removal.
 * For instance methods, the 'this' argument is always considered live.
 * e.g. We return {0, 2} for a method whose 0th and 2nd args are live.
 *
 * NOTE: In the IR, invoke instructions specify exactly one register
 *       for any param size.
 */
std::deque<uint16_t> RemoveArgs::compute_live_args(
    DexMethod* method,
    size_t num_args,
    std::vector<IRInstruction*>* dead_insns) {
  auto code = method->get_code();
  code->build_cfg(/* editable */ false);
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();
  LivenessFixpointIterator fixpoint_iter(cfg);
  fixpoint_iter.run(LivenessDomain(code->get_registers_size()));
  auto entry_block = cfg.entry_block();

  std::deque<uint16_t> live_arg_idxs;
  bool is_instance_method = !is_static(method);
  size_t last_arg_idx = is_instance_method ? num_args : num_args - 1;
  auto first_insn = entry_block->get_first_insn()->insn;
  // live_vars contains all the registers needed by entry_block's successors.
  auto live_vars = fixpoint_iter.get_live_out_vars_at(entry_block);

  for (auto it = entry_block->rbegin(); it != entry_block->rend(); ++it) {
    if (it->type != MFLOW_OPCODE) {
      continue;
    }
    auto insn = it->insn;
    if (opcode::is_load_param(insn->opcode())) {
      if (live_vars.contains(insn->dest()) ||
          (is_instance_method && it->insn == first_insn)) {
        // Mark live args live, and always mark the "this" arg live.
        live_arg_idxs.push_front(last_arg_idx);
      } else {
        dead_insns->emplace_back(it->insn);
      }
      last_arg_idx--;
    }
    fixpoint_iter.analyze_instruction(insn, &live_vars);
  }

  always_assert(live_arg_idxs.size() + dead_insns->size() == is_instance_method
                    ? num_args + 1
                    : num_args);

  return live_arg_idxs;
}

/**
 * Returns an updated argument type list for the given method with the given
 * live argument indices.
 */
std::deque<DexType*> RemoveArgs::get_live_arg_type_list(
    DexMethod* method, const std::deque<uint16_t>& live_arg_idxs) {
  std::deque<DexType*> live_args;
  auto args_list = method->get_proto()->get_args()->get_type_list();

  for (uint16_t arg_num : live_arg_idxs) {
    if (!is_static(method)) {
      if (arg_num == 0) {
        continue;
      }
      arg_num--;
    }
    live_args.push_back(args_list.at(arg_num));
  }
  return live_args;
}

/**
 * Returns true on successful update to the given method's signature, where
 * the updated args list is specified by live_args.
 */
bool RemoveArgs::update_method_signature(
    DexMethod* method,
    const std::deque<uint16_t>& live_arg_idxs,
    bool remove_result) {
  always_assert_log(method->is_def(),
                    "We don't treat virtuals, so methods must be defined\n");

  const std::string& full_name = method->get_deobfuscated_name();
  for (const auto& s : m_black_list) {
    if (full_name.find(s) != std::string::npos) {
      TRACE(ARGS, 3,
            "Skipping {%s} due to black list match of {%s} against {%s}",
            SHOW(method), full_name.c_str(), s.c_str());
      return false;
    }
  }

  auto num_orig_args = method->get_proto()->get_args()->get_type_list().size();
  auto live_args = get_live_arg_type_list(method, live_arg_idxs);
  auto live_args_list = DexTypeList::make_type_list(std::move(live_args));
  DexType* rtype =
      remove_result ? get_void_type() : method->get_proto()->get_rtype();
  auto updated_proto = DexProto::make_proto(rtype, live_args_list);
  always_assert(updated_proto != method->get_proto());

  auto colliding_method = DexMethod::get_method(
      method->get_class(), method->get_name(), updated_proto);
  if (colliding_method && colliding_method->is_def() &&
      is_constructor(static_cast<const DexMethod*>(colliding_method))) {
    // We can't rename constructors, so we give up on removing args.
    return false;
  }

  auto name = method->get_name();
  if (method->is_virtual()) {
    // TODO: T31388603 -- Remove unused args for true virtuals.

    // We need to worry about creating shadowing in the virtual scope ---
    // for this particular method change, but also across all other upcoming
    // method changes. To this end, we introduce unique names for each name and
    // arg list to avoid any such overlaps.
    size_t name_index = m_renamed_indices[name][live_args_list]++;
    std::stringstream ss;
    // This pass typically runs before the obfuscation pass, so we should not
    // need to be concerned here about creating long method names.
    // "uva" stands for unused virtual args
    ss << name->str() << "$uva" << std::to_string(m_iteration) << "$"
       << std::to_string(name_index);
    name = DexString::make_string(ss.str());
  }

  DexMethodSpec spec(method->get_class(), name, updated_proto);
  method->change(spec,
                 true /* rename on collision */,
                 true /* update deobfuscated name */);

  TRACE(ARGS, 3, "Method signature updated to %s", SHOW(method));
  log_opt(METHOD_PARAMS_REMOVED, method);
  return true;
}

/**
 * For methods that have unused arguments, record live argument registers.
 */
RemoveArgs::MethodStats RemoveArgs::update_meths_with_unused_args_or_results() {
  // Phase 1: Find (in parallel) all methods that we can potentially update

  struct Entry {
    std::vector<IRInstruction*> dead_insns;
    std::deque<uint16_t> live_arg_idxs;
    bool remove_result;
  };
  ConcurrentMap<DexMethod*, Entry> unordered_entries;
  walk::parallel::methods(m_scope, [&](DexMethod* method) {
    if (method->get_code() == nullptr) {
      return;
    }
    auto proto = method->get_proto();
    bool result_used = !!m_result_used.count_unsafe(method);
    auto num_args = proto->get_args()->size();
    bool remove_result = !proto->is_void() && !result_used;
    // For instance methods, num_args does not count the 'this' argument.
    if (num_args == 0 && !remove_result) {
      // Nothing to do if the method doesn't have args or result to remove.
      return;
    }

    if (!can_rename(method)) {
      // Nothing to do if ProGuard says we can't change the method args.
      TRACE(ARGS,
            5,
            "Method is disqualified from being updated by ProGuard rules: %s",
            SHOW(method));
      return;
    }

    // If a method is devirtualizable, proceed with live arg computation.
    if (method->is_virtual()) {
      auto virt_scope = m_type_system.find_virtual_scope(method);
      if (virt_scope == nullptr || !is_non_virtual_scope(virt_scope)) {
        // TODO: T31388603 -- Remove unused args for true virtuals.
        return;
      }
    }

    std::vector<IRInstruction*> dead_insns;
    auto live_arg_idxs = compute_live_args(method, num_args, &dead_insns);
    if (dead_insns.empty() && !remove_result) {
      return;
    }

    // Remember entry
    unordered_entries.emplace(
        method, (Entry){dead_insns, live_arg_idxs, remove_result});
  });

  // Phase 2: Deterministically update proto (including (re)name as needed)

  // Sort entries, so that we process all renaming operations in a
  // deterministic order.
  std::vector<std::pair<DexMethod*, Entry>> ordered_entries(
      unordered_entries.begin(), unordered_entries.end());
  std::sort(ordered_entries.begin(), ordered_entries.end(),
            [](const std::pair<DexMethod*, Entry>& a,
               const std::pair<DexMethod*, Entry>& b) {
              return compare_dexmethods(a.first, b.first);
            });

  RemoveArgs::MethodStats method_stats;
  std::vector<DexClass*> classes;
  std::unordered_map<DexClass*, std::vector<std::pair<DexMethod*, Entry>>>
      class_entries;
  for (auto& p : ordered_entries) {
    DexMethod* method = p.first;
    const Entry& entry = p.second;
    if (!update_method_signature(method, entry.live_arg_idxs,
                                 entry.remove_result)) {
      continue;
    }

    // Remember entry for further processing, and log statistics
    DexClass* cls = type_class(method->get_class());
    classes.push_back(cls);
    class_entries[cls].push_back(p);
    method_stats.methods_updated_count++;
    method_stats.method_params_removed_count += entry.dead_insns.size();
    method_stats.method_results_removed_count += entry.remove_result ? 1 : 0;
  }
  sort_unique(classes);

  // Phase 3: Update body of updated methods (in parallel)

  std::mutex local_dce_stats_mutex;
  auto& local_dce_stats = method_stats.local_dce_stats;
  walk::parallel::classes(classes, [&](DexClass* cls) {
    for (auto& p : class_entries.at(cls)) {
      DexMethod* method = p.first;
      const Entry& entry = p.second;

      if (!entry.dead_insns.empty()) {
        // We update the method signature, so we must remove unused
        // OPCODE_LOAD_PARAM_* to satisfy IRTypeChecker.
        for (auto dead_insn : entry.dead_insns) {
          method->get_code()->remove_opcode(dead_insn);
        }
        m_live_arg_idxs_map.emplace(method, entry.live_arg_idxs);
      }

      if (entry.remove_result) {
        for (const auto& mie : InstructionIterable(method->get_code())) {
          auto insn = mie.insn;
          if (is_return_value(insn->opcode())) {
            insn->set_opcode(OPCODE_RETURN_VOID);
            insn->set_arg_word_count(0);
          }
        }

        std::unordered_set<DexMethodRef*> pure_methods;
        auto local_dce = LocalDce(pure_methods);
        local_dce.dce(method->get_code());
        const auto& stats = local_dce.get_stats();
        if (stats.dead_instruction_count |
            stats.unreachable_instruction_count) {
          std::lock_guard<std::mutex> lock(local_dce_stats_mutex);
          local_dce_stats = add_dce_stats(local_dce_stats, stats);
        }
      }
    }
  });
  return method_stats;
}

/**
 * Removes dead arguments from the given invoke instr if applicable.
 * Returns the number of arguments removed.
 */
size_t RemoveArgs::update_callsite(IRInstruction* instr) {
  auto method_ref = instr->get_method();
  if (!method_ref->is_def()) {
    // TODO: T31388603 -- Remove unused args for true virtuals.
    return 0;
  };
  // Since is_def() is true, the following cast is safe and appropriate.
  DexMethod* method = static_cast<DexMethod*>(method_ref);

  auto kv_pair = m_live_arg_idxs_map.find(method);
  if (kv_pair == m_live_arg_idxs_map.end()) {
    // No removable arguments, so do nothing.
    return 0;
  }
  auto updated_srcs = kv_pair->second;
  for (size_t i = 0; i < updated_srcs.size(); ++i) {
    instr->set_src(i, instr->src(updated_srcs.at(i)));
  }
  always_assert_log(instr->srcs_size() > updated_srcs.size(),
                    "In RemoveArgs, callsites always update to fewer args\n");
  auto callsite_args_removed = instr->srcs_size() - updated_srcs.size();
  instr->set_arg_word_count(updated_srcs.size());
  return callsite_args_removed;
}

/**
 * Removes unused arguments at callsites and returns the number of arguments
 * removed.
 */
size_t RemoveArgs::update_callsites() {
  // Walk through all methods to look for and edit callsites.
  return walk::parallel::reduce_methods<size_t>(
      m_scope,
      [&](DexMethod* method) -> size_t {
        auto code = method->get_code();
        if (code == nullptr) {
          return 0;
        }
        size_t callsite_args_removed = 0;
        for (const auto& mie : InstructionIterable(code)) {
          auto insn = mie.insn;
          if (is_invoke(insn->opcode())) {
            size_t insn_args_removed = update_callsite(insn);
            if (insn_args_removed > 0) {
              log_opt(CALLSITE_ARGS_REMOVED, method, insn);
              callsite_args_removed += insn_args_removed;
            }
          }
        }
        return callsite_args_removed;
      },
      [](size_t a, size_t b) { return a + b; });
}

void RemoveUnusedArgsPass::run_pass(DexStoresVector& stores,
                                    ConfigFiles& /* conf */,
                                    PassManager& mgr) {
  auto scope = build_class_scope(stores);

  size_t num_callsite_args_removed = 0;
  size_t num_method_params_removed = 0;
  size_t num_methods_updated = 0;
  size_t num_method_results_removed_count = 0;
  size_t num_iterations = 0;
  LocalDce::Stats local_dce_stats{0, 0};
  while (true) {
    num_iterations++;
    RemoveArgs rm_args(scope, m_black_list, m_total_iterations++);
    auto pass_stats = rm_args.run();
    if (pass_stats.methods_updated_count == 0) {
      break;
    }
    num_callsite_args_removed += pass_stats.callsite_args_removed_count;
    num_method_params_removed += pass_stats.method_params_removed_count;
    num_methods_updated += pass_stats.methods_updated_count;
    num_method_results_removed_count += pass_stats.method_results_removed_count;
    local_dce_stats =
        add_dce_stats(local_dce_stats, pass_stats.local_dce_stats);
  }

  TRACE(ARGS,
        1,
        "Removed %d redundant callsite arguments",
        num_callsite_args_removed);
  TRACE(ARGS,
        1,
        "Removed %d redundant method parameters",
        num_method_params_removed);
  TRACE(ARGS,
        1,
        "Removed %d redundant method results",
        num_method_results_removed_count);
  TRACE(ARGS,
        1,
        "Updated %d methods with redundant parameters",
        num_methods_updated);

  mgr.set_metric(METRIC_CALLSITE_ARGS_REMOVED, num_callsite_args_removed);
  mgr.set_metric(METRIC_METHOD_PARAMS_REMOVED, num_method_params_removed);
  mgr.set_metric(METRIC_METHODS_UPDATED, num_methods_updated);
  mgr.set_metric(METRIC_METHOD_RESULTS_REMOVED,
                 num_method_results_removed_count);
  mgr.set_metric(METRIC_DEAD_INSTRUCTION_COUNT,
                 local_dce_stats.dead_instruction_count);
  mgr.set_metric(METRIC_UNREACHABLE_INSTRUCTION_COUNT,
                 local_dce_stats.unreachable_instruction_count);
  mgr.set_metric(METRIC_ITERATIONS, num_iterations);
}

static RemoveUnusedArgsPass s_pass;
} // namespace remove_unused_args
