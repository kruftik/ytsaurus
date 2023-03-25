// Copyright 2022 The Abseil Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef Y_ABSL_STRINGS_INTERNAL_HAS_ABSL_STRINGIFY_H_
#define Y_ABSL_STRINGS_INTERNAL_HAS_ABSL_STRINGIFY_H_
#include <util/generic/string.h>
#include <type_traits>
#include <utility>

#include "y_absl/strings/string_view.h"

namespace y_absl {
Y_ABSL_NAMESPACE_BEGIN

namespace strings_internal {

// This is an empty class not intended to be used. It exists so that
// `HasAbslStringify` can reference a universal class rather than needing to be
// copied for each new sink.
class UnimplementedSink {
 public:
  void Append(size_t count, char ch);

  void Append(string_view v);

  // Support `y_absl::Format(&sink, format, args...)`.
  friend void AbslFormatFlush(UnimplementedSink* sink, y_absl::string_view v);
};

template <typename T, typename = void>
struct HasAbslStringify : std::false_type {};

template <typename T>
struct HasAbslStringify<
    T, std::enable_if_t<std::is_void<decltype(AbslStringify(
           std::declval<strings_internal::UnimplementedSink&>(),
           std::declval<const T&>()))>::value>> : std::true_type {};

}  // namespace strings_internal

Y_ABSL_NAMESPACE_END
}  // namespace y_absl

#endif  // Y_ABSL_STRINGS_INTERNAL_HAS_ABSL_STRINGIFY_H_
