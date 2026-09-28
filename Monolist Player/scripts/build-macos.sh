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
#     --no-deploy  Leave Qt, libmpv and FFmpeg out of the bundle. Quicker, and
#                  fine on this Mac, but the app then needs Homebrew's copies.
#     --no-tools   Do not download yt-dlp and Deno to carry inside the app
#                  (offline builds). The app then uses Homebrew's, or fetches
#                  them itself from Settings › Update components.
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
tools=1
xcode=0

for argument in "$@"; do
    case "$argument" in
        --install)   install=1 ;;
        --dmg)       dmg=1 ;;
        --run)       run=1 ;;
        --debug)     config=Debug ;;
        --no-deploy) deploy=0 ;;
        --no-tools)  tools=0 ;;
        --xcode)     xcode=1 ;;
        -h|--help)
            sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'
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

    # FFmpeg beside the executable, where the app looks first. Homebrew's
    # build shares its libraries with libmpv, so macdeployqt (-executable
    # below) brings in little that is not there already. It is updated with
    # the app: YouTube's changes do not reach it.
    with_ffmpeg=0
    if [ -x "$BREW_PREFIX/bin/ffmpeg" ] && [ -x "$BREW_PREFIX/bin/ffprobe" ]; then
        for program in ffmpeg ffprobe; do
            cp -L "$BREW_PREFIX/bin/$program" "$APP/Contents/MacOS/$program"
            chmod u+w "$APP/Contents/MacOS/$program"
        done
        with_ffmpeg=1
    else
        warn "FFmpeg is not installed (brew install ffmpeg), so the app will look for it on PATH."
    fi

    deploy_options=""
    if [ "$config" = "Debug" ]; then
        deploy_options="-no-strip"   # keep the symbols a debugger wants
    fi
    # -qmldir: what the interface imports (QtQuick, the Basic controls, Shapes)
    # -libpath: where libmpv's own libraries are, for the ones it names by @rpath
    # The FFmpeg options carry the app's path, which has a space in it
    # ("Monolist Player"), so they are passed as separate words by hand.
    set -- -qmldir="$SRC" -libpath="$BREW_PREFIX/lib" -verbose=1
    [ -n "$deploy_options" ] && set -- "$@" "$deploy_options"
    [ "$with_ffmpeg" -eq 1 ] && set -- "$@" \
        "-executable=$APP/Contents/MacOS/ffmpeg" "-executable=$APP/Contents/MacOS/ffprobe"
    "$macdeployqt" "$APP" "$@"

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

# -------------------------------------------------------------------- tools

# yt-dlp and Deno travel inside the app as the archives their projects
# publish, checked against the SHA-256 each publishes, and are unpacked on the
# app's first launch into Application Support, where the app keeps them
# current (src/macos/toolstore.*). Downloads are kept in build-macos/tools
# for the next build, and used when GitHub cannot be reached.

# The version GitHub's "latest" download link redirects to, or nothing.
latest_tag() {
    curl -fsS -o /dev/null -w '%{redirect_url}' --max-time 30 \
         "https://github.com/$1/releases/latest/download/$2" 2>/dev/null \
        | sed -n 's#.*/releases/download/\([^/]*\)/.*#\1#p'
}

# bundle_tool <name> <repo> <asset> <checksum file>: prints the version.
bundle_tool() {
    name="$1"; repo="$2"; asset="$3"; checksums="$4"
    cache="$BUILD/tools"
    tag="$(latest_tag "$repo" "$checksums" || true)"
    if [ -n "$tag" ]; then
        folder="$cache/$name-${tag#v}"
        if [ ! -f "$folder/$asset" ] || [ ! -f "$folder/$checksums" ]; then
            mkdir -p "$folder"
            say "Downloading $name ${tag#v}…" >&2
            if ! curl -fsSL -o "$folder/$checksums" "https://github.com/$repo/releases/download/$tag/$checksums" \
                || ! curl -fL --progress-bar -o "$folder/$asset" "https://github.com/$repo/releases/download/$tag/$asset"; then
                rm -rf "$folder"
                fail "could not download $name $tag."
            fi
        fi
    else
        # Offline: the newest one downloaded before, if there is one. The
        # folders are this script's own, named <tool>-<version>.
        # shellcheck disable=SC2012
        folder="$(ls -dt "$cache/$name-"* 2>/dev/null | head -n 1 || true)"
        [ -n "$folder" ] || { warn "GitHub cannot be reached and no $name was downloaded before; the app will not carry it."; return 0; }
        warn "GitHub cannot be reached; using $(basename "$folder")."
    fi

    expected="$(grep "$asset" "$folder/$checksums" | head -n 1 | awk '{print $1}')"
    actual="$(shasum -a 256 "$folder/$asset" | awk '{print $1}')"
    if [ -z "$expected" ] || [ "$expected" != "$actual" ]; then
        rm -rf "$folder"
        fail "$name did not match the checksum its project publishes; not bundling it. Run again to download it afresh."
    fi
    cp "$folder/$asset" "$APP/Contents/Resources/tools/$name.zip"
    basename "$folder" | sed "s/^$name-//"
}

if [ "$tools" -eq 1 ]; then
    step "Putting yt-dlp and Deno inside the app"
    rm -rf "$APP/Contents/Resources/tools"
    mkdir -p "$APP/Contents/Resources/tools"
    case "$(uname -m)" in
        arm64) deno_arch=aarch64 ;;
        *)     deno_arch=x86_64 ;;
    esac
    deno_asset="deno-$deno_arch-apple-darwin.zip"
    ytdlp_version="$(bundle_tool yt-dlp yt-dlp/yt-dlp yt-dlp_macos.zip SHA2-256SUMS)"
    deno_version="$(bundle_tool deno denoland/deno "$deno_asset" "$deno_asset.sha256sum")"

    manifest="{"
    [ -n "$ytdlp_version" ] && manifest="$manifest\"yt-dlp\": \"$ytdlp_version\""
    if [ -n "$deno_version" ]; then
        [ "$manifest" = "{" ] || manifest="$manifest, "
        manifest="$manifest\"deno\": \"$deno_version\""
    fi
    printf '%s}\n' "$manifest" > "$APP/Contents/Resources/tools/tools.json"
    say "yt-dlp ${ytdlp_version:-(none)}, Deno ${deno_version:-(none)}"
fi

# --------------------------------------------------------------------- sign

# Ad hoc ("Sign to Run Locally"). Apple silicon runs nothing unsigned, and
# macdeployqt's changes void the linker's own signature. No account needed.
step "Signing (ad hoc)"
# Helper programs beside the executable are signed first, on their own:
# signing the app seals them, but does not sign them.
for program in ffmpeg ffprobe; do
    if [ -f "$APP/Contents/MacOS/$program" ]; then
        codesign --force --sign - "$APP/Contents/MacOS/$program"
    fi
done
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
