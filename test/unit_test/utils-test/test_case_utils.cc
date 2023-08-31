#include <stdexcept>
#include <stdlib.h>  
#include "gtest/gtest.h"
#include "utils.h"

/****************************************************************
Test Case Name.Test Name： FlutterAutoUtilsRtrim_Lv1Normal001
Use Case Name: Intialization
Test Summary：Test the function of rtrim
***************************************************************/

TEST(FlutterAutoUtilsRtrim, Lv1Normal001) {
    std::string input = "unit test";
    std::string output = Utils::rtrim(input, "t");
    EXPECT_EQ("unit tes", output);
}

/****************************************************************
Test Case Name.Test Name： FlutterAutoUtilsLtrim_Lv1Normal001
Use Case Name: Intialization
Test Summary：Test the function of ltrim
***************************************************************/

TEST(FlutterAutoUtilsLtrim, Lv1Normal001) {
    std::string input = "unit test";
    std::string output = Utils::ltrim(input, "u");
    EXPECT_EQ("nit test", output);
}

/****************************************************************
Test Case Name.Test Name： FlutterAutoUtilsTrim_Lv1Normal001
Use Case Name: Intialization
Test Summary：Test the function of trim
***************************************************************/

TEST(FlutterAutoUtilsTrim, Lv1Normal001) {
    std::string input = "unit test";
    std::string output = Utils::trim(input, "ut");
    EXPECT_EQ("nit tes", output);
}
