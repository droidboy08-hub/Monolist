#!/bin/bash
# Builds Monolist.app on a Mac.
#
#     scripts/build-macos.sh              a Release build: build-macos/dist/Monolist.app
#     scripts/build-macos.sh --install    the same, then copied into Applications
#     scripts/build-macos.sh --xcode      an Xcode project, opened in Xcode
#
# Options:
#     --install    Copy the finished app into /Applications (~/Applications
#                  when that is not writable), replacing an earlier Monolist.
#     --dmg        Also make build-macos/dist/Monolist.dmg to copy it from.
#     --run        Open the app once it is built.
#     --debug      The Debug configuration instead of Release.
#     --no-deploy  Leave Qt and libmpv out of the bundle. Quicker, and fine on
#                  this Mac, but the app then needs Homebrew's copies to run.
#     --xcode      Generate build-xcode/Monolist.xcodeproj and open it. Build
#                  and run it there with the "monolist" scheme (⌘R).
#
# Run scripts/setup-macos.sh once first. Builds go in build-macos/ and
# build-xcode/ inside "Monolist Player", which git ignores.
#
# Written for the bash that ships with macOS (3.2).

set -eo pipefail

config=Release
install=0
dmg=0
run=0
deploy=1
xcode=0

for argument in "$@"; do
    case "$argument" in
        --install)   install=1 ;;
        --dmg)       dmg=1 ;;
        --run)       run=1 ;;
        --debug)     config=Debug ;;
        --no-deploy) deploy=0 ;;
        --xcode)     xcode=1 ;;
        -h|--help)
            sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *)
            echo "Unknown option: $argument (try --help)" >&2
            exit 2 ;;
    esac
done

say()  { printf '%s\n' "$*"; }
step() { printf '\n== %s\n' "$*"; }
warn() { printf 'Warning: %s\n' "$*" >&2; }
fail() { printf 'Error: %s\n' "$*" >&2; exit 1; }

[ "$(uname -s)" = "Darwin" ] || fail "this script is for macOS. On Windows use scripts/build-windows.ps1."

SRC="$(cd "$(dirname "$0")/.." && pwd)"
BUNDLE_ID="io.github.droidboy08-hub.monolist"

# A Terminal running under Rosetta builds for Intel, and Homebrew's libraries
# on Apple silicon are not Intel: the link fails with a wall of errors.
if [ "$(sysctl -n sysctl.proc_translated 2>/dev/null || echo 0)" = "1" ]; then
    fail "this Terminal runs under Rosetta (Intel emulation). Quit it, untick \"Open using Rosetta\" in its Get Info window, and run this again."
fi

# ---------------------------------------------------------------- toolchain

if command -v brew >/dev/null 2>&1; then
    BREW="$(command -v brew)"
elif [ -x /opt/homebrew/bin/brew ]; then
    BREW=/opt/homebrew/bin/brew
elif [ -x /usr/local/bin/brew ]; then
    BREW=/usr/local/bin/brew
else
    fail "Homebrew is not installed. Run scripts/setup-macos.sh first."
fi
eval "$("$BREW" shellenv)"
export HOMEBREW_NO_ENV_HINTS=1

"$BREW" list --formula --versions qt >/dev/null 2>&1 \
    || fail "Qt is not installed. Run scripts/setup-macos.sh first."
command -v cmake >/dev/null 2>&1 || fail "CMake is not installed. Run scripts/setup-macos.sh first."

BREW_PREFIX="$("$BREW" --prefix)"
QT_PREFIX="$("$BREW" --prefix qt)"
# Qt's own keg first, then Homebrew's prefix, where every Qt module is linked
# whether Homebrew ships Qt as one formula or as several.
PREFIX_PATH="$QT_PREFIX;$BREW_PREFIX"

# ------------------------------------------------------------------- Xcode

if [ "$xcode" -eq 1 ]; then
    xcodebuild -version >/dev/null 2>&1 \
        || fail "an Xcode project needs the full Xcode (App Store), not only the Command Line Tools. Without it, run this script without --xcode."
    BUILD="$SRC/build-xcode"
    step "Generating the Xcode project"
    cmake -S "$SRC" -B "$BUILD" -G Xcode -DCMAKE_PREFIX_PATH="$PREFIX_PATH"
    project="$BUILD/Monolist.xcodeproj"
    [ -d "$project" ] || fail "CMake did not write $project."
    open "$project"
    say ""
    say "Opened $project"
    say "In Xcode: pick the \"monolist\" scheme and \"My Mac\" at the top, then Product › Run (⌘R)."
    say "That build runs from Xcode's build folder using Homebrew's Qt and libmpv. For an app"
    say "to keep in Applications, run: scripts/build-macos.sh --install"
    exit 0
fi

# ------------------------------------------------------------------- build

BUILD="$SRC/build-macos"
if command -v ninja >/dev/null 2>&1; then
    generator="Ninja"
else
    generator="Unix Makefiles"
fi

step "Configuring ($config, $generator)"
cmake -S "$SRC" -B "$BUILD" -G "$generator" \
      -DCMAKE_BUILD_TYPE="$config" \
      -DCMAKE_PREFIX_PATH="$PREFIX_PATH"

step "Building"
cmake --build "$BUILD" --parallel

BUILT_APP="$BUILD/Monolist.app"
[ -d "$BUILT_APP" ] || fail "the build did not produce $BUILT_APP."
APP="$BUILT_APP"

# ------------------------------------------------------------------ deploy

if [ "$deploy" -eq 1 ]; then
    step "Putting Qt and libmpv inside the app"

    macdeployqt=""
    for candidate in "$QT_PREFIX/bin/macdeployqt" \
                     "$("$BREW" --prefix qtbase 2>/dev/null)/bin/macdeployqt" \
                     "$BREW_PREFIX/bin/macdeployqt" \
                     "$(command -v macdeployqt 2>/dev/null)" \
                     "$(command -v macdeployqt6 2>/dev/null)"; do
        if [ -n "$candidate" ] && [ -x "$candidate" ]; then
            macdeployqt="$candidate"
            break
        fi
    done
    [ -n "$macdeployqt" ] || fail "macdeployqt was not found in Homebrew's Qt. Run with --no-deploy, or reinstall Qt: brew reinstall qt"

    DIST="$BUILD/dist"
    APP="$DIST/Monolist.app"
    mkdir -p "$DIST"
    rm -rf "$APP"
    ditto "$BUILT_APP" "$APP"

    deploy_options=""
    if [ "$config" = "Debug" ]; then
        deploy_options="-no-strip"   # keep the symbols a debugger wants
    fi
    # -qmldir: what the interface imports (QtQuick, the Basic controls, Shapes)
    # -libpath: where libmpv's own libraries are, for the ones it names by @rpath
    # shellcheck disable=SC2086  # options, split on purpose
    "$macdeployqt" "$APP" \
        -qmldir="$SRC" \
        -libpath="$BREW_PREFIX/lib" \
        -verbose=1 \
        $deploy_options

    # Anything still loaded from Homebrew would tie the app to this Mac's
    # Homebrew, and break when it upgrades.
    leftovers="$(find "$APP/Contents" -type f \( -name '*.dylib' -o -perm -u+x \) -print0 \
                 | xargs -0 otool -L 2>/dev/null \
                 | awk '/^[[:space:]]/ {print $1}' \
                 | grep -E "^($BREW_PREFIX|/usr/local|/opt/local)/" \
                 | sort -u || true)"
    if [ -n "$leftovers" ]; then
        warn "these are still loaded from outside the app, so it runs on this Mac but is not self-contained:"
        # shellcheck disable=SC2086  # one path per line, split on purpose
        printf '    %s\n' $leftovers >&2
    else
        say "Self-contained: nothing is loaded from Homebrew."
    fi
fi

# --------------------------------------------------------------------- sign

# Ad hoc ("Sign to Run Locally"). Apple silicon runs nothing unsigned, and
# macdeployqt's changes void the linker's own signature. No account needed.
step "Signing (ad hoc)"
codesign --force --deep --sign - "$APP"
if codesign --verify --deep --strict "$APP"; then
    say "Signature verified."
else
    fail "the signature does not verify; macOS would refuse to open the app."
fi

# ---------------------------------------------------------------------- dmg

if [ "$dmg" -eq 1 ]; then
    step "Making the disk image"
    DMG="$(dirname "$APP")/Monolist.dmg"
    staging="$(mktemp -d)"
    ditto "$APP" "$staging/Monolist.app"
    ln -s /Applications "$staging/Applications"
    rm -f "$DMG"
    hdiutil create -volname "Monolist" -srcfolder "$staging" -ov -format UDZO "$DMG" >/dev/null
    rm -rf "$staging"
    say "$DMG"
fi

# ------------------------------------------------------------------ install

if [ "$install" -eq 1 ]; then
    step "Installing"
    [ "$deploy" -eq 1 ] || warn "installing a --no-deploy build: it needs Homebrew's Qt and libmpv to stay installed."

    target_dir=/Applications
    if [ ! -w "$target_dir" ]; then
        target_dir="$HOME/Applications"
        mkdir -p "$target_dir"
    fi
    target="$target_dir/Monolist.app"

    if [ -d "$target" ]; then
        existing="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$target/Contents/Info.plist" 2>/dev/null || true)"
        if [ -n "$existing" ] && [ "$existing" != "$BUNDLE_ID" ]; then
            fail "$target is another app ($existing); not replacing it."
        fi
        if pgrep -x Monolist >/dev/null 2>&1; then
            warn "Monolist is running; quit it and reopen it to get this build."
        fi
        rm -rf "$target"
    fi
    ditto "$APP" "$target"
    APP="$target"
    say "Installed $target"
fi

# ---------------------------------------------------------------------- done

say ""
say "Monolist.app: $APP"
if [ "$run" -eq 1 ]; then
    open "$APP"
fi
