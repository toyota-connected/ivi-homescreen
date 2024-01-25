#include <stdexcept>
#include "gtest/gtest.h"
#include "backend/gl_process_resolver.h"

/****************************************************************
Test Case Name.Test Name： HomescreenGlProcessResolverProcessResolver_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test process_resolver func with valid process name.
              Test the Initialize() method at the same time
                by calling the API via the GetInstance() method.
***************************************************************/
TEST(HomescreenGlProcessResolver, Lv1Normal001) {
  auto gl_process = GlProcessResolver::GetInstance();
  auto process_resolver = gl_process.process_resolver("glGetString");

  EXPECT_TRUE(process_resolver != NULL);
}

/****************************************************************
Test Case Name.Test Name： HomescreenGlProcessResolverProcessResolver_Lv1Abnormal001
Use Case Name: Initialization
Test Summary：Test process_resolver func with invalid process name
***************************************************************/
TEST(HomescreenGlProcessResolver, Lv1Abnormal001) {
  auto gl_process = GlProcessResolver::GetInstance();
  auto process_resolver = gl_process.process_resolver("InvalidProcess");

  EXPECT_TRUE(process_resolver == NULL);
}

/****************************************************************
Test Case Name.Test Name： HomescreenGlProcessResolverProcessResolver_Lv1Abnormal002
Use Case Name: Initialization
Test Summary：Test process_resolver func with null param
***************************************************************/
TEST(HomescreenGlProcessResolver, Lv1Abnormal002) {
  auto gl_process = GlProcessResolver::GetInstance();
  auto process_resolver = gl_process.process_resolver(NULL);

  EXPECT_TRUE(process_resolver == NULL);
}

/****************************************************************
Test Case Name.Test Name： HomescreenGlProcessResolverGetHandle_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test process_resolver func with valid library name
***************************************************************/
TEST(HomescreenGlProcessResolverGetHandle, Lv1Normal001) {
  void* handle;
  int ret = EglProcessResolver::GetHandle("libEGL.so.1", &handle);

  EXPECT_EQ(ret, 1);
  EXPECT_TRUE(handle != NULL);
}

/****************************************************************
Test Case Name.Test Name： HomescreenGlProcessResolverGetHandle_Lv1Abnormal001
Use Case Name: Initialization
Test Summary：Test process_resolver func with invalid library name
***************************************************************/

TEST(HomescreenGlProcessResolverGetHandle, Lv1Abnormal001) {
  void* handle;
  int ret = EglProcessResolver::GetHandle("InvalidLibrary", &handle);

  EXPECT_EQ(ret, -1);
}
