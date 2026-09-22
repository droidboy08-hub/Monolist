#include "mediaextractor.h"

#include <QTimer>

MediaExtractor::MediaExtractor(QObject *parent)
    : QObject(parent) {}

void MediaExtractor::setBusy(bool busy)
{
    if (busy == m_busy)
        return;
    m_busy = busy;
    Q_EMIT busyChanged();
}

void MediaExtractor::search(const QString &query)
{
    // Stub: returns the query echoed as a single mock result after a short delay.
    setBusy(true);
    QTimer::singleShot(250, this, [this, query]() {
        QVariantList results;
        if (!query.trimmed().isEmpty()) {
            results.append(QVariantMap{
                { QStringLiteral("title"), query },
                { QStringLiteral("artist"), QStringLiteral("Unresolved source") },
                { QStringLiteral("album"), QString() },
                { QStringLiteral("durationText"), QStringLiteral("0:00") }
            });
        }
        setBusy(false);
        Q_EMIT searchFinished(results);
    });
}

void MediaExtractor::resolve(const QString &url)
{
    setBusy(true);
    QTimer::singleShot(150, this, [this, url]() {
        setBusy(false);
        Q_EMIT resolved(QVariantMap{
            { QStringLiteral("url"), url },
            { QStringLiteral("container"), QStringLiteral("unknown") },
            { QStringLiteral("stub"), true }
        });
    });
}

void MediaExtractor::cancel()
{
    setBusy(false);
}
