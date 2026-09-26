cmake_minimum_required(VERSION 3.21)
project(Monolist VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if(APPLE)
    # The few parts that talk to AppKit and MediaPlayer (src/macos/) are
    # Objective-C++.
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 17)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)

    # Xcode 15 and later run build scripts in a sandbox that may read only the
    # files a script declares. Qt's generators (moc, rcc, qmlcachegen) and the
    # build-number stamp read more than that, so the sandbox is off.
    set(CMAKE_XCODE_ATTRIBUTE_ENABLE_USER_SCRIPT_SANDBOXING NO)
endif()

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

if(APPLE)
    # — Monolist.app —
    target_sources(monolist PRIVATE
        # The window under its transparent title bar (WindowChrome).
        src/macos/macwindow.h          src/macos/macwindow.mm
        # Media keys, Control Center and the lock screen (Now Playing).
        src/macos/mediasession.h       src/macos/mediasession.mm
        macos/Monolist.icns
        # Settings › Update components runs this to update yt-dlp, FFmpeg and
        # Deno through Homebrew, from wherever the app is installed.
        scripts/setup-macos.sh
    )
    set_source_files_properties(src/macos/macwindow.mm src/macos/mediasession.mm
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    set_source_files_properties(macos/Monolist.icns
        PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_source_files_properties(scripts/setup-macos.sh
        PROPERTIES MACOSX_PACKAGE_LOCATION Resources/scripts)
    target_link_libraries(monolist PRIVATE "-framework AppKit" "-framework MediaPlayer")

    # The build number the stamp above just worked out, for CFBundleVersion.
    file(STRINGS "${MONOLIST_BUILDINFO}" _monolist_build_line REGEX "MONOLIST_BUILD_NUMBER")
    string(REGEX MATCH "\"([0-9]+)\"" _ "${_monolist_build_line}")
    set(MONOLIST_BUNDLE_BUILD "${CMAKE_MATCH_1}")
    if(NOT MONOLIST_BUNDLE_BUILD)
        set(MONOLIST_BUNDLE_BUILD 0)
    endif()

    # Read by macos/Info.plist.in. Homebrew builds Qt for the Mac it runs on,
    # so this is only a floor for anything built elsewhere.
    if(CMAKE_OSX_DEPLOYMENT_TARGET)
        set(MONOLIST_MACOS_MINIMUM "${CMAKE_OSX_DEPLOYMENT_TARGET}")
    else()
        set(MONOLIST_MACOS_MINIMUM "12.0")
    endif()

    # Signed ad hoc ("Sign to Run Locally"): builds on any Mac with no Apple
    # developer account, and runs there. A Developer ID is only needed to hand
    # the app to other people; see README.md.
    set(QT_NO_SET_XCODE_DEVELOPMENT_TEAM_ID ON)

    set_target_properties(monolist PROPERTIES
        MACOSX_BUNDLE TRUE
        # Monolist.app, as Finder, the Dock and the menu bar show it.
        OUTPUT_NAME Monolist
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist.in"
        MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.droidboy08-hub.monolist"
        MACOSX_BUNDLE_BUNDLE_NAME "Monolist"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${MONOLIST_BUNDLE_BUILD}"
        MACOSX_BUNDLE_ICON_FILE "Monolist.icns"
        MACOSX_BUNDLE_COPYRIGHT "Monolist. Archivo is licensed under the SIL Open Font License 1.1."
        XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Manual"
        XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "-"
        XCODE_ATTRIBUTE_ENABLE_HARDENED_RUNTIME NO
        # Xcode's Run button starts the app from the build folder, where it
        # still loads Qt and libmpv from Homebrew; scripts/build-macos.sh makes
        # the self-contained copy.
        XCODE_GENERATE_SCHEME TRUE
        # The log in Xcode's console as plain lines, as the Windows debug
        # build's console shows it.
        XCODE_SCHEME_ENVIRONMENT "QT_FORCE_STDERR_LOGGING=1"
    )
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
