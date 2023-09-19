#include <cstdlib>
#include <stdexcept>

#include "gtest/gtest.h"

#include "utils.h"

TEST(test_template, case_a) {
  auto input_value = "TEST";
  auto expected_value = std::string("TEST/.config/homescreen");
  setenv("XDG_CONFIG_HOME", input_value, true);
  auto home_path = Utils::GetConfigHomePath();
  EXPECT_EQ(home_path, expected_value);
}

TEST(test_template, case_b) {
  EXPECT_EQ(1, 1);
}
