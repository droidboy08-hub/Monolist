#pragma once

#include "appupdater.h"

#include <QByteArray>
#include <QString>
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

} // namespace MacUpdate
