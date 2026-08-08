# FindMpv.cmake — locates libmpv (the mpv client library).
#
# On Linux and macOS this is normally a packaged shared library and pkg-config
# finds it directly. On Windows there is no standard install location, so point
# the build at an unpacked libmpv SDK with either:
#
#     -DMPV_ROOT=C:/libs/libmpv
#
# or by setting the MPV_ROOT environment variable. The layout expected there is
# the one shipped in the official `mpv-dev` archives:
#
#     <MPV_ROOT>/include/mpv/client.h
#     <MPV_ROOT>/libmpv.dll.a  (or mpv.lib / libmpv.dll.a in x86_64/)
#
# Defines the imported target Mpv::Mpv on success.

include(FindPackageHandleStandardArgs)

set(_mpv_root "${MPV_ROOT}")
if(NOT _mpv_root AND DEFINED ENV{MPV_ROOT})
    set(_mpv_root "$ENV{MPV_ROOT}")
endif()

# pkg-config is the reliable path everywhere it exists.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND AND NOT _mpv_root)
    pkg_check_modules(PC_MPV QUIET mpv)
endif()

find_path(MPV_INCLUDE_DIR
    NAMES mpv/client.h
    HINTS ${_mpv_root} ${PC_MPV_INCLUDEDIR} ${PC_MPV_INCLUDE_DIRS}
    PATH_SUFFIXES include
)

find_library(MPV_LIBRARY
    NAMES mpv libmpv mpv.dll libmpv.dll.a
    HINTS ${_mpv_root} ${PC_MPV_LIBDIR} ${PC_MPV_LIBRARY_DIRS}
    PATH_SUFFIXES lib lib64 x86_64 x86_64-w64-mingw32
)

find_package_handle_standard_args(Mpv
    REQUIRED_VARS MPV_LIBRARY MPV_INCLUDE_DIR
    VERSION_VAR PC_MPV_VERSION
)

if(Mpv_FOUND AND NOT TARGET Mpv::Mpv)
    add_library(Mpv::Mpv UNKNOWN IMPORTED)
    set_target_properties(Mpv::Mpv PROPERTIES
        IMPORTED_LOCATION "${MPV_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${MPV_INCLUDE_DIR}"
    )
endif()

mark_as_advanced(MPV_INCLUDE_DIR MPV_LIBRARY)
