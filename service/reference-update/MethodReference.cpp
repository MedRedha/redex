/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MethodReference.h"

#include "Resolver.h"
#include "Walkers.h"

namespace method_reference {

IRInstruction* make_load_const(uint16_t dest, size_t val) {
  auto load = new IRInstruction(OPCODE_CONST);
  load->set_dest(dest);
  load->set_literal(static_cast<int32_t>(val));
  return load;
}

IRInstruction* make_invoke(DexMethod* callee,
                           IROpcode opcode,
                           std::vector<uint16_t> args) {
  always_assert(callee->is_def() && is_public(callee));
  auto invoke = (new IRInstruction(opcode))->set_method(callee);
  invoke->set_arg_word_count(args.size());
  for (size_t i = 0; i < args.size(); i++) {
    invoke->set_src(i, args.at(i));
  }
  return invoke;
}

void patch_callsite(const CallSite& callsite, const NewCallee& new_callee) {
  if (is_static(new_callee.method) || is_any_init(new_callee.method) ||
      new_callee.method->is_virtual()) {
    set_public(new_callee.method);
  }
  always_assert_log(is_public(new_callee.method) ||
                        new_callee.method->get_class() ==
                            callsite.caller->get_class(),
                    "\tUpdating a callsite of %s when not accessible from %s\n",
                    SHOW(new_callee.method), SHOW(callsite.caller));

  auto code = callsite.caller->get_code();
  auto iterator = code->iterator_to(*callsite.mie);
  auto insn = callsite.mie->insn;
  if (new_callee.additional_args != boost::none) {
    const auto& args = new_callee.additional_args.get();
    auto old_size = insn->srcs_size();
    insn->set_arg_word_count(old_size + args.size());
    size_t pos = old_size;
    for (uint32_t arg : args) {
      auto reg = code->allocate_temp();
      // Seems it is different from dasm(OPCODE_CONST, {{VREG, reg}, {LITERAL,
      // arg}}) which will cause instruction_lowering crash. Why?
      auto load_const = make_load_const(reg, arg);
      code->insert_before(iterator, load_const);
      insn->set_src(pos++, reg);
    }
  }
  insn->set_method(new_callee.method);
  // Assuming the following move-result is there and good.
}

void update_call_refs_simple(
    const Scope& scope,
    const std::unordered_map<DexMethod*, DexMethod*>& old_to_new_callee) {
  auto patcher = [&](DexMethod* meth, IRCode& code) {
    for (auto& mie : InstructionIterable(code)) {
      auto insn = mie.insn;
      if (!insn->has_method()) {
        continue;
      }
      const auto method =
          resolve_method(insn->get_method(), opcode_to_search(insn));
      if (method == nullptr || old_to_new_callee.count(method) == 0) {
        continue;
      }
      auto new_callee = old_to_new_callee.at(method);
      // At this point, a non static private should not exist.
      always_assert_log(!is_private(new_callee) || is_static(new_callee),
                        "%s\n",
                        vshow(new_callee).c_str());
      TRACE(REFU, 9, " Updated call %s to %s", SHOW(insn), SHOW(new_callee));
      insn->set_method(new_callee);
      if (new_callee->is_virtual()) {
        always_assert_log(is_invoke_virtual(insn->opcode()),
                          "invalid callsite %s\n",
                          SHOW(insn));
      } else if (is_static(new_callee)) {
        always_assert_log(is_invoke_static(insn->opcode()),
                          "invalid callsite %s\n",
                          SHOW(insn));
      }
    }
  };
  walk::parallel::code(scope, patcher);
}

CallSites collect_call_refs(const Scope& scope,
                            const MethodOrderedSet& callees) {
  if (callees.empty()) {
    CallSites empty;
    return empty;
  }
  auto patcher = [&](DexMethod* caller) {
    CallSites call_sites;
    auto code = caller->get_code();
    if (!code) {
      return call_sites;
    }

    for (auto& mie : InstructionIterable(caller->get_code())) {
      auto insn = mie.insn;
      if (!insn->has_method()) {
        continue;
      }

      const auto callee =
          resolve_method(insn->get_method(),
                         opcode_to_search(const_cast<IRInstruction*>(insn)));
      if (callee == nullptr || callees.count(callee) == 0) {
        continue;
      }

      call_sites.emplace_back(caller, &mie, callee);
      TRACE(REFU, 9, "  Found call %s from %s", SHOW(insn), SHOW(caller));
    }

    return call_sites;
  };

  CallSites call_sites = walk::parallel::reduce_methods<CallSites>(
      scope, patcher, [](CallSites left, CallSites right) {
        left.insert(left.end(), right.begin(), right.end());
        return left;
      });
  return call_sites;
}
} // namespace method_reference
