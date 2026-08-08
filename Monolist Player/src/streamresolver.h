#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QStringList>

class QNetworkAccessManager;
class QNetworkReply;
class YtDlpRequest;

// Turns a video id into a playable audio URL, trying progressively less
// reliable sources until one answers.
//
// This is the Melody streaming engine carried over from the React app, with one
// structural change: Melody raced Piped and Invidious because it had no better
// option — a browser cannot run yt-dlp, and every request had to clear CORS via
// a public proxy. A native app has neither limit, so yt-dlp becomes tier 0 and
// the instance racing is demoted to a fallback for when yt-dlp is missing,
// rate-limited, or broken by an upstream change.
//
// The racing itself is worth keeping: public instances fail constantly and
// unpredictably, and "ask twelve, take the first answer" degrades far better
// than any single endpoint. It is the C++ equivalent of Melody's Promise.any.
class StreamResolver : public QObject
{
    Q_OBJECT
public:
    enum Tier {
        TierYtDlp = 0,
        TierPiped,
        TierInvidious,
        TierExhausted
    };
    Q_ENUM(Tier)

    explicit StreamResolver(QObject *parent = nullptr);
    ~StreamResolver() override;

    void resolve(const QString &videoId);
    void cancel(const QString &videoId);
    void cancelAll();

    // Instance lists rot — hosts disappear every few months. They are settable
    // so a config update can fix playback without shipping a new binary.
    void setPipedInstances(const QStringList &hosts);
    void setInvidiousInstances(const QStringList &hosts);
    QStringList pipedInstances() const { return m_piped; }
    QStringList invidiousInstances() const { return m_invidious; }

    static QStringList defaultPipedInstances();
    static QStringList defaultInvidiousInstances();

Q_SIGNALS:
    void resolved(const QString &videoId, const QString &url, int tier);
    void failed(const QString &videoId, const QString &reason);
    void tierChanged(const QString &videoId, int tier);

private:
    struct Job {
        QString videoId;
        int tier = TierYtDlp;
        // Bumped on every tier change. Callbacks from an abandoned tier still
        // arrive — aborting a reply fires its finished() handler — and are
        // ignored by comparing against the generation they were created in.
        int generation = 0;
        QPointer<YtDlpRequest> ytdlp;
        QList<QPointer<QNetworkReply>> pending;
        QStringList errors;
        bool settled = false;
    };

    void startTier(Job *job, int tier);
    void startYtDlp(Job *job);
    void startPipedRace(Job *job);
    void startInvidiousRace(Job *job);

    void succeed(Job *job, const QString &url);
    void tierExhausted(Job *job, const QString &reason);
    void abortPending(Job *job);
    void discard(Job *job);

    QNetworkAccessManager *m_network;
    QHash<QString, Job *> m_jobs;
    QStringList m_piped;
    QStringList m_invidious;
};
