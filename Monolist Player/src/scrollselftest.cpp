#include "scrollselftest.h"
#include "artworkcache.h"

#include <QCoreApplication>
#include <QCursor>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QPointingDevice>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QScreen>
#include <QStyleHints>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

namespace {

// How evenly a run of intervals came, in milliseconds.
struct Spread {
    int count = 0;
    double median = 0;
    double p95 = 0;
    double longest = 0;
    int over25 = 0;
    int over50 = 0;
};

Spread spreadOf(std::vector<double> values)
{
    Spread s;
    s.count = int(values.size());
    if (values.empty())
        return s;
    std::sort(values.begin(), values.end());
    s.median = values[values.size() / 2];
    s.p95 = values[std::min(values.size() - 1, size_t(double(values.size()) * 0.95))];
    s.longest = values.back();
    for (const double v : values) {
        if (v > 25)
            ++s.over25;
        if (v > 50)
            ++s.over50;
    }
    return s;
}

QString framesText(const Spread &s)
{
    if (s.count == 0)
        return QStringLiteral("no frames drawn");
    return QStringLiteral("%1 frames, %2 ms apart at the median, %3 ms at the 95th percentile, %4 ms "
                          "at most; %5 over 25 ms, %6 over 50 ms")
        .arg(s.count + 1)
        .arg(s.median, 0, 'f', 1)
        .arg(s.p95, 0, 'f', 1)
        .arg(s.longest, 0, 'f', 1)
        .arg(s.over25)
        .arg(s.over50);
}

const char *apiName(QSGRendererInterface::GraphicsApi api)
{
    switch (api) {
    case QSGRendererInterface::Software:   return "software";
    case QSGRendererInterface::OpenGL:     return "OpenGL";
    case QSGRendererInterface::Direct3D11: return "Direct3D 11";
    case QSGRendererInterface::Direct3D12: return "Direct3D 12";
    case QSGRendererInterface::Vulkan:     return "Vulkan";
    case QSGRendererInterface::Metal:      return "Metal";
    case QSGRendererInterface::Null:       return "null";
    default:                               return "other";
    }
}

// Seen from above: an item is on screen only if it and everything it sits
// in are visible and not faded out (the views fade rather than hide).
bool onScreen(const QQuickItem *item)
{
    for (const QQuickItem *at = item; at; at = at->parentItem()) {
        if (!at->isVisible() || at->opacity() <= 0.0)
            return false;
    }
    return true;
}

bool isSideways(const QObject *item)
{
    return item->inherits("QQuickListView") && item->property("orientation").toInt() == Qt::Horizontal;
}

class ScrollProbe : public QObject
{
public:
    explicit ScrollProbe(QQuickWindow *window)
        : QObject(window)
        , m_window(window)
    {
        m_clock.start();
        // The render thread's frames: when each reached the screen.
        connect(window, &QQuickWindow::frameSwapped, this, [this]() {
            if (!m_recording.load())
                return;
            QMutexLocker lock(&m_mutex);
            m_swaps.push_back(m_clock.nsecsElapsed());
        }, Qt::DirectConnection);
        // The interface's thread: a timer that should come every 5 ms, so a
        // longer gap is the thread busy with something else — building
        // rows, decoding a cover, waiting on the render thread.
        m_heartbeat.setTimerType(Qt::PreciseTimer);
        m_heartbeat.setInterval(5);
        connect(&m_heartbeat, &QTimer::timeout, this, [this]() {
            const qint64 now = m_clock.nsecsElapsed();
            if (m_lastBeat > 0 && m_recording.load())
                m_stalls.push_back(double(now - m_lastBeat) / 1e6);
            m_lastBeat = now;
            if (m_recording.load() && m_page)
                m_positions.emplace_back(now, contentY());
        });
        m_heartbeat.start();

        m_steps = {
            [this]() { waitForPage(); },
            [this]() { runNotch(); },
            [this]() { runSpin(); },
            [this]() { runStream(QStringLiteral("a brisk touchpad swipe, 24 events of 40 every 8 ms"), 24, 40, 8); },
            [this]() { runStream(QStringLiteral("a slow touchpad drag, 30 events of 10 every 16 ms"), 30, 10, 16); },
            [this]() { runFling(); },
            [this]() { runThrough(); },
            [this]() { runShelf(); },
            [this]() { finish(); },
        };
        // What the page costs to build and fill counts as well: covers
        // decoded while it loads hold the thread just as they would while
        // scrolling.
        beginRun();
        next();
    }

private:
    void next()
    {
        if (m_step < m_steps.size())
            QTimer::singleShot(0, this, m_steps[m_step++]);
    }

    // After a pause, so the page is still and hover has caught up.
    void afterPause(int ms, std::function<void()> then) { QTimer::singleShot(ms, this, std::move(then)); }

    qreal contentY() const { return m_page ? m_page->property("contentY").toReal() : 0; }
    qreal endY() const
    {
        if (!m_page)
            return 0;
        return qMax<qreal>(0, m_page->property("contentHeight").toReal() - m_page->height());
    }
    void scrollTo(qreal y)
    {
        if (m_page)
            m_page->setProperty("contentY", qBound<qreal>(0, y, endY()));
    }

    // The page on screen: the visible vertical Flickable with the most to
    // scroll through.
    QQuickItem *findPage() const
    {
        QQuickItem *best = nullptr;
        qreal most = 0;
        std::function<void(QQuickItem *)> visit = [&](QQuickItem *item) {
            if (item->inherits("QQuickFlickable") && !isSideways(item) && item->width() > 300
                && item->height() > 200 && onScreen(item)) {
                const qreal range = item->property("contentHeight").toReal() - item->height();
                if (range > most) {
                    most = range;
                    best = item;
                }
            }
            const QList<QQuickItem *> children = item->childItems();
            for (QQuickItem *child : children)
                visit(child);
        };
        visit(m_window->contentItem());
        return best;
    }

    // `eighths` of a degree, as QWheelEvent counts: 120 is one notch of a
    // mouse wheel; a touchpad, or a Mac's trackpad through a Windows virtual
    // machine, sends a stream of small ones instead.
    void wheel(QQuickItem *over, int notches, int eighths = 120)
    {
        const QPointF at = over->mapToScene(QPointF(over->width() / 2, over->height() / 2));
        // Down is a negative delta, as a wheel turned towards the user gives.
        QWheelEvent event(at, m_window->mapToGlobal(at), QPoint(), QPoint(0, -eighths * notches),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false,
                          Qt::MouseEventNotSynthesized, QPointingDevice::primaryPointingDevice());
        // Stamped as the system stamps a real one: Flickable measures the
        // time between wheel events, and drops one that seems to come no
        // later than the last.
        event.setTimestamp(quint64(m_clock.elapsed()) + 1000);
        QCoreApplication::sendEvent(m_window, &event);
    }

    void beginRun()
    {
        {
            QMutexLocker lock(&m_mutex);
            m_swaps.clear();
        }
        m_positions.clear();
        m_stalls.clear();
        m_lastBeat = 0;
        m_runStart = m_clock.nsecsElapsed();
        m_recording.store(true);
    }

    // Until the page has stopped: no change for a third of a second.
    void settle(std::function<void()> then)
    {
        auto *poll = new QTimer(this);
        auto last = std::make_shared<qreal>(contentY());
        auto still = std::make_shared<QElapsedTimer>();
        auto waited = std::make_shared<QElapsedTimer>();
        still->start();
        waited->start();
        connect(poll, &QTimer::timeout, this, [this, poll, last, still, waited, then]() {
            const qreal y = contentY();
            if (!qFuzzyCompare(y + 1, *last + 1) || (m_page && m_page->property("moving").toBool())) {
                *last = y;
                still->restart();
            }
            if (still->elapsed() < 330 && waited->elapsed() < 10000)
                return;
            poll->stop();
            poll->deleteLater();
            then();
        });
        poll->start(16);
    }

    // How far the run went and how it got there.
    void report(const QString &what, qreal from, const QString &extra = QString())
    {
        m_recording.store(false);
        const qreal to = contentY();
        const qreal distance = to - from;
        qint64 reached90 = -1;
        qint64 arrived = -1;
        for (const auto &[at, y] : m_positions) {
            if (reached90 < 0 && std::abs(y - from) >= 0.9 * std::abs(distance))
                reached90 = at;
            if (arrived < 0 && std::abs(y - to) < 0.5)
                arrived = at;
        }
        const qint64 end = arrived > 0 ? arrived : m_clock.nsecsElapsed();
        std::vector<qint64> swaps;
        {
            QMutexLocker lock(&m_mutex);
            swaps = m_swaps;
        }
        std::vector<double> intervals;
        for (size_t i = 1; i < swaps.size(); ++i) {
            if (swaps[i] <= end + 20'000'000)
                intervals.push_back(double(swaps[i] - swaps[i - 1]) / 1e6);
        }
        const Spread frames = spreadOf(intervals);
        const Spread stalls = spreadOf(m_stalls);
        const auto ms = [this](qint64 at) { return at < 0 ? -1.0 : double(at - m_runStart) / 1e6; };
        qWarning("scrolltest: %s: %.0f px %s, 90%% of the way at %.0f ms, still at %.0f ms%s",
                 qPrintable(what), std::abs(distance), distance >= 0 ? "down" : "up",
                 ms(reached90), ms(arrived), qPrintable(extra));
        qWarning("scrolltest:   %s", qPrintable(framesText(frames)));
        qWarning("scrolltest:   the interface's thread was held up for %.0f ms at most (%d gaps over 50 ms)",
                 stalls.longest, stalls.over50);
    }

    // — the steps —

    void waitForPage()
    {
        auto *poll = new QTimer(this);
        auto lastHeight = std::make_shared<qreal>(-1);
        auto still = std::make_shared<QElapsedTimer>();
        auto waited = std::make_shared<QElapsedTimer>();
        still->start();
        waited->start();
        connect(poll, &QTimer::timeout, this, [this, poll, lastHeight, still, waited]() {
            m_page = findPage();
            const qreal height = m_page ? m_page->property("contentHeight").toReal() : -1;
            if (height != *lastHeight) {
                *lastHeight = height;
                still->restart();
            }
            const bool ready = m_page && endY() > 0 && still->elapsed() >= 2500;
            if (!ready && waited->elapsed() < 45000)
                return;
            poll->stop();
            poll->deleteLater();
            if (!m_page) {
                qWarning("scrolltest: no page to scroll: nothing on screen is taller than its window");
                QCoreApplication::exit(1);
                return;
            }
            m_recording.store(false);
            const Spread stalls = spreadOf(m_stalls);
            // The pointer where a person scrolling would have it, over the
            // page, so the rows it passes answer to it as they do then; or,
            // with --scroll-away, off the window, to see what that costs.
            const QPoint centre = m_window->mapToGlobal(
                m_page->mapToScene(QPointF(m_page->width() / 2, m_page->height() / 2)).toPoint());
            if (QCoreApplication::arguments().contains(QStringLiteral("--scroll-away")))
                QCursor::setPos(m_window->mapToGlobal(QPoint(-40, -40)));
            else
                QCursor::setPos(centre);
            describe();
            qWarning("scrolltest: while it loaded (%.1f s): the interface's thread was held up for %.0f ms "
                     "at most, %d times over 50 ms",
                     double(m_clock.nsecsElapsed() - m_runStart) / 1e9, stalls.longest, stalls.over50);
            qWarning("scrolltest: artwork so far: %s", qPrintable(ArtworkFetcher::decodeReport()));
            next();
        });
        poll->start(250);
    }

    void describe()
    {
        const QScreen *screen = m_window->screen();
        const QSGRendererInterface *renderer = m_window->rendererInterface();
        qWarning("scrolltest: %s, window %dx%d at %.2fx on a %.0f Hz screen; render loop %s",
                 renderer ? apiName(renderer->graphicsApi()) : "unknown", m_window->width(),
                 m_window->height(), m_window->devicePixelRatio(), screen ? screen->refreshRate() : 0.0,
                 qEnvironmentVariableIsEmpty("QSG_RENDER_LOOP") ? "default"
                                                                : qgetenv("QSG_RENDER_LOOP").constData());
        qWarning("scrolltest: page %s: %.0f px tall in a %.0f px window; flickDeceleration %.0f, "
                 "maximumFlickVelocity %.0f; the system scrolls %d lines a notch",
                 m_page->metaObject()->className(), m_page->property("contentHeight").toReal(),
                 m_page->height(), m_page->property("flickDeceleration").toReal(),
                 m_page->property("maximumFlickVelocity").toReal(),
                 QGuiApplication::styleHints()->wheelScrollLines());
        int items = 0, shown = 0, texts = 0, shapes = 0, images = 0, effects = 0;
        std::function<void(QQuickItem *)> count = [&](QQuickItem *item) {
            ++items;
            if (item->isVisible())
                ++shown;
            if (item->inherits("QQuickText"))
                ++texts;
            else if (item->inherits("QQuickShape"))
                ++shapes;
            else if (item->inherits("QQuickImage"))
                ++images;
            else if (item->inherits("QQuickMultiEffect") || item->inherits("QQuickShaderEffect"))
                ++effects;
            const QList<QQuickItem *> children = item->childItems();
            for (QQuickItem *child : children)
                count(child);
        };
        count(m_page);
        qWarning("scrolltest: the page holds %d items (%d visible): %d texts, %d shapes, %d images, %d effects",
                 items, shown, texts, shapes, images, effects);
    }

    void runNotch()
    {
        scrollTo(0);
        afterPause(800, [this]() {
            beginRun();
            wheel(m_page, 1);
            settle([this]() {
                report(QStringLiteral("one notch"), 0);
                next();
            });
        });
    }

    // Five notches 30 ms apart: the wheel turned quickly with a finger.
    void runSpin()
    {
        scrollTo(0);
        afterPause(600, [this]() {
            beginRun();
            for (int i = 0; i < 5; ++i)
                QTimer::singleShot(i * 30, this, [this]() { wheel(m_page, 1); });
            QTimer::singleShot(4 * 30 + 1, this, [this]() {
                settle([this]() {
                    report(QStringLiteral("a quick spin, five notches in 120 ms"), 0);
                    next();
                });
            });
        });
    }

    // A touchpad swipe as Windows passes it on: `count` small wheel events
    // of `eighths` each, `apart` ms apart. `notches` is what they add up to.
    void runStream(const QString &what, int count, int eighths, int apart)
    {
        scrollTo(0);
        afterPause(600, [this, what, count, eighths, apart]() {
            beginRun();
            for (int i = 0; i < count; ++i)
                QTimer::singleShot(i * apart, this, [this, eighths]() { wheel(m_page, 1, eighths); });
            QTimer::singleShot((count - 1) * apart + 1, this, [this, what, count, eighths]() {
                settle([this, what, count, eighths]() {
                    report(QStringLiteral("%1, %2 notches' worth").arg(what).arg(count * eighths / 120.0, 0, 'f', 1),
                           0);
                    next();
                });
            });
        });
    }

    // As hard as a drag can throw it.
    void runFling()
    {
        scrollTo(0);
        afterPause(600, [this]() {
            beginRun();
            const qreal top = m_page->property("maximumFlickVelocity").toReal();
            QMetaObject::invokeMethod(m_page, "flick", Q_ARG(qreal, 0), Q_ARG(qreal, -top));
            settle([this, top]() {
                report(QStringLiteral("a fling at %1 px/s").arg(top, 0, 'f', 0), 0);
                next();
            });
        });
    }

    // Top to bottom, a notch every 100 ms: reading down a long list.
    void runThrough()
    {
        scrollTo(0);
        afterPause(600, [this]() {
            beginRun();
            auto notches = std::make_shared<int>(0);
            auto *ticker = new QTimer(this);
            connect(ticker, &QTimer::timeout, this, [this, ticker, notches]() {
                if (contentY() >= endY() - 1 || *notches >= 200) {
                    ticker->stop();
                    ticker->deleteLater();
                    settle([this, notches]() {
                        report(QStringLiteral("the wheel to the end, a notch every 100 ms"), 0,
                               QStringLiteral(" (%1 notches, %2 px each)")
                                   .arg(*notches)
                                   .arg(*notches > 0 ? contentY() / *notches : 0, 0, 'f', 0));
                        next();
                    });
                    return;
                }
                ++*notches;
                wheel(m_page, 1);
            });
            ticker->start(100);
        });
    }

    // A notch with the pointer over a shelf that scrolls sideways: Home's
    // cards. The page should move, not the shelf.
    void runShelf()
    {
        QQuickItem *shelf = nullptr;
        std::function<void(QQuickItem *)> visit = [&](QQuickItem *item) {
            if (!shelf && isSideways(item) && onScreen(item) && item->width() > 200)
                shelf = item;
            const QList<QQuickItem *> children = item->childItems();
            for (QQuickItem *child : children)
                visit(child);
        };
        visit(m_page);
        if (!shelf) {
            next();
            return;
        }
        QQuickItem *content = m_page->property("contentItem").value<QQuickItem *>();
        const qreal shelfY = shelf->mapToItem(content, QPointF(0, 0)).y();
        scrollTo(shelfY - (m_page->height() - shelf->height()) / 2);
        QPointer<QQuickItem> held(shelf);
        afterPause(600, [this, held]() {
            if (!held) {
                next();
                return;
            }
            const qreal from = contentY();
            const qreal sideways = held->property("contentX").toReal();
            beginRun();
            wheel(held, 1);
            settle([this, held, from, sideways]() {
                const qreal moved = held ? held->property("contentX").toReal() - sideways : 0;
                report(QStringLiteral("a notch over a shelf of cards"), from,
                       QStringLiteral("; the shelf moved %1 px sideways").arg(moved, 0, 'f', 0));
                next();
            });
        });
    }

    void finish()
    {
        qWarning("scrolltest: artwork: %s", qPrintable(ArtworkFetcher::decodeReport()));
        qWarning("scrolltest: done, quitting");
        QCoreApplication::quit();
    }

    QQuickWindow *m_window;
    QPointer<QQuickItem> m_page;
    QElapsedTimer m_clock;
    std::atomic<bool> m_recording { false };
    QMutex m_mutex;
    std::vector<qint64> m_swaps;                        // render thread, ns
    std::vector<std::pair<qint64, qreal>> m_positions;  // interface thread: ns, contentY
    std::vector<double> m_stalls;                       // interface thread: ms between beats
    QTimer m_heartbeat;
    qint64 m_lastBeat = 0;
    qint64 m_runStart = 0;
    std::vector<std::function<void()>> m_steps;
    size_t m_step = 0;
};

} // namespace

void startScrollSelfTest(QQuickWindow *window)
{
    if (!window) {
        qWarning("scrolltest: no window");
        QCoreApplication::exit(1);
        return;
    }
    new ScrollProbe(window);
    QTimer::singleShot(180000, qApp, []() {
        qWarning("scrolltest: timed out");
        QCoreApplication::exit(2);
    });
}
