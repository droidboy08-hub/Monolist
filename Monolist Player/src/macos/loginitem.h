#pragma once

class QString;

// The ServiceManagement side of Startup on macOS. Plain C++ here, so that
// startup.cpp needs no Objective-C; the work is in loginitem.mm.
namespace MacLoginItem {

// The user turned Monolist's agent off in System Settings › General › Login
// Items & Extensions ("Allow in the Background"), so launchd will not start
// it at login whatever it says. macOS 13 and later; false before, and for a
// file macOS has never seen (a self-test's scratch agent).
bool turnedOff(const QString &agentPath);

} // namespace MacLoginItem
