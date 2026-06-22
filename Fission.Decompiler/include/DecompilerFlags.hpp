#pragma once
#include <cstdint>

enum class DecompilerFlags : uint32_t {
      None = 0,
      PrintIR = 1 << 0,
      WriteIRToFile = 1 << 1,
      GenerateIRGraph = 1 << 2,
      GenerateSSAIRGraph = 1 << 3,
      PrintTimingBreakdown = 1 << 4,
      InferTypes = 1 << 5,
      OptimizeIR = 1 << 6,
      InferRobloxTypes = 1 << 7,
      AutoNameVariables = 1 << 8,
      // drop Fission's info comments (function info, capture/name notes). warnings + banner still emitted.
      OmitFissionComments = 1 << 9,
      // added
      Upvalues = 1 << 10,
      MinifyUpvalues = 1 << 11,
      DebugInfo = 1 << 12,
      FunctionInfo = 1 << 13,
      Constants = 1 << 14,
      MinifyConstants = 1 << 15,
      Globals = 1 << 16,
      MinifyGlobals = 1 << 17,
      Protos = 1 << 18,
      MinifyProtos = 1 << 19,
      Semicolons = 1 << 20,
      CallLineInfo = 1 << 21,
};

constexpr DecompilerFlags operator|(DecompilerFlags lhs, DecompilerFlags rhs) {
      return static_cast<DecompilerFlags>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
}

constexpr DecompilerFlags operator&(DecompilerFlags lhs, DecompilerFlags rhs) {
      return static_cast<DecompilerFlags>(static_cast<uint32_t>(lhs) & static_cast<uint32_t>(rhs));
}

constexpr DecompilerFlags operator~(DecompilerFlags flag) { return static_cast<DecompilerFlags>(~static_cast<uint32_t>(flag)); }

inline DecompilerFlags &operator|=(DecompilerFlags &lhs, DecompilerFlags rhs) {
      lhs = lhs | rhs;
      return lhs;
}

inline DecompilerFlags &operator&=(DecompilerFlags &lhs, DecompilerFlags rhs) {
      lhs = lhs & rhs;
      return lhs;
}