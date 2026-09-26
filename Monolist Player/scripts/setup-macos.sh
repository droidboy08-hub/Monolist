#!/bin/bash
# Installs what Monolist needs to build and run on a Mac, through Homebrew.
#
#     scripts/setup-macos.sh             install whatever is missing
#     scripts/setup-macos.sh --update    update yt-dlp, FFmpeg and Deno
#
# To build:  qt (Qt 6), mpv (libmpv), cmake, ninja and pkgconf.
# To run:    yt-dlp, ffmpeg and deno, which the app finds in Homebrew's bin
#            folder even when it is opened from Finder or the Dock.
#
# Re-running skips what is already there. --update is what Settings ›
# Update components runs: YouTube changes often, and yt-dlp, FFmpeg and Deno
# should follow their latest releases. It prints one line per tool, which the
# app shows as it goes.
#
# Written for the bash that ships with macOS (3.2).

set -eo pipefail

BUILD_FORMULAE="qt mpv cmake ninja pkgconf"
TOOL_FORMULAE="yt-dlp ffmpeg deno"

update_only=0
for argument in "$@"; do
    case "$argument" in
        --update) update_only=1 ;;
        -h|--help)
            sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *)
            echo "Unknown option: $argument (try --help)" >&2
            exit 2 ;;
    esac
done

say()  { printf '%s\n' "$*"; }
fail() { printf 'Error: %s\n' "$*" >&2; exit 1; }

[ "$(uname -s)" = "Darwin" ] || fail "this script is for macOS. On Windows use scripts/setup-windows.ps1."

# Homebrew's own advice and hints are noise here, most of all in the app.
export HOMEBREW_NO_ENV_HINTS=1

# ---------------------------------------------------------------- Homebrew

find_brew() {
    if command -v brew >/dev/null 2>&1; then
        command -v brew
    elif [ -x /opt/homebrew/bin/brew ]; then
        echo /opt/homebrew/bin/brew        # Apple silicon
    elif [ -x /usr/local/bin/brew ]; then
        echo /usr/local/bin/brew           # Intel
    fi
}

BREW="$(find_brew)"
if [ -z "$BREW" ]; then
    if [ "$update_only" -eq 1 ] || [ ! -t 0 ]; then
        fail "Homebrew is not installed. Install it from https://brew.sh, then run scripts/setup-macos.sh."
    fi
    say "Homebrew is not installed. Monolist gets Qt, libmpv and its tools from it."
    printf 'Install Homebrew now, with the installer from https://brew.sh? [y/N] '
    read -r answer
    case "$answer" in
        y|Y|yes|YES) ;;
        *) fail "Homebrew is needed. Install it from https://brew.sh and run this again." ;;
    esac
    /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
    BREW="$(find_brew)"
    [ -n "$BREW" ] || fail "Homebrew did not install. See https://brew.sh."
fi
# Homebrew's environment, so this shell (and anything it starts) finds what
# it installs even when brew is not on PATH yet.
eval "$("$BREW" shellenv)"

installed() {
    "$BREW" list --formula --versions "$1" >/dev/null 2>&1
}

# The first line a tool gives about its version, for the report.
version_of() {
    case "$1" in
        yt-dlp) yt-dlp --version 2>/dev/null | head -n 1 ;;
        ffmpeg) ffmpeg -version 2>/dev/null | head -n 1 | sed 's/^ffmpeg version //; s/ Copyright.*//' ;;
        deno)   deno --version 2>/dev/null | head -n 1 | sed 's/^deno //; s/ .*//' ;;
    esac
}

# ------------------------------------------------------------------ update

if [ "$update_only" -eq 1 ]; then
    for formula in $TOOL_FORMULAE; do
        if installed "$formula"; then
            say "Updating $formula…"
            # Quietly; a failure is tried once more with its errors showing,
            # so the app has the reason to show.
            "$BREW" upgrade "$formula" >/dev/null 2>&1 \
                || "$BREW" upgrade "$formula" >/dev/null
        else
            say "Installing $formula…"
            "$BREW" install "$formula" >/dev/null
        fi
        say "$formula $(version_of "$formula")"
    done
    say "The tools are up to date."
    exit 0
fi

# ----------------------------------------------------------------- install

# Building needs Apple's compiler: the Command Line Tools at least, and the
# full Xcode for an Xcode project.
if ! xcode-select -p >/dev/null 2>&1; then
    say "Apple's Command Line Tools are not installed; asking macOS to install them."
    xcode-select --install || true
    fail "Finish the Command Line Tools install in the window macOS opened, then run this again."
fi

missing=""
for formula in $BUILD_FORMULAE $TOOL_FORMULAE; do
    if installed "$formula"; then
        say "✓ $formula"
    else
        missing="$missing $formula"
    fi
done

if [ -n "$missing" ]; then
    say "Installing:$missing"
    say "(Qt and mpv are large; the first install takes a while.)"
    # shellcheck disable=SC2086  # a list of formula names, split on purpose
    "$BREW" install $missing
fi

# Qt 6.6 is the floor (CMakeLists.txt); 6.9 or later puts the design under a
# transparent title bar, which is how the Mac build is meant to look.
qt_version="$("$BREW" list --versions qt | awk '{print $2}')"
say ""
say "Qt       $qt_version  ($("$BREW" --prefix qt))"
say "libmpv   $("$BREW" list --versions mpv | awk '{print $2}')"
for formula in $TOOL_FORMULAE; do
    printf '%-8s %s\n' "$formula" "$(version_of "$formula")"
done

case "$qt_version" in
    6.[0-5].*) fail "Qt $qt_version is too old; Monolist needs Qt 6.6 or later. Run: brew upgrade qt" ;;
    6.[6-8].*) say "Note: with Qt $qt_version the window keeps the system title bar; Qt 6.9 or later puts the design under it." ;;
esac

if xcodebuild -version >/dev/null 2>&1; then
    xcode="$(xcodebuild -version | head -n 1)"
else
    xcode=""
fi

say ""
say "Ready. Next:"
say "  scripts/build-macos.sh --install     build Monolist.app and put it in Applications"
if [ -n "$xcode" ]; then
    say "  scripts/build-macos.sh --xcode       make an Xcode project and open it ($xcode)"
else
    say "  (An Xcode project needs the full Xcode from the App Store; the build above does not.)"
fi
