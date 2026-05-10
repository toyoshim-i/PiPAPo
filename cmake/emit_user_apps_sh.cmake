# emit_user_apps_sh.cmake — Emit cmake/user_apps.cmake's app/test lists in
# shell-readable form, so non-cmake build paths (currently scripts/build.sh
# for xtensa_cc) can iterate the same canonical list every other target
# uses.  Single source of truth: cmake/user_apps.cmake.
#
# Invocation:
#   cmake -DPPAP_ROOT=<path> -DOUTPUT=<path> [-DPPAP_TESTS=ON] \
#         -P cmake/emit_user_apps_sh.cmake
#
# Writes shell variable assignments to ${OUTPUT}:
#   USER_APPS="app1 app2 ..."
#   USER_TESTS="test1 test2 ..."           (only if PPAP_TESTS=ON)
#   USER_APP_<name>_SOURCES="<absolute paths, space-separated>"
#       — emitted only for apps with a non-default main path or extra
#         sources.  Apps without one default to ${PPAP_ROOT}/src/user/<name>.c
#         in the consumer.
#   USER_TEST_<name>_SOURCE="<absolute path>"
#       — always points at tests/user/<name>.c.

cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED PPAP_ROOT)
    message(FATAL_ERROR "emit_user_apps_sh.cmake: PPAP_ROOT not set")
endif()
if(NOT DEFINED OUTPUT)
    message(FATAL_ERROR "emit_user_apps_sh.cmake: OUTPUT not set")
endif()

# Pull in the canonical USER_APPS list and per-app source overrides.
include(${PPAP_ROOT}/cmake/user_apps.cmake)

# USER_TESTS is currently declared in cmake/user.cmake (next to the
# build pipeline that uses it).  Keep it there to avoid disturbing the
# romfs targets; re-extract the literal list with a small regex.
file(READ ${PPAP_ROOT}/cmake/user.cmake _user_cmake)
string(REGEX MATCH "set\\(USER_TESTS([^)]*)\\)" _m "${_user_cmake}")
set(_tests "${CMAKE_MATCH_1}")
# Strip line comments, then collapse whitespace runs to single spaces.
string(REGEX REPLACE "#[^\n]*" "" _tests "${_tests}")
string(REGEX REPLACE "[ \t\r\n]+" " " _tests "${_tests}")
string(STRIP "${_tests}" _tests)
separate_arguments(USER_TESTS UNIX_COMMAND "${_tests}")

# Build the manifest body in a string, then write it to OUTPUT in one
# go.  cmake's message() goes to stderr, which the consumer can't
# capture cleanly via simple stdout redirection.
set(_out "")

# Paths are already absolute (the include of user_apps.cmake resolves
# them via _PPAP_USER_APPS_ROOT).  cmake lists are semicolon-separated;
# consumers want spaces.
string(REPLACE ";" " " _apps_sh "${USER_APPS}")
string(APPEND _out "USER_APPS=\"${_apps_sh}\"\n")

foreach(app ${USER_APPS})
    set(_sources "")
    if(DEFINED PPAP_USER_MAIN_SOURCE_${app})
        list(APPEND _sources ${PPAP_USER_MAIN_SOURCE_${app}})
    endif()
    if(DEFINED PPAP_USER_EXTRA_SOURCES_${app})
        # Apps with extras but no explicit main use the default
        # ${PPAP_ROOT}/src/user/<app>.c as the main source.
        if(NOT DEFINED PPAP_USER_MAIN_SOURCE_${app})
            list(APPEND _sources ${PPAP_ROOT}/src/user/${app}.c)
        endif()
        list(APPEND _sources ${PPAP_USER_EXTRA_SOURCES_${app}})
    endif()
    if(_sources)
        string(REPLACE ";" " " _sources_sh "${_sources}")
        string(APPEND _out "USER_APP_${app}_SOURCES=\"${_sources_sh}\"\n")
    endif()
endforeach()

if(PPAP_TESTS)
    string(REPLACE ";" " " _tests_sh "${USER_TESTS}")
    string(APPEND _out "USER_TESTS=\"${_tests_sh}\"\n")
    foreach(t ${USER_TESTS})
        string(APPEND _out "USER_TEST_${t}_SOURCE=\"${PPAP_ROOT}/tests/user/${t}.c\"\n")
    endforeach()
endif()

file(WRITE ${OUTPUT} "${_out}")
