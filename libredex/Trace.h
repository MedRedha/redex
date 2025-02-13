/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <mutex>
#include <string>

#include "Util.h"

#define TMS          \
  TM(ADD_REDEX_TXT)  \
  TM(ACCESS)         \
  TM(ANNO)           \
  TM(ARGS)           \
  TM(BBPROFILE)      \
  TM(BIND)           \
  TM(BPH)            \
  TM(BRIDGE)         \
  TM(BUILDERS)       \
  TM(CFG)            \
  TM(CFP)            \
  TM(CLP_GQL)        \
  TM(CLP_LITHO)      \
  TM(CONSTP)         \
  TM(CPG)            \
  TM(CS)             \
  TM(CSE)            \
  TM(CUSTOMSORT)     \
  TM(DBGSTRIP)       \
  TM(DC)             \
  TM(DCE)            \
  TM(DEDUP_BLOCKS)   \
  TM(DEDUP_RES)      \
  TM(DELINIT)        \
  TM(DELMET)         \
  TM(DRAC)           \
  TM(DS)             \
  TM(EMPTY)          \
  TM(ENUM)           \
  TM(FINALINLINE)    \
  TM(HASHER)         \
  TM(HOTNESS)        \
  TM(ICONSTP)        \
  TM(IDEX)           \
  TM(INLINE)         \
  TM(GETTER)         \
  TM(INL)            \
  TM(INLRES)         \
  TM(INTRO_SWITCH)   \
  TM(INSTRUMENT)     \
  TM(INTF)           \
  TM(BLD_PATTERN)    \
  TM(LIB)            \
  TM(LOC)            \
  TM(MAGIC_FIELDS)   \
  TM(MAIN)           \
  TM(MARIANA_TRENCH) \
  TM(METH_DEDUP)     \
  TM(METH_MERGER)    \
  TM(MMINL)          \
  TM(MMODE)          \
  TM(MORTIROLO)      \
  TM(MTRANS)         \
  TM(OBFUSCATE)      \
  TM(OPTRES)         \
  TM(ORIGINALNAME)   \
  TM(OSDCE)          \
  TM(OUTLINE)        \
  TM(PEEPHOLE)       \
  TM(PGR)            \
  TM(PM)             \
  TM(PTA)            \
  TM(QUICK)          \
  TM(RAL)            \
  TM(REDEX)          \
  TM(REACH)          \
  TM(REACH_DUMP)     \
  TM(REFU)           \
  TM(REFL)           \
  TM(REG)            \
  TM(RELO)           \
  TM(RENAME)         \
  TM(RG)             \
  TM(RME)            \
  TM(RMGOTO)         \
  TM(RMU)            \
  TM(RMUF)           \
  TM(RM_INTF)        \
  TM(RP)             \
  TM(SDIS)           \
  TM(SHORTEN)        \
  TM(SINK)           \
  TM(SPLIT_RES)      \
  TM(STATIC_RELO)    \
  TM(STRBUILD)       \
  TM(STR_CAT)        \
  TM(STR_SIMPLE)     \
  TM(SUPER)          \
  TM(SWITCH_EQUIV)   \
  TM(SYNT)           \
  TM(TIME)           \
  TM(TRACKRESOURCES) \
  TM(TYPE)           \
  TM(UCM)            \
  TM(UNTF)           \
  TM(VERIFY)         \
  TM(VMERGE)         \
  TM(VIRT)           \
  TM(TERA)           \
  TM(BRCR)           \
  TM(SWIN)           \
  TM(SWCL)           \
  TM(SW)             \
  TM(IFCS_ANALYSIS)  \
  TM(UNREF_INTF)     \
  TM(OPT_STORES)     \
  TM(MEINT)          \
  TM(OPUT)           \
  TM(IODI)           \
  TM(MODULARITY)     \
  TM(VM)

enum TraceModule : int {
#define TM(x) x,
  TMS
#undef TM
      N_TRACE_MODULES,
};

bool traceEnabled(TraceModule module, int level);
#ifdef NDEBUG
#define TRACE(...)
#define TRACE_NO_LINE(...)
#else
void trace(
    TraceModule module, int level, bool suppress_newline, const char* fmt, ...);
#define TRACE(module, level, fmt, ...)                                        \
  do {                                                                        \
    if (traceEnabled(module, level)) {                                        \
      trace(module, level, /* suppress_newline */ false, fmt, ##__VA_ARGS__); \
    }                                                                         \
  } while (0)
#define TRACE_NO_LINE(module, level, fmt, ...)                               \
  do {                                                                       \
    if (traceEnabled(module, level)) {                                       \
      trace(module, level, /* suppress_newline */ true, fmt, ##__VA_ARGS__); \
    }                                                                        \
  } while (0)
#endif // NDEBUG

struct TraceContext {
  explicit TraceContext(const std::string& current_method) {
    s_current_method = &current_method;
  }
  ~TraceContext() { s_current_method = nullptr; }

  thread_local static const std::string* s_current_method;
  static std::mutex s_trace_mutex;
};
