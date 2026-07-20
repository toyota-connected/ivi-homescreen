#include <stdexcept>
#include "gtest/gtest.h"
#include "utils.h"

/****************************************************************
Test Case Name.Test Name： HomescreenUtilsRtrim_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test the function of rtrim
***************************************************************/

TEST(HomescreenUtilsRtrim, Lv1Normal001) {
  std::string input = "unit test";
  const std::string output = Utils::rtrim(input, "t");
  EXPECT_EQ("unit tes", output);
}

/****************************************************************
Test Case Name.Test Name： HomescreenUtilsLtrim_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test the function of ltrim
***************************************************************/

TEST(HomescreenUtilsLtrim, Lv1Normal001) {
  std::string input = "unit test";
  const std::string output = Utils::ltrim(input, "u");
  EXPECT_EQ("nit test", output);
}

/****************************************************************
Test Case Name.Test Name： HomescreenUtilsTrim_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test the function of trim
***************************************************************/

TEST(HomescreenUtilsTrim, Lv1Normal001) {
  std::string input = "unit test";
  const std::string output = Utils::trim(input, "ut");
  EXPECT_EQ("nit tes", output);
}

/****************************************************************
Test Case Name.Test Name： HomescreenUtilsIsNumber_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test the function of IsNumber
***************************************************************/

TEST(HomescreenUtilsIsNumber, Lv1Normal001) {
  const bool result = Utils::IsNumber("1234567890");
  EXPECT_EQ(true, result);
}

/****************************************************************
Test Case Name.Test Name： HomescreenUtilsRemoveArgument_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test the function of RemoveArgument
***************************************************************/

TEST(HomescreenUtilsRemoveArgument, Lv1Normal001) {
  std::vector<std::string> args{"test1", "test2", "test3"};
  Utils::RemoveArgument(args, "test2");

  for (const std::string& str : args) {
    EXPECT_STRNE("test2", str.c_str());
  }
}

/****************************************************************
Test Case Name.Test Name： HomescreenUtilsRemoveArgument_Lv1Abnormal001
Use Case Name: Initialization
Test Summary：Test the function of RemoveArgument
***************************************************************/

TEST(HomescreenUtilsRemoveArgument, Lv1Abnormal001) {
  const std::vector<std::string> expected_args{"test1", "test2", "test3"};
  std::vector<std::string> args{"test1", "test2", "test3"};
  Utils::RemoveArgument(args, "test");

  EXPECT_EQ(expected_args.size(), args.size());
  EXPECT_TRUE(
      std::equal(expected_args.cbegin(), expected_args.cend(), args.cbegin()));
}

/****************************************************************
Test Case Name.Test Name： HomescreenUtilsGetHomePath_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test GetHomePath with setting HOME env param
***************************************************************/

TEST(HomescreenUtilsGetHomePath, Lv1Normal001) {
  std::string expected_value = getenv("HOME");
  expected_value = expected_value + "/" + kXdgApplicationDir;

  const auto home_path = Utils::GetHomePath();
  EXPECT_EQ(home_path, expected_value);
}

/****************************************************************
Test Case Name.Test Name： HomescreenUtilsGetConfigHomePath_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test GetConfigHomePath with XDG_CONFIG_HOME and static-cache
             idempotency in a single, order-independent test.

Note: GetConfigHomePath() resolves and caches the result on the first call
for the process lifetime.  Both concerns — the XDG path resolution and the
cache stability — are verified here so the test is not sensitive to
--gtest_shuffle or --gtest_filter execution order.
***************************************************************/

TEST(HomescreenUtilsGetConfigHomePath, Lv1Normal001) {
  // XDG_CONFIG_HOME must be an absolute path: the XDG Base Directory spec
  // states that a relative path "must be ignored", and the implementation
  // enforces this via IsSafeBasePath().  Use an absolute temp path.
  const auto input_value = "/tmp/homescreen-xdg-test";
  const auto expected_value =
      std::string("/tmp/homescreen-xdg-test/.config/homescreen");
  setenv("XDG_CONFIG_HOME", input_value, true);

  const auto home_path_1 = Utils::GetConfigHomePath();

  EXPECT_EQ(home_path_1, expected_value);

  // Unset before the second call to confirm the cache is used, not a
  // re-evaluation of the (now absent) env var.
  unsetenv("XDG_CONFIG_HOME");

  // Repeated calls must return the exact same pointer (static cache).
  const auto home_path_2 = Utils::GetConfigHomePath();

  ASSERT_NE(home_path_1, nullptr);
  EXPECT_EQ(home_path_1, home_path_2);
  // Must be an absolute path regardless of which branch was taken.
  EXPECT_EQ(home_path_1[0], '/');
}
