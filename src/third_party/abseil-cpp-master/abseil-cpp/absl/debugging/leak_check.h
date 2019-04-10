// Copyright 2018 The Abseil Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// -----------------------------------------------------------------------------
// File: leak_check.h
// -----------------------------------------------------------------------------
//
// This file contains functions that affect leak checking behavior within
// targets built with the LeakSanitizer (LSan), a memory leak detector that is
// integrated within the AddressSanitizer (ASan) as an additional component, or
// which can be used standalone. LSan and ASan are included (or can be provided)
// as additional components for most compilers such as Clang, gcc and MSVC.
// Note: this leak checking API is not yet supported in MSVC.
// Leak checking is enabled by default in all ASan builds.
//
// See https://github.com/google/sanitizers/wiki/AddressSanitizerLeakSanitizer
//
// -----------------------------------------------------------------------------
#ifndef ABSL_DEBUGGING_LEAK_CHECK_H_
#define ABSL_DEBUGGING_LEAK_CHECK_H_

#include <cstddef>

namespace absl {

// HaveLeakSanitizer()
//
// Returns true if a leak-checking sanitizer (either ASan or standalone LSan) is
// currently built into this target.
bool HaveLeakSanitizer();

// DoIgnoreLeak()
//
// Implements `IgnoreLeak()` below. This function should usually
// not be called directly; calling `IgnoreLeak()` is preferred.
void DoIgnoreLeak(const void* ptr);

// IgnoreLeak()
//
// Instruct the leak sanitizer to ignore leak warnings on the object referenced
// by the passed pointer, as well as all heap objects transitively referenced
// by it. The passed object pointer can point to either the beginning of the
// object or anywhere within it.
//
// Example:
//
//   static T* obj = IgnoreLeak(new T(...));
//
// If the passed `ptr` does not point to an actively allocated object at the
// time `IgnoreLeak()` is called, the call is a no-op; if it is actively
// allocated, the object must not get deallocated later.
//
template <typename T>
T* IgnoreLeak(T* ptr) {
  DoIgnoreLeak(ptr);
  return ptr;
}

// LeakCheckDisabler
//
// This helper class indicates that any heap allocations done in the code block
// covered by the scoped object, which should be allocated on the stack, will
// not be reported as leaks. Leak check disabling will occur within the code
// block and any nested function calls within the code block.
//
// Example:
//
//   void Foo() {
//     LeakCheckDisabler disabler;
//     ... code that allocates objects whose leaks should be ignored ...
//   }
//
// REQUIRES: Destructor runs in same thread as constructor
class LeakCheckDisabler {
 public:
  LeakCheckDisabler();
  LeakCheckDisabler(const LeakCheckDisabler&) = delete;
  LeakCheckDisabler& operator=(const LeakCheckDisabler&) = delete;
  ~LeakCheckDisabler();
};

// RegisterLivePointers()
//
// Registers `ptr[0,size-1]` as pointers to memory that is still actively being
// referenced and for which leak checking should be ignored. This function is
// useful if you store pointers in mapped memory, for memory ranges that we know
// are correct but for which normal analysis would flag as leaked code.
void RegisterLivePointers(const void* ptr, size_t size);

// UnRegisterLivePointers()
//
// Deregisters the pointers previously marked as active in
// `RegisterLivePointers()`, enabling leak checking of those pointers.
void UnRegisterLivePointers(const void* ptr, size_t size);

}  // namespace absl

#endif  // ABSL_DEBUGGING_LEAK_CHECK_H_
