#include <stdexcept>
#include "gio/gio.h"
#include "gtest/gtest.h"
#include "shared_library.h"

/****************************************************************
Test Case Name.Test Name： HomescreenSharedLibraryGetProcAddress_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test GetProcAddress func with valid symbol name and handle.
***************************************************************/
TEST(HomescreenSharedLibraryGetProcAddress, Lv1Normal001) {
  void* handle = dlopen("libgio-2.0.so", RTLD_LAZY | RTLD_LOCAL);
  ASSERT_TRUE(handle != nullptr);

  const void* symbol = ShellGetProcAddress(handle, "g_file_read");

  EXPECT_TRUE(symbol != nullptr);
}

/****************************************************************
Test Case Name.Test Name： HomescreenSharedLibraryGetProcAddress_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test GetProcAddress with invalid symbol name.
***************************************************************/
TEST(HomescreenSharedLibraryGetProcAddress, Lv1Abnormal001) {
  void* handle = dlopen("libgio-2.0.so", RTLD_LAZY | RTLD_LOCAL);
  ASSERT_TRUE(handle != nullptr);

  const void* symbol = ShellGetProcAddress(handle, "InvalidSymbol");
  EXPECT_TRUE(symbol == nullptr);
}

/****************************************************************
Test Case Name.Test Name： HomescreenSharedLibraryGetProcAddress_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test GetProcAddress with invalid symbol name.
***************************************************************/
TEST(HomescreenSharedLibraryGetProcAddress, Lv1Abnormal002) {
  // Using a real symbol like "g_file_read" would succeed if GLib is loaded.
  // Use a deliberately mangled name that cannot exist in any loaded library.
  const void* symbol =
      ShellGetProcAddress(nullptr, "__ivi_homescreen_no_such_symbol_a7f3b2");
  EXPECT_TRUE(symbol == nullptr);
}

/****************************************************************
Test Case Name.Test Name： HomescreenSharedLibraryGetFuncAddress_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test GetFuncAddress with valid func name and handle.
***************************************************************/
TEST(HomescreenSharedLibraryGetFuncAddress, Lv1Normal001) {
  void* handle = dlopen("libgio-2.0.so", RTLD_LAZY | RTLD_LOCAL);
  ASSERT_TRUE(handle != nullptr);

  GFileInputStream (*GFileRead)(GFile*, GCancellable*, GError**) = nullptr;
  ShellGetFuncAddress(handle, "g_file_read", &GFileRead);

  EXPECT_TRUE(GFileRead != nullptr);
}

/****************************************************************
Test Case Name.Test Name： HomescreenSharedLibraryGetFuncAddress_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test GetProcAddress with invalid func name.
***************************************************************/
TEST(HomescreenSharedLibraryGetFuncAddress, Lv1Abnormal001) {
  void* handle = dlopen("libgio-2.0.so", RTLD_LAZY | RTLD_LOCAL);
  ASSERT_TRUE(handle != nullptr);

  GFileInputStream (*GFileRead)(GFile*, GCancellable*, GError**) = nullptr;
  ShellGetFuncAddress(handle, "InvaludSymbol", &GFileRead);

  EXPECT_TRUE(GFileRead == nullptr);
}

/****************************************************************
Test Case Name.Test Name： HomescreenSharedLibraryGetFuncAddress_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test GetProcAddress with null handle.
***************************************************************/
TEST(HomescreenSharedLibraryGetFuncAddress, Lv1Abnormal002) {
  // Same as GetProcAddress Lv1Abnormal002: nullptr handle == RTLD_DEFAULT,
  // so a real symbol name like "g_file_read" would resolve if GLib is loaded.
  // Use a deliberately mangled name that cannot exist in any loaded library.
  GFileInputStream (*GFileRead)(GFile*, GCancellable*, GError**) = nullptr;
  ShellGetFuncAddress(nullptr, "__ivi_homescreen_no_such_symbol_a7f3b2",
                      &GFileRead);

  EXPECT_TRUE(GFileRead == nullptr);
}
