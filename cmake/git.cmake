include_guard()

#
# Branch
#
execute_process(
        COMMAND git rev-parse --abbrev-ref HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_BRANCH
        OUTPUT_STRIP_TRAILING_WHITESPACE
)

if (GIT_BRANCH)
    message(STATUS "GIT Branch ............. ${GIT_BRANCH}")
else ()
    set(GIT_BRANCH "unknown")
endif ()

#
# Commit Hash
#
execute_process(
        COMMAND git rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_COMMIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
)

if (GIT_COMMIT_HASH)
    message(STATUS "GIT Hash ............... ${GIT_COMMIT_HASH}")
else ()
    set(GIT_COMMIT_HASH "unknown")
endif ()
