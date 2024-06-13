
add_library(mock_drm_lib SHARED libdrm_mock.c)
set_target_properties(mock_drm_lib PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR})
