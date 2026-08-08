#pragma once

#include <QObject>
#include <QVariantMap>

// Placeholder for stream resolution.
//
// Intended production wiring:
//   * yt-dlp (QProcess, JSON on stdout) for stream URLs and downloads
//   * NewPipeExtractor for metadata and search where a lighter path is enough
//
// The UI only depends on the signals below, so swapping the implementation
// does not touch QML.
class MediaExtractor : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
public:
    explicit MediaExtractor(QObject *parent = nullptr);

    bool busy() const { return m_busy; }

public Q_SLOTS:
    void search(const QString &query);
    void resolve(const QString &url);
    void cancel();

Q_SIGNALS:
    void busyChanged();
    void searchFinished(const QVariantList &results);
    void resolved(const QVariantMap &stream);
    void failed(const QString &reason);

private:
    void setBusy(bool busy);
    bool m_busy = false;
};
