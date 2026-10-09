#include "trayselftest.h"

#include "library.h"
#include "tray.h"

#include <QAction>
#include <QMenu>

namespace {

// One line per check, and a count at the end, as in the other self-tests.
class Checks
{
public:
    bool check(bool ok, const QString &what, const QString &detail = QString())
    {
        ++m_count;
        if (!ok)
            ++m_failed;
        const QString line = ok || detail.isEmpty() ? what : what + QStringLiteral("  -- ") + detail;
        qWarning("tray-test: %s  %s", ok ? "ok  " : "FAIL", qPrintable(line));
        return ok;
    }

    int finish()
    {
        qWarning("tray-test: %d checks, %d failed", m_count, m_failed);
        return m_failed;
    }

private:
    int m_count = 0;
    int m_failed = 0;
};

QStringList texts(const QMenu *menu)
{
    QStringList list;
    if (!menu)
        return list;
    for (const QAction *action : menu->actions())
        list << (action->isSeparator() ? QStringLiteral("-") : action->text());
    return list;
}

} // namespace

int runTraySelfTest(Library *library)
{
    Checks t;
    if (qEnvironmentVariable("MONOLIST_DATA_DIR").isEmpty()) {
        qWarning("tray-test: needs MONOLIST_DATA_DIR (a scratch folder)");
        return 1;
    }
    library->setSetting(QStringLiteral("window.close_to_tray"), QString());

    {
        Tray tray(library, nullptr);
#ifdef Q_OS_MACOS
        t.check(!tray.available(), QStringLiteral("not on the Mac"));
#else
        t.check(tray.available(), QStringLiteral("this desktop has a system tray"));
#endif
        t.check(tray.closeToTray(), QStringLiteral("on unless turned off"));
        t.check(!tray.ending(), QStringLiteral("  and nothing is ending"));
        if (tray.available()) {
            const QStringList menu = texts(tray.menu());
            t.check(menu == QStringList({ QStringLiteral("Nothing playing"), QStringLiteral("-"), QStringLiteral("Play"),
                                          QStringLiteral("Next"), QStringLiteral("Previous"), QStringLiteral("-"),
                                          QStringLiteral("Open Monolist"), QStringLiteral("Mini player"),
                                          QStringLiteral("-"),
                                          QStringLiteral("Quit Monolist") }),
                    QStringLiteral("the menu: the song, play, next, previous, the window, the mini player, quit"),
                    menu.join(QStringLiteral(" | ")));
            t.check(!tray.menu()->actions().at(0)->isEnabled() && !tray.menu()->actions().at(2)->isEnabled(),
                    QStringLiteral("  with nothing playing, the song line and Play are greyed"));
            t.check(tray.toolTip() == QLatin1String("Monolist"), QStringLiteral("  and the tooltip is the name alone"));

            int opened = 0;
            int minis = 0;
            QObject::connect(&tray, &Tray::openRequested, [&opened]() { ++opened; });
            QObject::connect(&tray, &Tray::miniRequested, [&minis]() { ++minis; });
            tray.menu()->actions().at(6)->trigger();
            t.check(opened == 1 && minis == 0, QStringLiteral("Open Monolist asks for the full window"));
            tray.menu()->actions().at(7)->trigger();
            t.check(minis == 1, QStringLiteral("Mini player asks for the mini player"));
        }

        tray.setCloseToTray(false);
        t.check(!tray.closeToTray()
                    && library->settingValue(QStringLiteral("window.close_to_tray")) == QLatin1String("0"),
                QStringLiteral("turned off, and kept"));
        // Its note is for a window that went to the tray; with the switch
        // off none does, and none is said (nor shown on this desktop).
        tray.hidden();
        t.check(library->settingValue(QStringLiteral("window.tray_told")).isEmpty(),
                QStringLiteral("  and no note about the tray"));
    }
    {
        Tray next(library, nullptr);
        t.check(!next.closeToTray(), QStringLiteral("the next launch: still off"));
        next.setCloseToTray(true);
    }
    {
        Tray again(library, nullptr);
        t.check(again.closeToTray(), QStringLiteral("turned on again, and kept"));
    }
    return t.finish();
}
