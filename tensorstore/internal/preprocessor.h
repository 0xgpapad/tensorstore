// Copyright 2020 The TensorStore Authors
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

#ifndef TENSORSTORE_INTERNAL_PREPROCESSOR_H_
#define TENSORSTORE_INTERNAL_PREPROCESSOR_H_

/// \file
/// Defines preprocessor metaprogramming utilities.

/// Triggers another preprocessor scan.  The second macro
/// `TENSORSTORE_PP_EXPAND1` is provided in case nested uses are required.
#define TENSORSTORE_PP_EXPAND(...) __VA_ARGS__
#define TENSORSTORE_PP_EXPAND1(...) __VA_ARGS__

#define TENSORSTORE_PP_EMPTY(...)

/// Defers macro expansion until the next scan.
#define TENSORSTORE_PP_DEFER(...) __VA_ARGS__ TENSORSTORE_PP_EMPTY()

/// Behaves like `a ## b`, but does not inhibit expansion of `a` or `b`.
///
/// This is useful for concatenating a token with `__LINE__` or `__COUNTER__`.
#define TENSORSTORE_PP_CAT(a, b) TENSORSTORE_INTERNAL_PP_CAT1(a, b)

// Since ## inhibits macro expansion, we need an extra level of indirection to
// force expansion.
#define TENSORSTORE_INTERNAL_PP_CAT1(a, b) a##b

#define TENSORSTORE_PP_COMMA() ,

#endif  // TENSORSTORE_INTERNAL_PREPROCESSOR_H_
