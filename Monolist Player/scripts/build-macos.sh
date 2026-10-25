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
#     --sign <identity>
#                  Also sign with a Developer ID, for other Macs to open
#                  without a warning: "Developer ID Application: Name (TEAMID)"
#                  or its SHA-1, as `security find-identity -v -p codesigning`
#                  lists it (or MONOLIST_SIGN_IDENTITY). Hardened runtime,
#                  secure timestamps, every piece signed inside out, yt-dlp's
#                  too. Makes both disk images: Monolist-unsigned.dmg (the ad
#                  hoc build, as without --sign) and Monolist.dmg (signed).
#     --notarize <profile>
#                  With --sign: Apple's notary service checks the app and the
#                  disk image, and both get its ticket stapled on. <profile>
#                  holds the credentials, kept once with `xcrun notarytool
#                  store-credentials <profile>` (or MONOLIST_NOTARY_PROFILE).
#                  CI instead sets MONOLIST_NOTARY_KEY (an App Store Connect
#                  API key's .p8 file), MONOLIST_NOTARY_KEY_ID and
#                  MONOLIST_NOTARY_ISSUER.
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
identity="${MONOLIST_SIGN_IDENTITY:-}"
notary_profile="${MONOLIST_NOTARY_PROFILE:-}"
notarize=0
[ -n "$notary_profile" ] && notarize=1
[ -n "${MONOLIST_NOTARY_KEY:-}" ] && notarize=1

while [ $# -gt 0 ]; do
    case "$1" in
        --install)   install=1 ;;
        --dmg)       dmg=1 ;;
        --run)       run=1 ;;
        --debug)     config=Debug ;;
        --no-deploy) deploy=0 ;;
        --no-tools)  tools=0 ;;
        --xcode)     xcode=1 ;;
        --sign)
            [ $# -ge 2 ] || { echo "--sign needs an identity (try --help)" >&2; exit 2; }
            identity="$2"; shift ;;
        --sign=*)    identity="${1#--sign=}" ;;
        --notarize)
            [ $# -ge 2 ] || { echo "--notarize needs a notarytool profile (try --help)" >&2; exit 2; }
            notary_profile="$2"; notarize=1; shift ;;
        --notarize=*) notary_profile="${1#--notarize=}"; notarize=1 ;;
        -h|--help)
            sed -n '2,41p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *)
            echo "Unknown option: $1 (try --help)" >&2
            exit 2 ;;
    esac
    shift
done

say()  { printf '%s\n' "$*"; }
step() { printf '\n== %s\n' "$*"; }
warn() { printf 'Warning: %s\n' "$*" >&2; }
fail() { printf 'Error: %s\n' "$*" >&2; exit 1; }

[ "$(uname -s)" = "Darwin" ] || fail "this script is for macOS. On Windows use scripts/build-windows.ps1."

# Signing for other Macs: asked about now, before minutes of building.
if [ -n "$identity" ]; then
    [ "$xcode" -eq 0 ] || fail "--sign is for this script's own build, not --xcode."
    [ "$deploy" -eq 1 ] || fail "--sign needs the self-contained app; drop --no-deploy."
    security find-identity -v -p codesigning | grep -F -q -- "$identity" \
        || fail "no code-signing identity \"$identity\" in the keychain. Those there: $(security find-identity -v -p codesigning | sed -n 's/.*"\(.*\)"/\1/p' | paste -sd ',' -)"
    case "$identity" in
        "Developer ID Application:"*|[0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f]*) ;;
        *) warn "\"$identity\" is not a Developer ID Application identity; Apple notarises only those, and an Apple Development identity runs on this Mac alone." ;;
    esac
    if [ "$notarize" -eq 1 ]; then
        if [ -n "$notary_profile" ]; then
            notary_auth="--keychain-profile"
        else
            [ -n "${MONOLIST_NOTARY_KEY_ID:-}" ] && [ -n "${MONOLIST_NOTARY_ISSUER:-}" ] && [ -f "${MONOLIST_NOTARY_KEY:-}" ] \
                || fail "notarising with an API key needs MONOLIST_NOTARY_KEY (a .p8 file), MONOLIST_NOTARY_KEY_ID and MONOLIST_NOTARY_ISSUER."
            notary_auth="--key"
        fi
    fi
    dmg=1
elif [ "$notarize" -eq 1 ]; then
    fail "--notarize needs --sign: Apple notarises only what a Developer ID signed."
fi

SRC="$(cd "$(dirname "$0")/.." && pwd)"
BUNDLE_ID="io.github.droidboy08-hub.monolist"

# A Terminal running under Rosetta builds for Intel, and Homebrew's libraries
# on Apple silicon are not Intel: the link fails with a wall of errors.
if [ "$(sysctl -n sysctl.proc_translated 2>/dev/null || echo 0)" = "1" ]; then
    fail "this Terminal runs under Rosetta (Intel emulation). Quit it, untick \"Open using Rosetta\" in its Get Info window, and run this again."
fi

# --------------------------------------------------------------- the keys

# The Last.fm key and the Google Drive backup's sign-in client are the
# builder's and never committed (README "Last.fm key", "Google sign-in
# client"). Taken from the environment, or else from the login Keychain,
# where they are kept once (as build-windows.ps1 takes them from the Windows
# user's environment):
#     security add-generic-password -U -a "$USER" -s MONOLIST_GOOGLE_CLIENT_ID -w
# (it asks for the value, so it is never typed on a command line or kept in a
# shell's history). Nothing here prints a value: CMake says only whether
# there is a key and a client.
for name in MONOLIST_LASTFM_API_KEY MONOLIST_LASTFM_SHARED_SECRET \
            MONOLIST_GOOGLE_CLIENT_ID MONOLIST_GOOGLE_CLIENT_SECRET; do
    eval "value=\${$name:-}"
    if [ -z "$value" ]; then
        value="$(security find-generic-password -a "$USER" -s "$name" -w 2>/dev/null || true)"
        if [ -n "$value" ]; then
            export "$name=$value"
        fi
    fi
done
unset value

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

    # Plugins of Qt modules Monolist does not use (Homebrew's Qt has every
    # module: the virtual keyboard, 3D, PDF, timelines), which macdeployqt
    # copies without the frameworks they need. Each would load Homebrew's
    # copy of what it lacks, and with it a second QtCore and QtGui beside
    # the app's own: the PDF image plugin did, at the first cover shown.
    find "$APP/Contents/PlugIns" -name '*.dylib' -print0 \
        | while IFS= read -r -d '' plugin; do
            for framework in $(otool -L "$plugin" | awk '/@rpath\/[^ ]*\.framework/ {print $1}' \
                                   | sed 's#^@rpath/##; s#\.framework/.*#.framework#'); do
                if [ ! -d "$APP/Contents/Frameworks/$framework" ]; then
                    rm -f "$plugin"
                    say "Left out ${plugin#$APP/Contents/} (it needs $framework, which Monolist does not use)"
                    break
                fi
            done
        done
    # And the links macdeployqt made to them from their QML modules, which
    # point at nothing now and would fail the signature.
    find "$APP/Contents" -type l ! -exec test -e {} \; -delete

    # Search paths into Homebrew that the linker left in the executable (the
    # build tree's) and Homebrew in a few of its libraries, and libmpv's
    # into Xcode's Swift (the system's, /usr/lib/swift, stays). With them,
    # anything the bundle lacks is quietly loaded from this Mac's Homebrew
    # beside the bundle's own copies; without them it fails, and shows.
    find "$APP/Contents" -type f \( -name '*.dylib' -o -perm -u+x \) -print0 \
        | while IFS= read -r -d '' binary; do
            otool -l "$binary" 2>/dev/null \
                | awk '/cmd LC_RPATH/ {rpath = 1; next} rpath && /path / {print $2; rpath = 0}' \
                | grep -E "^($BREW_PREFIX|/usr/local|/opt/local|/Applications/Xcode[^/]*\.app)/" \
                | while IFS= read -r path; do
                    install_name_tool -delete_rpath "$path" "$binary" 2>/dev/null \
                        && say "Removed the search path $path from ${binary#$APP/}"
                done || true
        done

    # The qt.conf the build tree's app carries names the build folder for
    # QML imports (QmlImports = <this Mac's build-macos>, merged with Qt's
    # own): shipped, it put a path of this Mac's into the app, and on this
    # Mac had QML looked for in the build folder before the app's own. The
    # app's own folders, and nothing else.
    printf '[Paths]\nPlugins = PlugIns\nQmlImports = Resources/qml\n' > "$APP/Contents/Resources/qt.conf"

    # The oldest macOS the app runs on is the newest that any of its code was
    # built for: Homebrew builds for the macOS it runs on (macOS 26 code from
    # a Mac on 26). Info.plist says so, so that an older Mac refuses it with
    # macOS's own message rather than a crash at launch.
    minimum="$(find "$APP/Contents" -type f \( -name '*.dylib' -o -perm -u+x \) -print0 \
        | xargs -0 -n 1 otool -l 2>/dev/null \
        | awk '/cmd LC_BUILD_VERSION/ {build = 1; next} build && $1 == "minos" {print $2; build = 0}' \
        | sort -t . -k 1,1n -k 2,2n -k 3,3n | tail -n 1 || true)"
    if [ -n "$minimum" ]; then
        /usr/libexec/PlistBuddy -c "Set :LSMinimumSystemVersion $minimum" "$APP/Contents/Info.plist"
        say "Runs on macOS $minimum or later (the newest its libraries were built for)."
    fi

    # Anything still loaded from Homebrew would tie the app to this Mac's
    # Homebrew, and break when it upgrades.
    leftovers="$(find "$APP/Contents" -type f \( -name '*.dylib' -o -perm -u+x \) -print0 \
                 | xargs -0 otool -L 2>/dev/null \
                 | awk '/^[[:space:]]/ {print $1}' \
                 | grep -E "^($BREW_PREFIX|/usr/local|/opt/local|/Applications/Xcode[^/]*\.app)/" \
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

# make_dmg <dmg>: the app as it is now, beside a link to Applications.
make_dmg() {
    staging="$(mktemp -d)"
    ditto "$APP" "$staging/Monolist.app"
    ln -s /Applications "$staging/Applications"
    rm -f "$1"
    hdiutil create -volname "Monolist" -srcfolder "$staging" -ov -format UDZO "$1" >/dev/null
    rm -rf "$staging"
    say "$1"
}

if [ "$dmg" -eq 1 ]; then
    if [ -n "$identity" ]; then
        # Kept beside the signed one: the build that needs no Apple account.
        step "Making the unsigned disk image"
        make_dmg "$(dirname "$APP")/Monolist-unsigned.dmg"
    else
        step "Making the disk image"
        make_dmg "$(dirname "$APP")/Monolist.dmg"
    fi
fi

# ------------------------------------------------------------ Developer ID

# For other Macs to open the app with no warning: every piece of code signed
# with a Developer ID, from the inside out (a bundle's signature seals what
# is inside it, so what is inside is signed first), each with the hardened
# runtime and a secure timestamp, which notarisation requires; then Apple's
# notary service, whose ticket is stapled to the app and the disk image so
# that Gatekeeper need not ask Apple for it on a Mac that is offline.
# Checked on a build signed so, under the hardened runtime: the app needs no
# entitlements at all (no JIT: Homebrew's Qt runs QML without one, and
# scrolling, playback, the sign-in window and the Keychain were the same),
# as long as mpv's Lua scripts stay off on a Mac (MpvEngine): LuaJIT's code
# is killed there whatever the entitlements ("Code Signature Invalid").
# Library validation stays on: every library carries the same team.

# sign_code <path> [codesign options]: one file or bundle, quiet unless it fails.
sign_code() {
    target="$1"
    shift
    if ! out="$(codesign --force --timestamp --options runtime --sign "$identity" "$@" "$target" 2>&1)"; then
        printf '%s\n' "$out" >&2
        fail "codesign would not sign ${target#$APP/}."
    fi
}

# relink_framework <dir>: a framework whose links were packed as copies, as
# yt-dlp's archive packs its Python.framework (Versions/Current, Python and
# Resources each a second copy of Versions/<version>'s), made a framework
# again, links and all, so that codesign takes it for one ("bundle format is
# ambiguous" otherwise). Only copies of what the version holds are replaced.
relink_framework() {
    framework="$1"
    [ -d "$framework/Versions/Current" ] && [ ! -L "$framework/Versions/Current" ] || return 0
    version=""
    for candidate in "$framework"/Versions/*; do
        [ -L "$candidate" ] && continue
        [ "$(basename "$candidate")" = "Current" ] && continue
        [ -z "$version" ] || return 0   # more than one version: left as it is
        version="$(basename "$candidate")"
    done
    [ -n "$version" ] || return 0
    rm -rf "${framework:?}/Versions/Current"
    ln -s "$version" "$framework/Versions/Current"
    for entry in "$framework/Versions/$version"/*; do
        name="$(basename "$entry")"
        [ -e "$framework/$name" ] || continue
        rm -rf "${framework:?}/${name:?}"
        ln -s "Versions/Current/$name" "$framework/$name"
    done
}

# is_macho <file>: a program or library, of any architecture.
is_macho() {
    file -b "$1" | grep -q 'Mach-O'
}

# staple <file>: Apple's ticket attached to it. A ticket can take a while to
# be served after the notary service has accepted: on GitHub's Mac the disk
# image's first try failed seconds after "Accepted" (v0.1.177), so it is
# asked again, for up to five minutes, before giving up.
staple() {
    for wait in 0 10 20 30 60 90 90; do
        sleep "$wait"
        if xcrun stapler staple -q "$1" 2>/dev/null; then
            return 0
        fi
        say "The ticket for $(basename "$1") is not served yet; asking again."
    done
    xcrun stapler staple "$1" >&2 || true
    return 1
}

# notarize <file>: through Apple's notary service, waiting for its answer;
# its log is shown when it refuses.
notarize() {
    sent="$(basename "$1")"
    if [ "$notary_auth" = "--keychain-profile" ]; then
        set -- "$1" --keychain-profile "$notary_profile"
    else
        set -- "$1" --key "$MONOLIST_NOTARY_KEY" --key-id "$MONOLIST_NOTARY_KEY_ID" --issuer "$MONOLIST_NOTARY_ISSUER"
    fi
    say "Sending $sent to Apple's notary service (usually a few minutes)…"
    # Its answer, and its log, kept beside the build: the log lists every
    # piece of code the ticket covers, and any warnings, even when accepted.
    # One pair for each thing sent (Monolist-zip, Monolist-dmg), and none
    # left from an earlier run to be taken for this one's.
    tag="${sent%.*}-${sent##*.}"
    result="$BUILD/notary-$tag.json"
    log="$BUILD/notary-$tag-log.json"
    rm -f "$result" "$log"
    xcrun notarytool submit "$@" --wait --timeout 2h --output-format json > "$result" 2>&1 || true
    submission="$(plutil -extract id raw -o - "$result" 2>/dev/null || true)"
    status="$(plutil -extract status raw -o - "$result" 2>/dev/null || true)"
    if [ -n "$submission" ]; then
        shift
        xcrun notarytool log "$submission" "$@" "$log" >/dev/null 2>&1 || true
    fi
    if [ "$status" != "Accepted" ]; then
        cat "$result" >&2
        [ -f "$log" ] && cat "$log" >&2
        fail "Apple's notary service did not accept $sent (${status:-no answer})."
    fi
    say "Accepted ($submission); its log: $log"
}

if [ -n "$identity" ]; then
    step "Signing with $identity"

    # yt-dlp travels as its project's archive, unpacked at the first launch
    # (ToolStore, which does not check the bundled archive's checksum); its
    # build is signed ad hoc, which the notary service refuses inside an app.
    # Every program and library in it is signed afresh: the libraries, then
    # its Python.framework as a framework, then its program; and it is packed
    # again as it came. It needs no entitlements (checked: hardened, library
    # validation on, it extracts a video). Deno's is signed by its makers,
    # with the hardened runtime and a timestamp, and is left as it is.
    archive="$APP/Contents/Resources/tools/yt-dlp.zip"
    if [ -f "$archive" ]; then
        work="$(mktemp -d)"
        ditto -x -k --noqtn "$archive" "$work/yt-dlp"
        [ -f "$work/yt-dlp/yt-dlp_macos" ] \
            || fail "yt-dlp's archive no longer has yt-dlp_macos at its top; its layout changed."
        find "$work/yt-dlp" -type d -name '*.framework' -print0 \
            | while IFS= read -r -d '' framework; do relink_framework "$framework"; done
        # _internal/Python, PyInstaller's link to the framework's library,
        # came as one more copy of it.
        for library in "$work"/yt-dlp/_internal/Python.framework/Versions/*/Python; do
            [ -f "$library" ] && [ ! -L "$work/yt-dlp/_internal/Python" ] \
                && cmp -s "$library" "$work/yt-dlp/_internal/Python" || continue
            version="$(basename "$(dirname "$library")")"
            [ "$version" = "Current" ] && continue
            rm -f "$work/yt-dlp/_internal/Python"
            ln -s "Python.framework/Versions/$version/Python" "$work/yt-dlp/_internal/Python"
        done
        find "$work/yt-dlp" -mindepth 2 -type f ! -path '*.framework/*' -print0 \
            | while IFS= read -r -d '' file; do
                if is_macho "$file"; then sign_code "$file"; fi
            done
        find "$work/yt-dlp" -type d -name '*.framework' -prune -print0 \
            | while IFS= read -r -d '' framework; do sign_code "$framework"; done
        sign_code "$work/yt-dlp/yt-dlp_macos" --identifier "$BUNDLE_ID.yt-dlp"
        # Every program and library in it: this team's, hardened, timestamped.
        find "$work/yt-dlp" -type f -print0 \
            | while IFS= read -r -d '' file; do
                is_macho "$file" || continue
                details="$(codesign -dvv "$file" 2>&1)" || fail "${file#$work/} is not signed."
                case "$details" in *"(runtime)"*) ;; *) fail "${file#$work/} lacks the hardened runtime." ;; esac
                case "$details" in *"Timestamp="*) ;; *) fail "${file#$work/} lacks a secure timestamp." ;; esac
            done
        codesign --verify --strict "$work/yt-dlp/yt-dlp_macos" \
            || fail "yt-dlp's signature does not verify."
        rm -f "$archive"
        # Without the resource forks and attributes plain ditto adds as "._"
        # entries; its links stay links.
        ditto -c -k --norsrc --noextattr --noacl --noqtn "$work/yt-dlp" "$archive"
        rm -rf "${work:?}"
        say "yt-dlp signed, inside its archive."
    fi

    # Libraries, Qt's plugins and frameworks, and the helper programs beside
    # the executable; then the app itself.
    find "$APP/Contents/Frameworks" -maxdepth 1 -type f -name '*.dylib' -print0 \
        | while IFS= read -r -d '' file; do sign_code "$file"; done
    find "$APP/Contents/PlugIns" -type f -name '*.dylib' -print0 \
        | while IFS= read -r -d '' file; do sign_code "$file"; done
    for framework in "$APP"/Contents/Frameworks/*.framework; do
        sign_code "$framework"
    done
    for program in ffmpeg ffprobe; do
        if [ -f "$APP/Contents/MacOS/$program" ]; then
            sign_code "$APP/Contents/MacOS/$program" --identifier "$BUNDLE_ID.$program"
        fi
    done
    sign_code "$APP"

    codesign --verify --deep --strict "$APP" \
        || fail "the Developer ID signature does not verify."
    team="$(codesign -dv "$APP" 2>&1 | sed -n 's/^TeamIdentifier=//p')"
    say "Signed: team $team, hardened runtime, no entitlements."

    if [ "$notarize" -eq 1 ]; then
        step "Notarising"
        # The app on its own first, so that its ticket can be stapled to it:
        # copied out of the disk image, it then opens on a Mac that is offline.
        app_zip="$(mktemp -d)/Monolist.zip"
        ditto -c -k --keepParent "$APP" "$app_zip"
        notarize "$app_zip"
        rm -rf "$(dirname "$app_zip")"
        staple "$APP" || fail "the app's ticket could not be stapled."
    fi

    step "Making the signed disk image"
    DMG="$(dirname "$APP")/Monolist.dmg"
    make_dmg "$DMG"
    # The disk image's own signature: Gatekeeper checks it as it opens.
    codesign --force --timestamp --sign "$identity" --identifier "$BUNDLE_ID.dmg" "$DMG" >/dev/null 2>&1 \
        || fail "codesign would not sign the disk image."
    if [ "$notarize" -eq 1 ]; then
        notarize "$DMG"
        staple "$DMG" || fail "the disk image's ticket could not be stapled."
        # What another Mac's Gatekeeper will say, asked here.
        verdict="$(spctl --assess --type execute -v "$APP" 2>&1)" \
            || fail "Gatekeeper refuses the notarised app: $verdict"
        say "Gatekeeper, the app: $verdict"
        verdict="$(spctl --assess --type open --context context:primary-signature -v "$DMG" 2>&1)" \
            || fail "Gatekeeper refuses the notarised disk image: $verdict"
        say "Gatekeeper, the disk image: $verdict"
    else
        say "Not notarised (no --notarize): other Macs will say Apple could not check it."
    fi
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
