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

/*
 * The dev.osgi/bridge channel handler.
 *
 * WHY THROUGH THE CODEC
 *
 * HandleMethodCall is private and static, so a test cannot call it. That is not
 * a limitation worth working around: driving the plugin through a stub
 * BinaryMessenger with StandardMethodCodec-encoded messages covers the decode
 * path as well, and the decode path is where the plugin's own comment says the
 * hazard lives -- "StandardMethodCodec narrows small Dart ints to int32_t, so a
 * port or an address may arrive as either width". Calling the private method
 * with a hand-built EncodableMap would skip exactly that.
 *
 * WHAT THIS CANNOT REACH, AND WHY
 *
 * The handler talks to BridgeRegistry::Instance() -- the process-wide singleton
 * -- so a test cannot give it a registry with a fake DartPortApi. The DL API is
 * therefore never bound here, and every init stops at dart_api_unavailable
 * before the role logic runs.
 *
 * So this file covers argument decoding, dispatch, and the paths that do not
 * need a live VM. The successful registration paths -- the startup race, the
 * declared-name check, framework-port delivery -- belong to osgi_bridge-test,
 * which constructs its own BridgeRegistry with a fake DartPortApi and asserts
 * them properly. Splitting them is deliberate: a test that cannot reach a path
 * should not look as though it covers it.
 *
 * dl_data IS ALWAYS ZERO HERE, AND MUST STAY THAT WAY
 *
 * Because the registry is the singleton, it holds the *real* DartPortApi, and
 * InitializeDartApi passes the address straight to Dart_InitializeApiDL --
 * which dereferences it to read the DL symbol table. A plausible-looking value
 * segfaults the test binary; this file did, before the addresses were zeroed.
 *
 * Zero is the one value that cannot: InitializeDartApi rejects it explicitly,
 * before calling into Dart. So a call still decodes both arguments, still
 * reaches the DL check, and still answers dart_api_unavailable -- without
 * touching a VM that is not there.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <method_call.h>
#include <method_result.h>
#include <standard_method_codec.h>

#include "osgi/bridge_registry.h"
#include "osgi/osgi_bridge_plugin.h"

namespace {

using ihs::osgi::OsgiBridgePlugin;

// Captures the handler the plugin registers, and the bytes it replies with.
class StubBinaryMessenger : public flutter::BinaryMessenger {
 public:
  void Send(const std::string& /*channel*/,
            const uint8_t* /*message*/,
            size_t /*message_size*/,
            flutter::BinaryReply /*reply*/ = nullptr) const override {}

  void SetMessageHandler(const std::string& channel,
                         flutter::BinaryMessageHandler handler) override {
    if (channel == OsgiBridgePlugin::kChannelName) {
      handler_ = std::move(handler);
    }
  }

  flutter::BinaryMessageHandler handler_;
};

// What the plugin replied: a success value, or an error code.
struct Reply {
  bool handled = false;
  bool success = false;
  bool not_implemented = false;
  std::string error_code;
  flutter::EncodableValue value;
};

// Turns a reply envelope back into something assertable. The codec calls
// exactly one of these three.
class RecordingResult : public flutter::MethodResult<flutter::EncodableValue> {
 public:
  explicit RecordingResult(Reply& into) : reply_(into) {}

 protected:
  void SuccessInternal(const flutter::EncodableValue* result) override {
    reply_.handled = true;
    reply_.success = true;
    if (result != nullptr) {
      reply_.value = *result;
    }
  }

  void ErrorInternal(
      const std::string& error_code,
      const std::string& /*error_message*/,
      const flutter::EncodableValue* /*error_details*/) override {
    reply_.handled = true;
    reply_.error_code = error_code;
  }

  void NotImplementedInternal() override {
    reply_.handled = true;
    reply_.not_implemented = true;
  }

 private:
  Reply& reply_;
};

class OsgiBridgePluginTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // The handler reaches the singleton, so each test starts from a known
    // state. Not the DL binding, which is a VM-global and stays unbound here.
    ihs::osgi::BridgeRegistry::Instance().ResetForTesting();
    plugin_ = std::make_unique<OsgiBridgePlugin>(&messenger_);
    ASSERT_TRUE(messenger_.handler_) << "plugin registered no handler";
  }

  void TearDown() override {
    ihs::osgi::BridgeRegistry::Instance().ResetForTesting();
  }

  // Encode a call, hand it to the registered handler, decode what comes back.
  Reply Invoke(const std::string& method, flutter::EncodableValue args) {
    const flutter::MethodCall<flutter::EncodableValue> call(
        method, std::make_unique<flutter::EncodableValue>(std::move(args)));
    const auto& codec = flutter::StandardMethodCodec::GetInstance();
    const std::unique_ptr<std::vector<uint8_t>> encoded =
        codec.EncodeMethodCall(call);

    Reply reply;
    messenger_.handler_(
        encoded->data(), encoded->size(),
        [&codec, &reply](const uint8_t* response, const size_t size) {
          // An empty response is not an absent one: it is how the channel
          // protocol spells "not implemented". Treating it as nothing is what
          // made the unknown-method case here fail -- the reply arrived, said
          // exactly what it should, and was discarded before being decoded.
          if (response == nullptr || size == 0) {
            reply.handled = true;
            reply.not_implemented = true;
            return;
          }
          RecordingResult result(reply);
          codec.DecodeAndProcessResponseEnvelope(response, size, &result);
        });
    return reply;
  }

  static flutter::EncodableValue Map(
      std::initializer_list<std::pair<const flutter::EncodableValue,
                                      flutter::EncodableValue>> entries) {
    return flutter::EncodableValue(flutter::EncodableMap(entries));
  }

  StubBinaryMessenger messenger_;
  std::unique_ptr<OsgiBridgePlugin> plugin_;
};

}  // namespace

// --- Argument decoding ------------------------------------------------------
//
// Every one of these is a bundle sending something the shell cannot act on. The
// reply has to say which, because the bundle surfaces it on its own panel and
// "rejected" with no reason is what a config typo used to look like.

TEST_F(OsgiBridgePluginTest, NonMapArgumentsAreRefused) {
  const Reply reply = Invoke(OsgiBridgePlugin::kMethodInit,
                             flutter::EncodableValue("not a map"));
  EXPECT_TRUE(reply.handled);
  EXPECT_EQ(reply.error_code, OsgiBridgePlugin::kErrorBadArgs);
}

TEST_F(OsgiBridgePluginTest, InitNeedsAStringRole) {
  EXPECT_EQ(Invoke(OsgiBridgePlugin::kMethodInit, Map({})).error_code,
            OsgiBridgePlugin::kErrorBadArgs);

  // Present but wrong type is a different mistake from absent, and both are
  // the caller's.
  EXPECT_EQ(
      Invoke(
          OsgiBridgePlugin::kMethodInit,
          Map({{flutter::EncodableValue("role"), flutter::EncodableValue(7)}}))
          .error_code,
      OsgiBridgePlugin::kErrorBadArgs);
}

TEST_F(OsgiBridgePluginTest, InitNeedsIntegerDlDataAndPort) {
  EXPECT_EQ(Invoke(OsgiBridgePlugin::kMethodInit,
                   Map({{flutter::EncodableValue("role"),
                         flutter::EncodableValue("framework")}}))
                .error_code,
            OsgiBridgePlugin::kErrorBadArgs)
      << "dl_data missing";

  EXPECT_EQ(Invoke(OsgiBridgePlugin::kMethodInit,
                   Map({{flutter::EncodableValue("role"),
                         flutter::EncodableValue("framework")},
                        {flutter::EncodableValue("dl_data"),
                         flutter::EncodableValue("not an int")}}))
                .error_code,
            OsgiBridgePlugin::kErrorBadArgs)
      << "dl_data wrong type";

  EXPECT_EQ(Invoke(OsgiBridgePlugin::kMethodInit,
                   Map({{flutter::EncodableValue("role"),
                         flutter::EncodableValue("framework")},
                        {flutter::EncodableValue("dl_data"),
                         flutter::EncodableValue(int64_t{1})}}))
                .error_code,
            OsgiBridgePlugin::kErrorBadArgs)
      << "port missing";
}

// The reason the decode path is worth testing at all: StandardMethodCodec
// narrows a small Dart int to int32_t, so the same value arrives as a different
// variant alternative depending on its magnitude. Both must get past decoding
// and reach the DL check -- which fails here, but fails *later* than a
// bad_arguments would, and that is what distinguishes the two.
TEST_F(OsgiBridgePluginTest, AcceptsAPortAsEitherIntegerWidth) {
  // dl_data stays 0 in both legs -- see the header. The port is what varies,
  // and it is decoded before the DL check, so reaching dart_api_unavailable
  // (rather than bad_arguments) is what proves the width was accepted.
  const Reply narrow = Invoke(OsgiBridgePlugin::kMethodInit,
                              Map({{flutter::EncodableValue("role"),
                                    flutter::EncodableValue("framework")},
                                   {flutter::EncodableValue("dl_data"),
                                    flutter::EncodableValue(int32_t{0})},
                                   {flutter::EncodableValue("port"),
                                    flutter::EncodableValue(int32_t{7})}}));
  EXPECT_EQ(narrow.error_code, OsgiBridgePlugin::kErrorDartApi)
      << "a 32-bit port must decode, then stop at the unbound DL API";

  const Reply wide =
      Invoke(OsgiBridgePlugin::kMethodInit,
             Map({{flutter::EncodableValue("role"),
                   flutter::EncodableValue("framework")},
                  {flutter::EncodableValue("dl_data"),
                   flutter::EncodableValue(int64_t{0})},
                  {flutter::EncodableValue("port"),
                   flutter::EncodableValue(int64_t{0x1'0000'0001})}}));
  EXPECT_EQ(wide.error_code, OsgiBridgePlugin::kErrorDartApi)
      << "a 64-bit port must decode the same way";
}

// The DL binding is checked before the role is looked at, so an unusable
// symbol table is reported as itself rather than as an unknown role. Worth
// pinning because the two are easy to reorder and the wrong one sends a bundle
// author hunting a config typo that is not there.
//
// It also means the unknown-role branch is unreachable from this fixture: it
// sits after a check that always fails here. osgi_bridge-test owns the paths
// beyond the DL binding.
TEST_F(OsgiBridgePluginTest, InitChecksTheDartApiBeforeTheRole) {
  const Reply reply = Invoke(OsgiBridgePlugin::kMethodInit,
                             Map({{flutter::EncodableValue("role"),
                                   flutter::EncodableValue("supervisor")},
                                  {flutter::EncodableValue("dl_data"),
                                   flutter::EncodableValue(int64_t{0})},
                                  {flutter::EncodableValue("port"),
                                   flutter::EncodableValue(int64_t{7})}}));
  EXPECT_EQ(reply.error_code, OsgiBridgePlugin::kErrorDartApi)
      << "an unknown role must not mask an unusable DL binding";
}

// --- Lifecycle reports ------------------------------------------------------

TEST_F(OsgiBridgePluginTest, ActiveAndStoppedNeedASymbolicName) {
  for (const char* method :
       {OsgiBridgePlugin::kMethodActive, OsgiBridgePlugin::kMethodStopped}) {
    EXPECT_EQ(Invoke(method, Map({})).error_code,
              OsgiBridgePlugin::kErrorBadArgs)
        << method << " with no name";

    EXPECT_EQ(Invoke(method, Map({{flutter::EncodableValue("symbolic_name"),
                                   flutter::EncodableValue(1)}}))
                  .error_code,
              OsgiBridgePlugin::kErrorBadArgs)
        << method << " with a non-string name";
  }
}

// A report from a bundle that never registered. The registry refuses it, and
// the plugin has to turn that into an error rather than a silent success --
// the bundle is waiting to hear whether its ACTIVE landed.
TEST_F(OsgiBridgePluginTest, ActiveFromAnUnregisteredBundleIsRejected) {
  const Reply reply = Invoke(OsgiBridgePlugin::kMethodActive,
                             Map({{flutter::EncodableValue("symbolic_name"),
                                   flutter::EncodableValue("com.ivi.ghost")}}));
  EXPECT_EQ(reply.error_code, OsgiBridgePlugin::kErrorRejected);
}

// ReportStopped answers true whatever the registry knows, so this is the one
// report that succeeds without a registration. Asserted so a change to that
// behaviour is deliberate rather than incidental.
TEST_F(OsgiBridgePluginTest, StoppedIsAcceptedEvenWithoutARegistration) {
  const Reply reply = Invoke(OsgiBridgePlugin::kMethodStopped,
                             Map({{flutter::EncodableValue("symbolic_name"),
                                   flutter::EncodableValue("com.ivi.ghost")}}));
  EXPECT_TRUE(reply.success);
}

// --- shutdown ---------------------------------------------------------------

TEST_F(OsgiBridgePluginTest, ShutdownNeedsASymbolicName) {
  EXPECT_EQ(Invoke(OsgiBridgePlugin::kMethodShutdown, Map({})).error_code,
            OsgiBridgePlugin::kErrorBadArgs);
}

// shutdown replies with whether the bundle was known, rather than erroring on
// an unknown one: a teardown path that runs after a failed registration has
// nothing to release and should not fail for saying so.
TEST_F(OsgiBridgePluginTest, ShutdownReportsWhetherTheBundleWasKnown) {
  const Reply reply = Invoke(OsgiBridgePlugin::kMethodShutdown,
                             Map({{flutter::EncodableValue("symbolic_name"),
                                   flutter::EncodableValue("com.ivi.ghost")}}));
  ASSERT_TRUE(reply.success);
  const auto* known = std::get_if<bool>(&reply.value);
  ASSERT_NE(known, nullptr) << "shutdown replies with a bool";
  EXPECT_FALSE(*known);
}

// --- Dispatch ---------------------------------------------------------------

TEST_F(OsgiBridgePluginTest, AnUnknownMethodIsNotImplemented) {
  const Reply reply = Invoke("teleport", Map({}));
  EXPECT_TRUE(reply.not_implemented)
      << "an unrecognized method must be NotImplemented, not an error";
}
