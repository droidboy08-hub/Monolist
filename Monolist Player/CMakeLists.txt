cmake_minimum_required(VERSION 3.21)
project(Monolist VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if(MSVC)
  add_compile_options("/utf-8")
endif()

# libmpv is the only dependency without a clean Windows package. Turning this on
# swaps in a null audio engine so the UI, search and downloads can be built and
# tested before wrestling with it. Playback is the only thing lost.
option(MONOLIST_NO_MPV "Build without libmpv; audio playback is disabled." OFF)

# 6.6 is the floor because Icon.qml uses Shape.CurveRenderer, which is what
# keeps the glyph outlines sharp at fractional scales.
find_package(Qt6 6.6 REQUIRED COMPONENTS Core Gui Quick QuickControls2 Sql Svg Network)

if(MONOLIST_NO_MPV)
    set(MONOLIST_MPV_SOURCES src/mpvengine.h src/mpvengine_stub.cpp)
    message(STATUS "Monolist: building WITHOUT libmpv - playback disabled (MONOLIST_NO_MPV=ON)")
else()
    find_package(Mpv REQUIRED)
    set(MONOLIST_MPV_SOURCES src/mpvengine.h src/mpvengine.cpp)
    message(STATUS "Monolist: building with libmpv at ${MPV_LIBRARY}")
endif()

qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(monolist
    src/main.cpp
    src/appdatabase.h              src/appdatabase.cpp
    src/library.h                  src/library.cpp
    src/playlistmodel.h            src/playlistmodel.cpp
    src/albummodel.h               src/albummodel.cpp
    src/trackmodel.h               src/trackmodel.cpp
    src/playbackcontroller.h       src/playbackcontroller.cpp
    src/queuemodel.h               src/queuemodel.cpp
    src/mediaextractor.h           src/mediaextractor.cpp
    src/innertube.h                src/innertube.cpp
    src/catalog.h                  src/catalog.cpp
    src/lyrics.h                   src/lyrics.cpp
    src/windowchrome.h             src/windowchrome.cpp
    src/appinfo.h                  src/appinfo.cpp
    # — the recommender core, ported from the iOS player —
    # Plain C++ over QtCore: no BLAS, no ANN index, no new dependency. The
    # catalogue and graph files it reads are not shipped with the app; see
    # RECOMMENDATION_PORTING.md section 7 on why they cannot be.
    src/rec/matchkey.h             src/rec/matchkey.cpp
    src/rec/catalog.h              src/rec/catalog.cpp
    src/rec/vectorsearch.h         src/rec/vectorsearch.cpp
    src/rec/taste.h                src/rec/taste.cpp
    src/rec/graph.h                src/rec/graph.cpp
    src/rec/suitable.h             src/rec/suitable.cpp
    src/rec/shelves.h              src/rec/shelves.cpp
    src/recommender.h              src/recommender.cpp
    # — backend merged in from the Melody player —
    ${MONOLIST_MPV_SOURCES}
    src/ytdlp.h                    src/ytdlp.cpp
    src/streamresolver.h           src/streamresolver.cpp
    src/downloadmanager.h          src/downloadmanager.cpp
    src/downloadmodels.h           src/downloadmodels.cpp
    src/artworkcache.h             src/artworkcache.cpp
)

# The build's own identity: version, build number, commit, date. Regenerated
# before every build rather than at configure time, so the build number follows
# the commits; the script rewrites the header only when something changed, so
# an unchanged tree does not recompile.
set(MONOLIST_BUILDINFO "${CMAKE_CURRENT_BINARY_DIR}/generated/buildinfo.h")
add_custom_target(monolist_buildinfo
    BYPRODUCTS "${MONOLIST_BUILDINFO}"
    COMMAND ${CMAKE_COMMAND}
            -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DTEMPLATE=${CMAKE_CURRENT_SOURCE_DIR}/src/buildinfo.h.in
            -DOUTPUT=${MONOLIST_BUILDINFO}
            -DAPP_VERSION=${PROJECT_VERSION}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/buildinfo.cmake
    COMMENT "Stamping the build number"
    VERBATIM)
add_dependencies(monolist monolist_buildinfo)
target_include_directories(monolist PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

# Written once now as well, because the first configure happens before any
# build runs and appinfo.cpp has to have something to include.
execute_process(COMMAND ${CMAKE_COMMAND}
    -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
    -DTEMPLATE=${CMAKE_CURRENT_SOURCE_DIR}/src/buildinfo.h.in
    -DOUTPUT=${MONOLIST_BUILDINFO}
    -DAPP_VERSION=${PROJECT_VERSION}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/buildinfo.cmake)

# components/ and views/ hold .qml files outside the module root. Qt 6.8+ wants
# a qmldir in each so the directory imports in Main.qml and the views resolve
# through the module rather than the old implicit-directory path. NEW makes Qt
# generate them; without it the build still works but warns (QTP0004).
if(QT_KNOWN_POLICY_QTP0004)
    qt_policy(SET QTP0004 NEW)
endif()

set_source_files_properties(Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(monolist
    URI Monolist
    VERSION 1.0
    # C++ types declared to the module (QML_ELEMENT), so qmllint and the QML
    # compiler know them like any other component.
    SOURCES
        src/videosurface.h             src/videosurface.cpp
    QML_FILES
        Main.qml
        Theme.qml
        components/Icon.qml
        components/IconButton.qml
        components/LikeButton.qml
        components/NavItem.qml
        components/PlaylistRow.qml
        components/Sidebar.qml
        components/TopBar.qml
        components/PosterHero.qml
        components/SectionHeader.qml
        components/AlbumCard.qml
        components/Artwork.qml
        components/TrackTable.qml
        components/NowPlayingBar.qml
        components/ProgressSlider.qml
        components/HRule.qml
        components/PosterButton.qml
        components/DownloadButton.qml
        components/DownloadGlyph.qml
        components/ChoiceChip.qml
        components/TrackMenu.qml
        components/QueuePanel.qml
        components/CardShelf.qml
        components/ActionButton.qml
        components/WindowButtons.qml
        components/WindowDragArea.qml
        components/CollectionCover.qml
        components/Toast.qml
        components/MonoMenu.qml
        components/MonoMenuItem.qml
        components/MonoMenuRule.qml
        components/PlaylistSubmenu.qml
        components/MonoScrollBar.qml
        components/LyricsPane.qml
        components/ToggleRow.qml
        components/DownloadOptions.qml
        components/ServiceRow.qml
        components/RecShelf.qml
        views/HomeView.qml
        views/SearchView.qml
        views/LibraryView.qml
        views/DownloadsView.qml
        views/PageView.qml
        views/PlaylistView.qml
        views/NowPlayingView.qml
        views/SettingsView.qml
    RESOURCES
        Icons.js
        # Bundled so the design renders identically on Windows, macOS and Linux
        # without asking the user to install anything. Archivo is OFL-1.1;
        # fonts/OFL.txt travels with it as the licence requires.
        fonts/Archivo-Regular.ttf
        fonts/Archivo-SemiBold.ttf
        fonts/Archivo-ExtraBold.ttf
        fonts/OFL.txt
)

target_include_directories(monolist PRIVATE src)

target_link_libraries(monolist PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Sql
    Qt6::Network
)

if(MONOLIST_NO_MPV)
    target_compile_definitions(monolist PRIVATE MONOLIST_NO_MPV)
else()
    target_link_libraries(monolist PRIVATE Mpv::Mpv)
endif()

if(WIN32)
    # WindowChrome: the drop shadow and square corners of the caption-less frame.
    # netapi32: the account's full name, for the sidebar (Library).
    target_link_libraries(monolist PRIVATE dwmapi netapi32)
endif()

# On Windows the app is a GUI binary; keep the console attached in debug builds
# so qWarning output about a missing yt-dlp or libmpv is visible.
if(WIN32)
    set_target_properties(monolist PROPERTIES
        WIN32_EXECUTABLE $<NOT:$<CONFIG:Debug>>
    )
endif()

install(TARGETS monolist
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
