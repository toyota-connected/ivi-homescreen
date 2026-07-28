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

#
# Dirty tree
#
# A build from a modified tree is not the commit it names. Without this a
# developer build and the tagged release it was derived from are
# indistinguishable in the binary, which is exactly the moment the stamp is
# being consulted. Untracked files are excluded: build directories and local
# scratch are not modifications to the source the binary was compiled from.
execute_process(
        COMMAND git status --porcelain --untracked-files=no
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_TREE_STATUS
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
)

if (GIT_TREE_STATUS)
    set(GIT_COMMIT_HASH "${GIT_COMMIT_HASH}-dirty")
    message(STATUS "GIT Tree ............... dirty")
endif ()

#
# Re-configure when the checkout moves
#
# Everything above is captured at configure time, and nothing re-runs configure
# when HEAD moves. A build tree configured on one commit therefore keeps
# stamping that commit into every binary built afterwards. A binary that
# confidently names the wrong commit is worse than one that names none, because
# it defeats the very check it exists to support.
#
# Two paths matter and both are needed. HEAD changes on checkout and detach.
# Committing, amending, or resetting on a branch leaves HEAD alone and moves
# the branch ref instead, so watching HEAD by itself misses the most common
# case. Ask git for both paths rather than assuming a layout: in a linked
# worktree .git is a file, HEAD lives in that worktree's own git directory, and
# refs/heads lives in the common one.
set(_git_watch "")

execute_process(
        COMMAND git rev-parse --git-path HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE _git_head_path
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
)
if (_git_head_path)
    list(APPEND _git_watch "${_git_head_path}")
endif ()

# The ref HEAD resolves to, when HEAD is symbolic. A detached HEAD has none,
# and then HEAD alone is the whole story.
execute_process(
        COMMAND git symbolic-ref --quiet HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE _git_symref
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
)
if (_git_symref)
    execute_process(
            COMMAND git rev-parse --git-path "${_git_symref}"
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            OUTPUT_VARIABLE _git_ref_path
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
    )
    if (_git_ref_path)
        list(APPEND _git_watch "${_git_ref_path}")
    endif ()
endif ()

# A packed ref has no loose file to watch, which is what a fresh clone with no
# local commits looks like.
execute_process(
        COMMAND git rev-parse --git-path packed-refs
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE _git_packed_path
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
)
if (_git_packed_path)
    list(APPEND _git_watch "${_git_packed_path}")
endif ()

# `git rev-parse --git-path` may answer relative to the directory it ran in, so
# anchor anything relative back to that same directory.
foreach (_git_path IN LISTS _git_watch)
    if (NOT IS_ABSOLUTE "${_git_path}")
        set(_git_path "${CMAKE_SOURCE_DIR}/${_git_path}")
    endif ()
    if (EXISTS "${_git_path}")
        set_property(DIRECTORY "${CMAKE_SOURCE_DIR}"
                APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_git_path}")
    endif ()
endforeach ()

unset(_git_watch)
unset(_git_head_path)
unset(_git_symref)
unset(_git_ref_path)
unset(_git_packed_path)
unset(_git_path)
