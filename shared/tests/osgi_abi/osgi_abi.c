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
 * ABI conformance for the OSGi handshake structs.
 *
 * WHY THIS EXISTS
 *
 * dart:ffi cannot read a C header, so every Dart caller of this surface mirrors
 * IhsOsgiPeerInfo and IhsOsgiBundleInfo by hand -- today in two places, the
 * osgi_ffi package and ivi-homescreen's own activator fixture. Nothing connects
 * those declarations to this header.
 *
 * struct_size does not close the gap. The forwarder refuses a struct smaller
 * than it expects, so a Dart side that lost a field is caught -- but a field
 * that *moved* keeps the same size, passes that check, and then writes a port
 * id over a pointer. That failure is memory corruption at a boundary, not a
 * failed assertion, and it would surface as an unrelated crash somewhere else.
 *
 * So the layout is pinned from both sides. These are the numbers
 * packages/osgi_ffi/test/abi_conformance_test.dart asserts against in the
 * dart_osgi repository. Changing either struct means changing both, and this
 * fails first -- at compile time, since the assertions are _Static_assert.
 *
 * PLATFORM
 *
 * The literals are LP64 (64-bit pointers, 8-byte size_t), which is what
 * ivi-homescreen targets on every supported board. A 32-bit port would fail
 * here rather than silently disagreeing with the Dart side, which is the
 * correct outcome: the Dart mirrors would need their own review before that
 * port could work at all.
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "ihs/ihs_osgi.h"

/* IhsOsgiPeerInfo: size_t struct_size; void* dart_api_dl_data; int64_t port; */
_Static_assert(sizeof(IhsOsgiPeerInfo) == 24,
               "IhsOsgiPeerInfo changed size; update the Dart mirrors");
_Static_assert(offsetof(IhsOsgiPeerInfo, struct_size) == 0,
               "IhsOsgiPeerInfo.struct_size moved");
_Static_assert(offsetof(IhsOsgiPeerInfo, dart_api_dl_data) == 8,
               "IhsOsgiPeerInfo.dart_api_dl_data moved");
_Static_assert(offsetof(IhsOsgiPeerInfo, port) == 16,
               "IhsOsgiPeerInfo.port moved");

/* IhsOsgiBundleInfo: size_t struct_size; IhsOsgiPeerInfo peer; const char* */
_Static_assert(sizeof(IhsOsgiBundleInfo) == 40,
               "IhsOsgiBundleInfo changed size; update the Dart mirrors");
_Static_assert(offsetof(IhsOsgiBundleInfo, struct_size) == 0,
               "IhsOsgiBundleInfo.struct_size moved");
_Static_assert(offsetof(IhsOsgiBundleInfo, peer) == 8,
               "IhsOsgiBundleInfo.peer moved");
_Static_assert(offsetof(IhsOsgiBundleInfo, symbolic_name) == 32,
               "IhsOsgiBundleInfo.symbolic_name moved");

/* The status values are hardcoded on the Dart side too -- twice -- so they are
 * part of the same contract as the layout. */
_Static_assert(IHS_OSGI_OK == 0, "IHS_OSGI_OK changed");
_Static_assert(IHS_OSGI_ERR_INVALID == -1, "IHS_OSGI_ERR_INVALID changed");
_Static_assert(IHS_OSGI_ERR_DART_API == -2, "IHS_OSGI_ERR_DART_API changed");
_Static_assert(IHS_OSGI_ERR_REJECTED == -3, "IHS_OSGI_ERR_REJECTED changed");
_Static_assert(IHS_OSGI_ERR_UNAVAILABLE == -4,
               "IHS_OSGI_ERR_UNAVAILABLE changed");

int main(void) {
  /* Printed, not just asserted: when a port does fail the static assertions
   * above, the first question is "what are the numbers now", and the answer
   * should not require rebuilding this by hand. */
  printf("IhsOsgiPeerInfo   size=%zu struct_size@%zu dart_api_dl_data@%zu port@%zu\n",
         sizeof(IhsOsgiPeerInfo), offsetof(IhsOsgiPeerInfo, struct_size),
         offsetof(IhsOsgiPeerInfo, dart_api_dl_data),
         offsetof(IhsOsgiPeerInfo, port));
  printf("IhsOsgiBundleInfo size=%zu struct_size@%zu peer@%zu symbolic_name@%zu\n",
         sizeof(IhsOsgiBundleInfo), offsetof(IhsOsgiBundleInfo, struct_size),
         offsetof(IhsOsgiBundleInfo, peer),
         offsetof(IhsOsgiBundleInfo, symbolic_name));

  /* The surface is inert without a host, which is the state this test runs in:
   * no shell has installed one. Asserting that here keeps the "safe default"
   * promise covered from an out-of-tree C consumer as well as from the unit
   * tests inside the tree. */
  if (ihs_osgi_available()) {
    printf("FAIL: ihs_osgi_available() is true with no host installed\n");
    return 1;
  }
  if (ihs_osgi_report_active(NULL) != IHS_OSGI_ERR_INVALID) {
    printf("FAIL: a null handle should be IHS_OSGI_ERR_INVALID\n");
    return 1;
  }
  if (ihs_osgi_unregister(NULL) != IHS_OSGI_OK) {
    printf("FAIL: unregister(NULL) should be IHS_OSGI_OK\n");
    return 1;
  }
  printf("PASS\n");
  return 0;
}
