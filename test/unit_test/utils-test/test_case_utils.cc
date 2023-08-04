#include <stdexcept>
#include <stdlib.h>  
#include "gtest/gtest.h"
#include "utils.h"

TEST(utils_rtrim, Normalcase_01) {
    std::string input = "unit test";
    std::string output = Utils::rtrim(input, "t");
    EXPECT_EQ("unit tes", output);
}

TEST(utils_ltrim, Normalcase_01) {
    std::string input = "unit test";
    std::string output = Utils::ltrim(input, "u");
    EXPECT_EQ("nit test", output);
}

TEST(utils_trim, Normalcase_01) {
    std::string input = "unit test";
    std::string output = Utils::trim(input, "ut");
    EXPECT_EQ("nit tes", output);
}
