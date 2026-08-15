/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SHELL_ACCESSIBILITY_SEMANTICS_ACTION_ARGS_H_
#define SHELL_ACCESSIBILITY_SEMANTICS_ACTION_ARGS_H_

#include <cstdint>
#include <optional>
#include <vector>

namespace accessibility {

/*
 * Turns a hub action argument into the StandardMessageCodec message the
 * framework decodes.
 *
 * The split is deliberate. The hub is Flutter-neutral by construction, so its
 * `data` carries the argument in a plain layout of its own (see
 * ihs_semantics.h) and the encoding happens here, on the only side that
 * already links Flutter. A consumer -- the MCP provider, AccessKit -- then
 * needs no codec at all, and the hub ABI does not have Flutter's wire format
 * baked into it.
 *
 * Uses the vendored flutter::StandardMessageCodec rather than a second
 * implementation: a reimplementation can drift from the decoder silently, and
 * the framework then drops the argument with no error raised anywhere -- the
 * action appears to have been delivered and simply does nothing.
 *
 * Returns the encoded message, or nullopt when `data` does not match the
 * layout the action requires -- a truncated offset or a stray byte count is a
 * caller error, and forwarding it would reach the framework as a silently
 * ignored action rather than a refusal.
 *
 * An action that takes no argument returns an empty vector for null/0 input,
 * so the caller can encode unconditionally.
 */
std::optional<std::vector<uint8_t>> EncodeActionArguments(uint64_t action,
                                                          const uint8_t* data,
                                                          size_t data_length);

}  // namespace accessibility

#endif  // SHELL_ACCESSIBILITY_SEMANTICS_ACTION_ARGS_H_
