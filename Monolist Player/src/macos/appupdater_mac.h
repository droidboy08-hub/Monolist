#pragma once

#include "appupdater.h"

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVariantMap>

// The Mac's part of the in-app update (appupdater_mac.mm), named here for
// the test (--update-test) as well as AppUpdater.
namespace MacUpdate {

// The code requirement a Developer ID copy of Monolist meets: Apple's
// Developer ID chain, the "Developer ID Application" kind, and the project's
// team. The update of such a copy has to meet it too.
QString developerIdRequirement();

// A bundle as an update target: where it is, and why it cannot be replaced
// there, if it cannot (blockerFor). `developerId`: whether it is signed so.
UpdateTarget describeBundle(const QString &bundle, bool developerId);
// Empty when the bundle can be replaced in place; else the reason, in words:
// it runs from a disk image or another read-only volume, from where macOS
// moved it to on opening it (App Translocation), from a folder Monolist may
// not change, or from a build folder.
QString blockerFor(const QString &bundle);

// Whether this running copy is signed with the project's Developer ID.
bool runningIsDeveloperId();

// CFBundleIdentifier, CFBundleVersion and CFBundleShortVersionString, read
// afresh from the bundle's Info.plist (never cached, as NSBundle would).
QVariantMap bundleInfo(const QString &bundle);

// The mount point in `hdiutil attach -plist`'s answer, or empty; and the
// image's own device (/dev/diskN), by which it is let go of even when
// nothing of it was mounted.
QString mountPointFrom(const QByteArray &plist);
QString deviceFrom(const QByteArray &plist);
// From `hdiutil info -plist`: the devices of the images attached from a
// file under `folder` (a launch lets go of those a crash left).
QStringList devicesAttachedFrom(const QByteArray &plist, const QString &folder);

// Whether macOS has the file marked as downloaded (com.apple.quarantine),
// so that Gatekeeper checks what comes out of it.
bool isQuarantined(const QString &path);

// The helper's first argument after --update-swap: what this build's
// helper understands. The arguments are a contract within one build only:
// the running Monolist starts its own program as the helper.
inline constexpr char kSwapProtocol[] = "1";

// --update-swap: this program, run by the Monolist that is updating (its
// own executable, signed by the same developer as the app it replaces, as
// macOS's App Management asks of whatever changes a notarised app), once
// that Monolist has quit: waits for it to end, checks the new app's
// signature once more, swaps old for new with a way back, registers the new
// one with LaunchServices, opens it, waits for it to start, and only then
// deletes the old one. Its arguments, after the flag:
//   protocol  pid  app  new  aside  lsregister  opener  result  started
//   quitSeconds  startSeconds  requirement  NAME=value...
// Called from main() before anything else; returns the exit code.
int runSwapHelper(int argc, char **argv);

} // namespace MacUpdate
