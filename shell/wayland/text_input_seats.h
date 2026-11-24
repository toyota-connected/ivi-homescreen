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

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <wl/ime/text_input_receiver.hpp>

// One text input per seat. zwp_text_input_v3 belongs to a seat: the compositor
// sends enter on the text input of the seat whose keyboard focus is on the
// surface, and the input method that seat runs answers through it. So with
// more than one seat -- weston's RDP and VNC backends add one per remote
// client -- each needs its own, or text input only follows one of them.
//
// What the engine asks for goes to every seat's text input. Only one whose
// seat has focus has entered and is enabled; the others hold the state and
// apply it if their seat takes focus. The last request is kept, so a seat that
// arrives while a field is focused, or one whose text input enters after the
// request was sent, gets it too.
//
// @p Backend is wl::ime::SelectedTextInput, or a fake in the tests. Reactor
// thread only, like the proxies it drives.
template <typename Backend>
class TextInputSeats {
 public:
  // Adds @p backend, already bound, as @p seat's text input. When a field is
  // focused it is activated with the current state.
  void Add(const void* seat, std::unique_ptr<Backend> backend) {
    if (backend == nullptr) {
      return;
    }
    Remove(seat);
    Backend& b = *backend;
    seats_.emplace_back(seat, std::move(backend));
    if (state_.active) {
      b.Activate();
      Send(b);
    }
  }

  // Drops @p seat's text input, before the seat itself is released.
  void Remove(const void* seat) {
    for (auto it = seats_.begin(); it != seats_.end(); ++it) {
      if (it->first == seat) {
        seats_.erase(it);
        return;
      }
    }
  }

  void Clear() { seats_.clear(); }

  [[nodiscard]] size_t size() const { return seats_.size(); }

  [[nodiscard]] bool Has(const void* seat) const {
    for (const auto& [s, b] : seats_) {
      if (s == seat) {
        return true;
      }
    }
    return false;
  }

  // A field is focused: enable with its content type and cursor rectangle.
  void Activate(const wl::ime::ContentHint hint,
                const wl::ime::ContentPurpose purpose,
                const int32_t x,
                const int32_t y,
                const int32_t width,
                const int32_t height) {
    state_ = State{};
    state_.active = true;
    state_.hint = hint;
    state_.purpose = purpose;
    state_.rect = {x, y, width, height};
    for (auto& [seat, b] : seats_) {
      b->Activate();
      Send(*b);
    }
  }

  // The field lost focus.
  void Deactivate() {
    state_ = State{};
    for (auto& [seat, b] : seats_) {
      b->Deactivate();
      b->Commit();
    }
  }

  void SetCursorRectangle(const int32_t x,
                          const int32_t y,
                          const int32_t width,
                          const int32_t height) {
    state_.rect = {x, y, width, height};
    for (auto& [seat, b] : seats_) {
      b->SetCursorRectangle(x, y, width, height);
      b->Commit();
    }
  }

  void SetSurroundingText(const std::string_view utf8,
                          const uint32_t cursor,
                          const uint32_t anchor) {
    state_.surrounding.assign(utf8);
    state_.cursor = cursor;
    state_.anchor = anchor;
    state_.has_surrounding = true;
    for (auto& [seat, b] : seats_) {
      b->SetSurroundingText(utf8, cursor, anchor);
      b->Commit();
    }
  }

  // A text input entered. The backend has just made its enable pending, and
  // enable resets every field, so the state goes again with the commit that
  // applies it. Which seat entered is not reported, so every text input gets
  // it; the others only see their pending state replaced by the same values.
  void OnEnter() {
    if (!state_.active) {
      return;
    }
    for (auto& [seat, b] : seats_) {
      Send(*b);
    }
  }

  [[nodiscard]] bool active() const { return state_.active; }

 private:
  struct Rect {
    int32_t x{0};
    int32_t y{0};
    int32_t width{0};
    int32_t height{0};
  };
  struct State {
    bool active{false};
    wl::ime::ContentHint hint{};
    wl::ime::ContentPurpose purpose{};
    Rect rect{};
    std::string surrounding;
    uint32_t cursor{0};
    uint32_t anchor{0};
    bool has_surrounding{false};
  };

  // Everything enable resets, then the commit that applies it.
  void Send(Backend& b) const {
    b.SetContentType(state_.hint, state_.purpose);
    b.SetCursorRectangle(state_.rect.x, state_.rect.y, state_.rect.width,
                         state_.rect.height);
    if (state_.has_surrounding) {
      b.SetSurroundingText(state_.surrounding, state_.cursor, state_.anchor);
    }
    b.Commit();
  }

  std::vector<std::pair<const void*, std::unique_ptr<Backend>>> seats_;
  State state_;
};
