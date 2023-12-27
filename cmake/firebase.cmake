#
# Copyright 2020 Toyota Connected North America
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

include_guard()

if (NOT DEFINED FIREBASE_CPP_SDK_DIR)
    message(FATAL_ERROR "FIREBASE_CPP_SDK_DIR is not set")
endif ()
if (NOT DEFINED FIREBASE_SDK_LIBDIR)
    message(FATAL_ERROR "FIREBASE_SDK_LIBDIR is not set")
endif ()

find_package(PkgConfig REQUIRED)
pkg_check_modules(UUID REQUIRED IMPORTED_TARGET uuid)
pkg_check_modules(SECRET REQUIRED IMPORTED_TARGET libsecret-1)

set(CMAKE_THREAD_PREFER_PTHREAD ON)
include(FindThreads)

add_library(firebase_sdk INTERFACE)

target_include_directories(firebase_sdk INTERFACE
        ${FIREBASE_CPP_SDK_DIR}
        ${FIREBASE_CPP_SDK_DIR}/app/src/include
        ${FIREBASE_CPP_SDK_DIR}/auth/src/include
        ${FIREBASE_CPP_SDK_DIR}/firestore/src/include
        ${FIREBASE_CPP_SDK_DIR}/database/src/include
        ${FIREBASE_CPP_SDK_DIR}/storage/src/include
        ${FIREBASE_CPP_SDK_DIR}/build/external/src/firestore/Firestore/core/include
)

target_link_libraries(firebase_sdk INTERFACE
        ${FIREBASE_SDK_LIBDIR}/app/libfirebase_app.a
        ${FIREBASE_SDK_LIBDIR}/app/rest/libfirebase_rest_lib.a
        ${FIREBASE_SDK_LIBDIR}/liblibuWS.a
        ${FIREBASE_SDK_LIBDIR}/dynamic_links/libfirebase_dynamic_links.a
        ${FIREBASE_SDK_LIBDIR}/database/libfirebase_database.a
        ${FIREBASE_SDK_LIBDIR}/app_check/libfirebase_app_check.a
        ${FIREBASE_SDK_LIBDIR}/auth/libfirebase_auth.a
        ${FIREBASE_SDK_LIBDIR}/functions/libfirebase_functions.a
        ${FIREBASE_SDK_LIBDIR}/messaging/libfirebase_messaging.a
        ${FIREBASE_SDK_LIBDIR}/gma/libfirebase_gma.a
        ${FIREBASE_SDK_LIBDIR}/analytics/libfirebase_analytics.a
        ${FIREBASE_SDK_LIBDIR}/installations/libfirebase_installations.a
        ${FIREBASE_SDK_LIBDIR}/storage/libfirebase_storage.a
        ${FIREBASE_SDK_LIBDIR}/firestore/libfirebase_firestore.a
        ${FIREBASE_SDK_LIBDIR}/remote_config/libfirebase_remote_config.a
        ${FIREBASE_SDK_LIBDIR}/external/src/boringssl-build/ssl/libssl.a
        ${FIREBASE_SDK_LIBDIR}/external/src/boringssl-build/crypto/libcrypto.a
        ${FIREBASE_SDK_LIBDIR}/external/src/libuv-build/libuv_a.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/Firestore/core/libfirestore_core.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/Firestore/core/libfirestore_nanopb.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/Firestore/core/libfirestore_util.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/Firestore/Protos/libfirestore_protos_nanopb.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/leveldb-build/libleveldb.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/nanopb-build/libprotobuf-nanopb.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/libgrpc++.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/libupb.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/libgrpc.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/re2/libre2.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/abseil-cpp/absl/strings/libabsl_cordz_functions.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/abseil-cpp/absl/strings/libabsl_cordz_info.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/abseil-cpp/absl/strings/libabsl_strings.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/abseil-cpp/absl/strings/libabsl_strings_internal.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/abseil-cpp/absl/strings/libabsl_cordz_handle.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/abseil-cpp/absl/strings/libabsl_cord_internal.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/abseil-cpp/absl/strings/libabsl_str_format_internal.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/abseil-cpp/absl/strings/libabsl_cord.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/abseil-cpp/absl/hash/libabsl_hash.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/abseil-cpp/absl/hash/libabsl_low_level_hash.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/abseil-cpp/absl/hash/libabsl_city.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/abseil-cpp/absl/time/libabsl_time.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/abseil-cpp/absl/time/libabsl_civil_time.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/abseil-cpp/absl/time/libabsl_time_zone.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/abseil-cpp/absl/profiling/libabsl_exponential_biased.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/abseil-cpp/absl/types/libabsl_bad_optional_access.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/abseil-cpp/absl/types/libabsl_bad_variant_access.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/abseil-cpp/absl/random/libabsl_random_internal_randen_hwaes.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/abseil-cpp/absl/random/libabsl_random_internal_randen.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/abseil-cpp/absl/random/libabsl_random_internal_randen_slow.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/abseil-cpp/absl/random/libabsl_random_distributions.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/abseil-cpp/absl/random/libabsl_random_internal_seed_material.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/abseil-cpp/absl/random/libabsl_random_internal_platform.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/abseil-cpp/absl/random/libabsl_random_internal_randen_hwaes_impl.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/abseil-cpp/absl/random/libabsl_random_internal_pool_urbg.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/abseil-cpp/absl/random/libabsl_random_seed_sequences.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/abseil-cpp/absl/random/libabsl_random_seed_gen_exception.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/abseil-cpp/absl/synchronization/libabsl_graphcycles_internal.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/abseil-cpp/absl/synchronization/libabsl_synchronization.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/abseil-cpp/absl/container/libabsl_raw_hash_set.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/abseil-cpp/absl/container/libabsl_hashtablez_sampler.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/abseil-cpp/absl/debugging/libabsl_symbolize.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/abseil-cpp/absl/debugging/libabsl_stacktrace.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/abseil-cpp/absl/debugging/libabsl_debugging_internal.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/abseil-cpp/absl/debugging/libabsl_demangle_internal.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/abseil-cpp/absl/base/libabsl_malloc_internal.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/abseil-cpp/absl/base/libabsl_throw_delegate.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/abseil-cpp/absl/base/libabsl_base.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/abseil-cpp/absl/base/libabsl_raw_logging_internal.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/abseil-cpp/absl/base/libabsl_strerror.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/abseil-cpp/absl/base/libabsl_log_severity.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/abseil-cpp/absl/base/libabsl_spinlock_wait.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/abseil-cpp/absl/numeric/libabsl_int128.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/abseil-cpp/absl/status/libabsl_statusor.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/abseil-cpp/absl/status/libabsl_status.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/protobuf/libprotoc.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/protobuf/libprotobuf.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/third_party/cares/cares/lib/libcares.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/libgpr.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/libgrpc.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/libgrpc++.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/grpc-build/libaddress_sorting.a
        #${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/boringssl/src/util/ar/testdata/mac/libsample.a
        #${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/boringssl/src/util/ar/testdata/linux/libsample.a
        ${FIREBASE_SDK_LIBDIR}/external/src/firestore-build/external/src/snappy-build/libsnappy.a
        ${FIREBASE_SDK_LIBDIR}/external/src/flatbuffers-build/libflatbuffers.a
        ${FIREBASE_SDK_LIBDIR}/external/src/curl-build/lib/libcurl.a
        ${FIREBASE_SDK_LIBDIR}/external/src/boringssl/util/ar/testdata/mac/libsample.a
        ${FIREBASE_SDK_LIBDIR}/external/src/boringssl/util/ar/testdata/linux/libsample.a
        ${FIREBASE_SDK_LIBDIR}/external/src/boringssl/libssl.a
        ${FIREBASE_SDK_LIBDIR}/external/src/boringssl/libcrypto.a
        ${FIREBASE_SDK_LIBDIR}/external/src/zlib-build/libz.a
        PkgConfig::UUID
        PkgConfig::SECRET
        Threads::Threads
)
