include_guard()

add_library(toolchain INTERFACE)
add_library(toolchain::toolchain ALIAS toolchain)

if (ENABLE_STATIC_LINK)
    target_link_options(toolchain INTERFACE -static-libstdc++ -static-libgcc -lc -v)
else ()
    target_link_options(toolchain INTERFACE -lc -v)
endif ()

if (ENABLE_LTO AND IPO_SUPPORT_RESULT)
    set_property(TARGET toolchain PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif ()
