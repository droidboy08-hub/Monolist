# Writes src/buildinfo.h from the repository's current state.
#
# Run as a script (cmake -P) before every build rather than once at configure
# time, so the build number follows the commits instead of following whenever
# somebody last re-ran CMake. It rewrites the header only when the contents
# actually change, so an unchanged tree does not force a rebuild.
#
# Expects: SOURCE_DIR, TEMPLATE, OUTPUT, APP_VERSION.
#
# Not `VERSION`: find_package below overwrites a variable of that name from
# inside its find modules, which silently turned "0.1" into "0".

find_package(Git QUIET)

# Every git call below carries `-c safe.directory=*`. The tree is often on a
# share or a mount whose owner is not the building user, and git refuses such a
# repository outright ("dubious ownership") — which here would silently mean a
# build that cannot say which commit it is. Reading the commit count is not an
# operation that ownership protects anyone from.
set(GIT_SAFE -c safe.directory=*)

set(MONOLIST_VERSION "${APP_VERSION}")
set(MONOLIST_BUILD_NUMBER "0")
set(MONOLIST_COMMIT "unknown")
set(MONOLIST_DIRTY "false")
set(MONOLIST_SOURCE_DIR "${SOURCE_DIR}")

if(GIT_FOUND)
    # A repository is not a given: a source tarball has none, and neither does
    # a checkout copied without its .git. Every query below is allowed to fail
    # and leave the defaults above.
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" ${GIT_SAFE} rev-list --count HEAD
        WORKING_DIRECTORY "${SOURCE_DIR}"
        OUTPUT_VARIABLE count OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE countResult)
    if(countResult EQUAL 0 AND count)
        set(MONOLIST_BUILD_NUMBER "${count}")
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" ${GIT_SAFE} rev-parse --short HEAD
        WORKING_DIRECTORY "${SOURCE_DIR}"
        OUTPUT_VARIABLE sha OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE shaResult)
    if(shaResult EQUAL 0 AND sha)
        set(MONOLIST_COMMIT "${sha}")
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" ${GIT_SAFE} status --porcelain
        WORKING_DIRECTORY "${SOURCE_DIR}"
        OUTPUT_VARIABLE dirty OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE dirtyResult)
    if(dirtyResult EQUAL 0 AND NOT dirty STREQUAL "")
        set(MONOLIST_DIRTY "true")
    endif()
endif()

string(TIMESTAMP MONOLIST_BUILD_DATE "%Y-%m-%d" UTC)

configure_file("${TEMPLATE}" "${OUTPUT}.candidate" @ONLY)

# Only replace the real header when something changed, so that an unchanged
# tree does not recompile appinfo.cpp on every build.
if(EXISTS "${OUTPUT}")
    file(READ "${OUTPUT}" existing)
    file(READ "${OUTPUT}.candidate" candidate)
    if(existing STREQUAL candidate)
        file(REMOVE "${OUTPUT}.candidate")
        return()
    endif()
endif()

file(RENAME "${OUTPUT}.candidate" "${OUTPUT}")
