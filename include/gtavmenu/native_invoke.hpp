#pragma once

// Short, unqualified call-site spellings for the templated native invokers in
// native_arg.hpp. Shared so the menu's two native-calling translation units
// (features.cpp, native_bridge.cpp) use one definition and one spelling instead
// of a per-file copy. native_arg.hpp owns the ABI struct and the raw invoker;
// this header is just the ergonomic wrapper everyone calls.

#include "gtavmenu/native_arg.hpp"

namespace gtavmenu {

template <typename... Args>
static inline void invoke_void(uint64_t address, Args... args) {
  invoke_native_void(address, args...);
}

template <typename R, typename... Args>
static inline R invoke_return(uint64_t address, Args... args) {
  return invoke_native_return<R>(address, args...);
}

}  // namespace gtavmenu
