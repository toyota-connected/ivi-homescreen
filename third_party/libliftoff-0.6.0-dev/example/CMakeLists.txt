
add_executable(simple common.c simple.c)
target_link_libraries(simple PRIVATE liftoff)

add_executable(compositor common.c compositor.c)
target_link_libraries(compositor PRIVATE liftoff)

add_executable(dynamic common.c dynamic.c)
target_link_libraries(dynamic PRIVATE liftoff)

add_executable(multi-output common.c multi-output.c)
target_link_libraries(multi-output PRIVATE liftoff)
