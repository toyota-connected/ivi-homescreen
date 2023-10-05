#include <stdexcept>
#include "gtest/gtest.h"
#include "shared_library.h"
#include "gio/gio.h"

/****************************************************************
Test Case Name.Test Name： HomescreenSharedLibraryGetProcAddress_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test GetProcAddress func with valid symbol name and handle.
***************************************************************/
TEST(HomescreenSharedLibraryGetProcAddress, Lv1Normal001) {
  void* handle = dlopen("libgio-2.0.so", RTLD_LAZY | RTLD_LOCAL);
  ASSERT_TRUE(handle != NULL);

  void* symbol = GetProcAddress(handle, "g_file_read");

  EXPECT_TRUE(symbol != NULL);
}

/****************************************************************
Test Case Name.Test Name： HomescreenSharedLibraryGetProcAddress_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test GetProcAddress with invalid symbol name.
***************************************************************/
TEST(HomescreenSharedLibraryGetProcAddress, Lv1Abnormal001) {
  void* handle = dlopen("libgio-2.0.so", RTLD_LAZY | RTLD_LOCAL);
  ASSERT_TRUE(handle != NULL);

  void* symbol = GetProcAddress(handle, "InvalidSymbol");
  EXPECT_TRUE(symbol == NULL);
}

/****************************************************************
Test Case Name.Test Name： HomescreenSharedLibraryGetProcAddress_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test GetProcAddress with invalid symbol name.
***************************************************************/
TEST(HomescreenSharedLibraryGetProcAddress, Lv1Abnormal002) {
  void* symbol = GetProcAddress(nullptr, "g_file_read");
  EXPECT_TRUE(symbol == NULL);
}

/****************************************************************
Test Case Name.Test Name： HomescreenSharedLibraryGetFuncAddress_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test GetFuncAddress with valid func name and handle.
***************************************************************/
TEST(HomescreenSharedLibraryGetFuncAddress, Lv1Normal001) {
  void* handle = dlopen("libgio-2.0.so", RTLD_LAZY | RTLD_LOCAL);
  ASSERT_TRUE(handle != NULL);

  GFileInputStream (*GFileRead)(GFile*, GCancellable*, GError**) = nullptr;
  GetFuncAddress(handle, "g_file_read", &GFileRead);

  EXPECT_TRUE(GFileRead != NULL);
}

/****************************************************************
Test Case Name.Test Name： HomescreenSharedLibraryGetFuncAddress_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test GetProcAddress with invalid func name.
***************************************************************/
TEST(HomescreenSharedLibraryGetFuncAddress, Lv1Abnormal001) {
  void* handle = dlopen("libgio-2.0.so", RTLD_LAZY | RTLD_LOCAL);
  ASSERT_TRUE(handle != NULL);

  GFileInputStream (*GFileRead)(GFile*, GCancellable*, GError**) = nullptr;
  GetFuncAddress(handle, "InvaludSymbol", &GFileRead);

  EXPECT_TRUE(GFileRead == NULL);
}

/****************************************************************
Test Case Name.Test Name： HomescreenSharedLibraryGetFuncAddress_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test GetProcAddress with null handle.
***************************************************************/
TEST(HomescreenSharedLibraryGetFuncAddress, Lv1Abnormal002) {
  GFileInputStream (*GFileRead)(GFile*, GCancellable*, GError**) = nullptr;
  GetFuncAddress(nullptr, "g_file_read", &GFileRead);

  EXPECT_TRUE(GFileRead == NULL);
}
