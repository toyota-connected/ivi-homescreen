
add_library(plugin_logging STATIC logging.cc logging.h)
target_link_libraries(plugin_logging PUBLIC platform_homescreen flutter_common)