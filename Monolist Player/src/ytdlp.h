#pragma once

#include <QJsonDocument>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

// One yt-dlp invocation. Owns its QProcess and reports back through signals;
// deleteLater()s itself once a terminal signal has been emitted, so callers may
// fire and forget, or hold the pointer to cancel().
class YtDlpRequest : public QObject
{
    Q_OBJECT
public:
    ~YtDlpRequest() override;

    void cancel();
    bool isRunning() const;

    // Emits failed() once and schedules deletion. Public so the factory can
    // report a missing binary without depending on friend access from a lambda.
    void settleFailed(const QString &reason);

Q_SIGNALS:
    void succeededJson(const QJsonDocument &document);
    void progress(qint64 received, qint64 total);   // total < 0 when unknown
    void finishedFile(const QString &path);
    void failed(const QString &reason);

private:
    friend class YtDlp;
    explicit YtDlpRequest(QObject *parent = nullptr);

    void start(const QString &program, const QStringList &arguments, bool expectJson);
    void handleStdout();
    void handleFinished(int exitCode, QProcess::ExitStatus status);

    QProcess *m_process = nullptr;
    QByteArray m_stdout;
    QByteArray m_stderr;
    QString m_destinationPath;
    bool m_expectJson = true;
    bool m_settled = false;
};

// Locates and drives the yt-dlp binary.
//
// yt-dlp is the primary extraction path because it tracks YouTube's signature
// and throttling changes far faster than any hand-written scraper. The HTML
// scraping and public-instance racing the previous player relied on live on in
// StreamResolver as a fallback tier, not as the first choice.
class YtDlp
{
public:
    // Search order: explicit override, PATH, the directory next to the running
    // executable (for a bundled copy), then `python -m yt_dlp`.
    static bool isAvailable();
    static QString resolvedDescription();
    static void setExecutableOverride(const QString &path);

    // `ytsearchN:` query against YouTube; flat, so it stays fast.
    static YtDlpRequest *search(const QString &query, int limit, QObject *parent);

    // Full metadata for one video, including a direct bestaudio URL.
    static YtDlpRequest *resolveAudio(const QString &videoIdOrUrl, QObject *parent);

    // Downloads bestaudio to `destinationDir`, remuxing to m4a via ffmpeg when
    // available. Emits progress() while running and finishedFile() at the end.
    static YtDlpRequest *download(const QString &videoIdOrUrl,
                                  const QString &destinationDir,
                                  const QString &fileStem,
                                  QObject *parent);

    static QString normaliseToUrl(const QString &videoIdOrUrl);

private:
    struct Invocation {
        QString program;
        QStringList prefixArgs;
        bool valid = false;
    };
    static Invocation locate();
    static YtDlpRequest *run(const QStringList &args, bool expectJson, QObject *parent);
};
