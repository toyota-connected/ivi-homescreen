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
  std::string output = Utils::rtrim(input, "t");
  EXPECT_EQ("unit tes", output);
}

/****************************************************************
Test Case Name.Test Name： HomescreenUtilsLtrim_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test the function of ltrim
***************************************************************/

TEST(HomescreenUtilsLtrim, Lv1Normal001) {
  std::string input = "unit test";
  std::string output = Utils::ltrim(input, "u");
  EXPECT_EQ("nit test", output);
}

/****************************************************************
Test Case Name.Test Name： HomescreenUtilsTrim_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test the function of trim
***************************************************************/

TEST(HomescreenUtilsTrim, Lv1Normal001) {
  std::string input = "unit test";
  std::string output = Utils::trim(input, "ut");
  EXPECT_EQ("nit tes", output);
}

/****************************************************************
Test Case Name.Test Name： HomescreenUtilsIsNumber_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test the function of IsNumber
***************************************************************/

TEST(HomescreenUtilsIsNumber, Lv1Normal001) {
  bool result = Utils::IsNumber("1234567890");
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
  std::vector<std::string> expected_args{"test1", "test2", "test3"};
  std::vector<std::string> args{"test1", "test2", "test3"};
  Utils::RemoveArgument(args, "test");

  EXPECT_EQ(expected_args.size(), args.size());
  EXPECT_TRUE(std::equal(expected_args.cbegin(), expected_args.cend(), args.cbegin()));
}

/****************************************************************
Test Case Name.Test Name： HomescreenUtilsGetHomePath_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test GetHomePath with setting HOME env param
***************************************************************/

TEST(HomescreenUtilsGetHomePath, Lv1Normal001) {
  std::string expected_value = getenv("HOME");
  expected_value = expected_value + "/" + kXdgApplicationDir;

  auto home_path = Utils::GetHomePath();
  EXPECT_EQ(home_path, expected_value);
}

/****************************************************************
Test Case Name.Test Name： HomescreenUtilsGetConfigHomePath_Lv1Normal001
Use Case Name: Initialization
Test Summary：Test GetConfigHomePath with setting XDG_CONFIG_HOME env param
***************************************************************/

TEST(HomescreenUtilsGetConfigHomePath, Lv1Normal001) {
  auto input_value = "TEST";
  auto expected_value = std::string("TEST/.config/homescreen");
  setenv("XDG_CONFIG_HOME", input_value, true);

  auto home_path = Utils::GetConfigHomePath();

  EXPECT_EQ(home_path, expected_value);

  // delete param
  unsetenv("XDG_CONFIG_HOME");
}

/****************************************************************
Test Case Name.Test Name： HomescreenUtilsGetConfigHomePath_Lv1Normal002
Use Case Name: Initialization
Test Summary：Test GetConfigHomePath without setting XDG_CONFIG_HOME env param
***************************************************************/

TEST(HomescreenUtilsGetConfigHomePath, Lv1Normal002) {
  std::string expected_value = getenv("HOME");
  expected_value = expected_value + "/" + kXdgApplicationDir;

  // if XDG_CONFIG_HOME does not set, ret value is home path
  auto home_path = Utils::GetConfigHomePath();
  EXPECT_EQ(home_path, expected_value);
}
