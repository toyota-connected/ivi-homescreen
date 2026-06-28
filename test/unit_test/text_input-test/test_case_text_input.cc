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

// Unit tests for the compositor IME (zwp_text_input_v3) text-plumbing path of
// flutter::TextInputPlugin. The plugin is driven through its public surface
// without a compositor or a font: inbound IME edits (CommitString /
// SetPreeditString / DeleteSurrounding) and framework method calls
// (TextInput.setClient / setEditingState / show / setEditableSizeAndTransform)
// are simulated, and the plugin's two outputs are checked:
//   1. The surrounding-text callback (plugin -> compositor IME), which carries
//      committed UTF-8 text plus UTF-8 byte offsets for cursor and anchor. This
//      is the primary oracle for byte-offset correctness across multibyte and
//      emoji input.
//   2. The outgoing TextInputClient.updateEditingState method call
//      (plugin -> framework), decoded back from the binary messenger, which
//      carries model text and UTF-16 selection / composing ranges.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "flutter/shell/platform/common/json_method_codec.h"
#include "gtest/gtest.h"
#include "shell/platform/homescreen/text_input_plugin.h"

namespace {

constexpr char kTextInputChannel[] = "flutter/textinput";
constexpr char kUpdateEditingStateMethod[] =
    "TextInputClient.updateEditingState";

// A four-byte UTF-8 / surrogate-pair codepoint: U+1F923 ROLLING ON THE FLOOR
// LAUGHING. One Unicode codepoint, four UTF-8 bytes, two UTF-16 code units.
constexpr char kEmoji[] = "\xF0\x9F\xA4\xA3";

// One record captured from the plugin's surrounding-text callback.
struct SurroundingRecord {
  std::string text;
  uint32_t cursor;
  uint32_t anchor;
};

// One record captured from the plugin's cursor-rectangle callback.
struct CursorRectRecord {
  int32_t x;
  int32_t y;
  int32_t w;
  int32_t h;
};

// The decoded contents of a TextInputClient.updateEditingState method call.
struct EditingState {
  std::string text;
  int selection_base;
  int selection_extent;
  int composing_base;
  int composing_extent;
};

// A minimal flutter::BinaryMessenger that captures the inbound method-call
// handler the plugin registers on "flutter/textinput" and records the binary
// payloads the plugin sends back on the same channel so the test can decode the
// plugin's outgoing TextInputClient.updateEditingState calls.
class StubBinaryMessenger : public flutter::BinaryMessenger {
 public:
  void Send(const std::string& channel,
            const uint8_t* message,
            size_t message_size,
            flutter::BinaryReply /*reply*/ = nullptr) const override {
    if (channel == kTextInputChannel) {
      sent_.emplace_back(message, message + message_size);
    }
  }

  void SetMessageHandler(const std::string& channel,
                         flutter::BinaryMessageHandler handler) override {
    if (channel == kTextInputChannel) {
      handler_ = std::move(handler);
    }
  }

  // The handler the plugin registered for "flutter/textinput".
  flutter::BinaryMessageHandler handler_;

  // Raw payloads the plugin sent on "flutter/textinput", in order. Mutable
  // because BinaryMessenger::Send is const.
  mutable std::vector<std::vector<uint8_t>> sent_;
};

class TextInputTest : public ::testing::Test {
 protected:
  void SetUp() override {
    plugin_ = std::make_unique<flutter::TextInputPlugin>(&messenger_);

    plugin_->SetImeActivate([this](uint32_t, uint32_t, int32_t, int32_t,
                                   int32_t, int32_t) { ++activate_count_; });
    plugin_->SetImeDeactivate([this]() { ++deactivate_count_; });
    plugin_->SetImeUpdateCursorRect(
        [this](int32_t x, int32_t y, int32_t w, int32_t h) {
          cursor_rects_.push_back({x, y, w, h});
        });
    plugin_->SetImeUpdateSurrounding(
        [this](const std::string& utf8, uint32_t cursor, uint32_t anchor) {
          surrounding_.push_back({utf8, cursor, anchor});
        });
  }

  // ---- framework -> plugin method-channel drivers --------------------------

  void Dispatch(const std::string& method,
                std::unique_ptr<rapidjson::Document> args) {
    flutter::MethodCall<rapidjson::Document> call(method, std::move(args));
    const auto encoded =
        flutter::JsonMethodCodec::GetInstance().EncodeMethodCall(call);
    ASSERT_TRUE(messenger_.handler_) << "plugin never registered a handler";
    messenger_.handler_(encoded->data(), encoded->size(),
                        [](const uint8_t*, size_t) {});
  }

  void SetClient(int client_id,
                 const std::string& input_type = "TextInputType.text",
                 const std::string& input_action = "TextInputAction.done") {
    auto doc = std::make_unique<rapidjson::Document>(rapidjson::kArrayType);
    auto& alloc = doc->GetAllocator();
    doc->PushBack(client_id, alloc);
    rapidjson::Value config(rapidjson::kObjectType);
    rapidjson::Value type(rapidjson::kObjectType);
    type.AddMember("name", rapidjson::Value(input_type, alloc).Move(), alloc);
    config.AddMember("inputType", type, alloc);
    config.AddMember("inputAction",
                     rapidjson::Value(input_action, alloc).Move(), alloc);
    doc->PushBack(config, alloc);
    Dispatch("TextInput.setClient", std::move(doc));
  }

  void SetEditingState(const std::string& text,
                       int selection_base,
                       int selection_extent,
                       int composing_base = -1,
                       int composing_extent = -1) {
    auto doc = std::make_unique<rapidjson::Document>(rapidjson::kObjectType);
    auto& alloc = doc->GetAllocator();
    doc->AddMember("text", rapidjson::Value(text, alloc).Move(), alloc);
    doc->AddMember("selectionBase", selection_base, alloc);
    doc->AddMember("selectionExtent", selection_extent, alloc);
    doc->AddMember("composingBase", composing_base, alloc);
    doc->AddMember("composingExtent", composing_extent, alloc);
    Dispatch("TextInput.setEditingState", std::move(doc));
  }

  void Show() { Dispatch("TextInput.show", nullptr); }

  // Sends a column-major 4x4 transform whose translation (elements 12/13) is
  // (x, y), plus the editable box size.
  void SetEditableSizeAndTransform(int x, int y, int w, int h) {
    auto doc = std::make_unique<rapidjson::Document>(rapidjson::kObjectType);
    auto& alloc = doc->GetAllocator();
    doc->AddMember("width", static_cast<double>(w), alloc);
    doc->AddMember("height", static_cast<double>(h), alloc);
    rapidjson::Value transform(rapidjson::kArrayType);
    for (int i = 0; i < 16; ++i) {
      double v = 0.0;
      if (i == 0 || i == 5 || i == 10 || i == 15) {
        v = 1.0;  // identity scale
      } else if (i == 12) {
        v = static_cast<double>(x);
      } else if (i == 13) {
        v = static_cast<double>(y);
      }
      transform.PushBack(v, alloc);
    }
    doc->AddMember("transform", transform, alloc);
    Dispatch("TextInput.setEditableSizeAndTransform", std::move(doc));
  }

  // ---- plugin -> framework decode ------------------------------------------

  // Decodes the most recent TextInputClient.updateEditingState the plugin sent.
  std::optional<EditingState> LastEditingState() const {
    for (auto it = messenger_.sent_.rbegin(); it != messenger_.sent_.rend();
         ++it) {
      auto call = flutter::JsonMethodCodec::GetInstance().DecodeMethodCall(*it);
      if (!call || call->method_name() != kUpdateEditingStateMethod) {
        continue;
      }
      const rapidjson::Document* args = call->arguments();
      if (!args || !args->IsArray() || args->Size() < 2) {
        continue;
      }
      const rapidjson::Value& state = (*args)[1];
      EditingState e;
      e.text = state["text"].GetString();
      e.selection_base = state["selectionBase"].GetInt();
      e.selection_extent = state["selectionExtent"].GetInt();
      e.composing_base = state["composingBase"].GetInt();
      e.composing_extent = state["composingExtent"].GetInt();
      return e;
    }
    return std::nullopt;
  }

  StubBinaryMessenger messenger_;
  std::unique_ptr<flutter::TextInputPlugin> plugin_;

  std::vector<SurroundingRecord> surrounding_;
  std::vector<CursorRectRecord> cursor_rects_;
  int activate_count_ = 0;
  int deactivate_count_ = 0;
};

}  // namespace

// 1. A plain-ASCII commit into an empty field surfaces both as the model's
//    UTF-16 editing state and as a byte-accurate surrounding-text update.
TEST_F(TextInputTest, CommitAsciiUpdatesModelAndSurrounding) {
  SetClient(1);
  Show();
  ASSERT_GE(activate_count_, 1);

  plugin_->CommitString("hello");

  const auto state = LastEditingState();
  ASSERT_TRUE(state.has_value());
  EXPECT_EQ(state->text, "hello");
  EXPECT_EQ(state->selection_base, 5);
  EXPECT_EQ(state->selection_extent, 5);

  ASSERT_FALSE(surrounding_.empty());
  EXPECT_EQ(surrounding_.back().text, "hello");
  EXPECT_EQ(surrounding_.back().cursor, 4u + 1u);  // 5 ASCII bytes
  EXPECT_EQ(surrounding_.back().anchor, 5u);
}

// 2a. A single four-byte emoji commit reports a cursor byte offset of 4 (not 1
//     codepoint, not 2 UTF-16 units) and a UTF-16 selection of 2.
TEST_F(TextInputTest, CommitEmojiByteOffsetIsFour) {
  SetClient(1);
  Show();

  plugin_->CommitString(kEmoji);

  const auto state = LastEditingState();
  ASSERT_TRUE(state.has_value());
  EXPECT_EQ(state->text, std::string(kEmoji));
  EXPECT_NE(state->text.find(kEmoji), std::string::npos);
  EXPECT_EQ(state->selection_extent, 2);  // surrogate pair = 2 UTF-16 units

  ASSERT_FALSE(surrounding_.empty());
  EXPECT_EQ(surrounding_.back().text, std::string(kEmoji));
  EXPECT_EQ(surrounding_.back().cursor, 4u);  // four UTF-8 bytes, not 1 or 2
  EXPECT_EQ(surrounding_.back().anchor, 4u);
}

// 2b. A mixed ASCII + emoji + ASCII commit ("a" + emoji + "b") reports a cursor
//     byte offset of 6 (1 + 4 + 1) and a UTF-16 selection of 4 (1 + 2 + 1).
TEST_F(TextInputTest, CommitMixedAsciiEmojiByteOffsets) {
  SetClient(1);
  Show();

  plugin_->CommitString(std::string("a") + kEmoji + "b");

  const auto state = LastEditingState();
  ASSERT_TRUE(state.has_value());
  EXPECT_EQ(state->text, std::string("a") + kEmoji + "b");
  EXPECT_EQ(state->selection_extent, 4);  // a(1) + surrogate-pair(2) + b(1)

  ASSERT_FALSE(surrounding_.empty());
  EXPECT_EQ(surrounding_.back().text, std::string("a") + kEmoji + "b");
  EXPECT_EQ(surrounding_.back().cursor, 6u);  // 1 + 4 + 1 UTF-8 bytes
  EXPECT_EQ(surrounding_.back().anchor, 6u);
}

// 3. A commit replaces (does not append after) an active preedit: the composing
//    run is discarded, the commit text becomes the only committed text, and the
//    composing range is cleared.
TEST_F(TextInputTest, CommitReplacesActivePreedit) {
  SetClient(1);
  Show();

  plugin_->SetPreeditString("o", 0, 1);
  plugin_->CommitString("off plan ");

  const auto state = LastEditingState();
  ASSERT_TRUE(state.has_value());
  EXPECT_EQ(state->text, "off plan ");  // not "ooff plan " / "ooff plan o"
  EXPECT_EQ(state->composing_base, -1);
  EXPECT_EQ(state->composing_extent, -1);
}

// 4. An empty preedit clears an active composing run without committing it; the
//    committed text is left untouched and the composing range is cleared.
TEST_F(TextInputTest, EmptyPreeditClearsComposing) {
  SetClient(1);
  SetEditingState("ab", 2, 2);
  Show();

  plugin_->SetPreeditString("o", 0, 1);  // text becomes "abo", composing [2,3]
  plugin_->SetPreeditString("", 0, 0);   // clears composing, text back to "ab"

  const auto state = LastEditingState();
  ASSERT_TRUE(state.has_value());
  EXPECT_EQ(state->text, "ab");
  EXPECT_EQ(state->composing_base, -1);
  EXPECT_EQ(state->composing_extent, -1);
}

// 5. The surrounding text sent to the IME excludes the active preedit: with
//    committed "ab" and a composing "XY", the IME sees only "ab" with the
//    cursor at byte offset 2.
TEST_F(TextInputTest, SurroundingExcludesActivePreedit) {
  SetClient(1);
  SetEditingState("ab", 2, 2);
  Show();

  plugin_->SetPreeditString("XY", 0, 2);

  ASSERT_FALSE(surrounding_.empty());
  EXPECT_EQ(surrounding_.back().text, "ab");  // not "abXY"
  EXPECT_EQ(surrounding_.back().cursor, 2u);
  EXPECT_EQ(surrounding_.back().anchor, 2u);
}

// 6. DeleteSurrounding(before, after) removes committed text relative to the
//    cursor.
TEST_F(TextInputTest, DeleteSurroundingRemovesBeforeCursor) {
  SetClient(1);
  SetEditingState("abcd", 4, 4);
  Show();

  plugin_->DeleteSurrounding(2, 0);

  const auto state = LastEditingState();
  ASSERT_TRUE(state.has_value());
  EXPECT_EQ(state->text, "ab");
}

// 7. Surrounding-text sends are de-duplicated: an edit that yields an identical
//    (text, cursor, anchor) does not re-invoke the callback, but a
//    re-activation via TextInput.show invalidates the cache so the next send
//    goes through.
TEST_F(TextInputTest, SurroundingDedupeAndReactivationInvalidates) {
  SetClient(1);
  SetEditingState("hello", 5, 5);
  Show();

  const size_t before_noop = surrounding_.size();
  // A no-op preedit clear while not composing recomputes the same surrounding
  // text and must be suppressed.
  plugin_->SetPreeditString("", 0, 0);
  EXPECT_EQ(surrounding_.size(), before_noop)
      << "identical surrounding text must not be re-sent";

  // Re-activation invalidates the dedupe cache; the seed send goes through even
  // though the surrounding text is unchanged.
  Show();
  EXPECT_GT(surrounding_.size(), before_noop)
      << "re-activation must invalidate the surrounding cache";
}

// 8. Cursor-rectangle sends are de-duplicated: two identical
//    setEditableSizeAndTransform calls invoke the callback once, and a changed
//    rectangle invokes it again.
TEST_F(TextInputTest, CursorRectDedupe) {
  SetClient(1);

  SetEditableSizeAndTransform(10, 20, 100, 30);
  SetEditableSizeAndTransform(10, 20, 100, 30);
  ASSERT_EQ(cursor_rects_.size(), 1u);
  EXPECT_EQ(cursor_rects_.back().x, 10);
  EXPECT_EQ(cursor_rects_.back().y, 20);
  EXPECT_EQ(cursor_rects_.back().w, 100);
  EXPECT_EQ(cursor_rects_.back().h, 30);

  SetEditableSizeAndTransform(11, 20, 100, 30);
  EXPECT_EQ(cursor_rects_.size(), 2u);
  EXPECT_EQ(cursor_rects_.back().x, 11);
}

// 9. Inbound IME edits with no active client are safe no-ops: no crash and no
//    callback invocations.
TEST_F(TextInputTest, CommitWithoutClientIsNoop) {
  plugin_->CommitString("hello");
  plugin_->SetPreeditString("o", 0, 1);
  plugin_->DeleteSurrounding(1, 0);

  EXPECT_TRUE(surrounding_.empty());
  EXPECT_TRUE(messenger_.sent_.empty());
}
