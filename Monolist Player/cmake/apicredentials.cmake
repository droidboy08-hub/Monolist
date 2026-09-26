# Writes generated/apicredentials.h from MONOLIST_LASTFM_API_KEY and
# MONOLIST_LASTFM_SHARED_SECRET in the environment.
#
# Run as a script (cmake -P) before every build, as buildinfo.cmake is, so
# setting or changing the variables takes effect at the next build without
# re-running CMake. The values are read here, from the environment, rather than
# passed on the command line, so they never appear in build.ninja either. It
# rewrites the header only when the contents change.
#
# Never message() a value: say whether there is a key, and nothing more.
#
# Expects: TEMPLATE, OUTPUT.

set(key "$ENV{MONOLIST_LASTFM_API_KEY}")
set(secret "$ENV{MONOLIST_LASTFM_SHARED_SECRET}")
string(STRIP "${key}" key)
string(STRIP "${secret}" secret)
set(given FALSE)
if(NOT key STREQUAL "" OR NOT secret STREQUAL "")
    set(given TRUE)
endif()

# Last.fm's are 32 hexadecimal characters. Anything but letters and digits is
# refused rather than pasted into a C string, where a quote or a backslash
# would break the build or quietly change the value.
if(NOT key STREQUAL "" AND NOT key MATCHES "^[A-Za-z0-9]+$")
    message(WARNING "Monolist: MONOLIST_LASTFM_API_KEY is not letters and digits only; building without a Last.fm key.")
    set(key "")
    set(secret "")
endif()
if(NOT secret STREQUAL "" AND NOT secret MATCHES "^[A-Za-z0-9]+$")
    message(WARNING "Monolist: MONOLIST_LASTFM_SHARED_SECRET is not letters and digits only; building without a Last.fm key.")
    set(key "")
    set(secret "")
endif()

# Both or neither: a key without its secret cannot sign a single request.
if(key STREQUAL "" OR secret STREQUAL "")
    if(NOT key STREQUAL "" OR NOT secret STREQUAL "")
        message(WARNING "Monolist: only one of MONOLIST_LASTFM_API_KEY and MONOLIST_LASTFM_SHARED_SECRET is set; building without a Last.fm key.")
    endif()
    set(key "")
    set(secret "")
endif()

set(MONOLIST_LASTFM_API_KEY_VALUE "${key}")
set(MONOLIST_LASTFM_SHARED_SECRET_VALUE "${secret}")
configure_file("${TEMPLATE}" "${OUTPUT}.candidate" @ONLY)

if(EXISTS "${OUTPUT}")
    file(READ "${OUTPUT}" existing)
    file(READ "${OUTPUT}.candidate" candidate)
    if(existing STREQUAL candidate)
        file(REMOVE "${OUTPUT}.candidate")
        return()
    endif()
endif()

file(RENAME "${OUTPUT}.candidate" "${OUTPUT}")
if(key STREQUAL "" AND NOT given)
    message(STATUS "Monolist: this build has no Last.fm key (MONOLIST_LASTFM_API_KEY and MONOLIST_LASTFM_SHARED_SECRET are not set)")
elseif(key STREQUAL "")
    message(STATUS "Monolist: this build has no Last.fm key")
else()
    message(STATUS "Monolist: this build has a Last.fm key")
endif()
