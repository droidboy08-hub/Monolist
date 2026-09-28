#pragma once

#include <QJsonDocument>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <functional>

// How a download is written to disk.
struct DownloadOptions
{
    enum class Format {
        Original,   // the best stream as published: unwrapped, never re-encoded
        M4a,        // AAC in .m4a, YouTube's own AAC stream when there is one
        Mp3         // re-encoded, for players and car stereos that take nothing else
    };

    Format format = Format::Original;
    bool embedMetadata = true;   // title, artist, album, date
    bool embedArtwork = true;    // the thumbnail, cropped square, as cover art
    bool skipNonMusic = true;    // cut SponsorBlock's "music_offtopic" segments
};

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
    // Download progress. total is < 0 when unknown; speed (bytes/s) and eta
    // (seconds) are < 0 until yt-dlp can estimate them.
    void progress(qint64 received, qint64 total, double speed, int etaSeconds);
    // A post-processing step has started: "ExtractAudio", "EmbedThumbnail", ...
    void postProcessing(const QString &step);
    // The final path, and what yt-dlp knows about the track: title, artist,
    // uploader, channel, album, duration (seconds), thumbnail.
    void finishedFile(const QString &path, const QVariantMap &metadata);
    void failed(const QString &reason);

private:
    friend class YtDlp;
    explicit YtDlpRequest(QObject *parent = nullptr);

    void start(const QString &program, const QStringList &arguments, bool expectJson);
    void handleStdout();
    void handleStderr();
    void handleLine(const QString &line);
    void handleFinished(int exitCode, QProcess::ExitStatus status);
    // Ends yt-dlp and every process it started. A download waits until they
    // have gone, since its files are deleted next; a lookup (JSON) holds no
    // files, and returns at once while they end on their own.
    void stopProcess();

    QProcess *m_process = nullptr;
    // Windows: the job yt-dlp and its children run in (a HANDLE), or null.
    void *m_job = nullptr;
    QByteArray m_stdout;
    QByteArray m_stderr;
    QByteArray m_stderrLine;
    QString m_destinationPath;
    QVariantMap m_metadata;
    bool m_expectJson = true;
    bool m_settled = false;
};

// Locates and drives the yt-dlp binary and the tools it depends on.
//
// yt-dlp is the primary extraction path because it tracks YouTube's signature
// and throttling changes far faster than any hand-written scraper. The HTML
// scraping and public-instance racing the previous player relied on live on in
// StreamResolver as a fallback tier, not as the first choice.
//
// Two companions matter:
//   FFmpeg  extracts the audio track, writes tags and embeds cover art. Without
//           it a download is saved as the raw stream, untagged.
//   Deno    runs YouTube's player challenges. Without a JavaScript runtime
//           yt-dlp loses most formats, the high-quality audio ones included.
//
// All three are looked for in MONOLIST_TOOLS_DIR, <app>/tools, <app>/tools/yt-dlp
// (the unpacked yt-dlp), next to the executable, and finally on PATH. Bundled
// copies win, so a packaged app runs the versions it shipped with. On macOS
// <app> is Monolist.app/Contents/MacOS, where FFmpeg is, and yt-dlp and Deno
// come first from where the app unpacks and updates them (ToolStore).
class YtDlp
{
public:
    static bool isAvailable();
    static QString resolvedDescription();
    static void setExecutableOverride(const QString &path);
    // The switch back for cancelling a lookup (a stream, a search): true
    // waits, on the calling thread, until its processes have gone, as every
    // cancel once did. Downloads always wait. Set once at start, from the
    // setting ytdlp.cancel ("wait"); off by default.
    static void setCancelWaits(bool wait);

    // Playback first. A resolver says so while a song someone is waiting to
    // hear is being resolved (StreamResolver; `who` is the resolver, so any
    // number can), and no new download starts its yt-dlp until none is: two
    // extractions at once each take several times as long under emulation
    // or on a small machine. Downloads already running carry on. `then`
    // runs on `context`'s thread once none is resolving, queued, and at
    // once (still queued) when none is.
    static void setPlaybackResolving(const void *who, bool resolving);
    static bool playbackResolving();
    static void whenPlaybackResolved(QObject *context, std::function<void()> then);

    // Adds the places a package manager puts these tools to PATH, where the
    // platform does not already. Call once at startup, before anything looks.
    //
    // macOS: an app opened from Finder or the Dock inherits launchd's PATH,
    // /usr/bin:/bin:/usr/sbin:/sbin, not the shell's, so Homebrew's tools —
    // which is where yt-dlp, FFmpeg and Deno are on a Mac — would be missing
    // even though Terminal finds them.
    static void extendSearchPath();

    // How to install yt-dlp on this platform, for the notice that says it is
    // missing.
    static QString installHint();

    // Full paths, or empty when the tool is missing.
    static QString ffmpegPath();
    static QString denoPath();
    // The command that runs yt-dlp: the program first, then any arguments that
    // have to precede its own (a pip install needs "-m yt_dlp"). Empty when it
    // is not installed. For asking it its version, which is worth showing
    // because a yt-dlp six months old is the usual reason a track will not
    // play.
    static QStringList invocationCommand();

    // `ytsearchN:` query against YouTube; flat, so it stays fast.
    static YtDlpRequest *search(const QString &query, int limit, QObject *parent);

    // Full metadata for one video, including a direct bestaudio URL.
    static YtDlpRequest *resolveAudio(const QString &videoIdOrUrl, QObject *parent);

    // The muxed stream alone — itag 18, the sound with a small picture in one
    // file, or failing that the best other stream that carries both — for a
    // track whose sound-only streams the player would not play.
    static YtDlpRequest *resolveMuxed(const QString &videoIdOrUrl, QObject *parent);

    // The picture as well as the sound, capped so a music video does not
    // arrive as 4K. One stream where YouTube still muxes both (up to 720p),
    // otherwise two, which mpv plays together.
    static YtDlpRequest *resolveVideo(const QString &videoIdOrUrl, int maxHeight,
                                      QObject *parent = nullptr);

    // Downloads one track into `destinationDir` as `<fileStem>.<ext>`, or, with
    // an empty stem, as "<artist> - <title> [<id>].<ext>" from the fetched
    // metadata. Emits progress() and postProcessing() while running and
    // finishedFile() with the final path at the end.
    static YtDlpRequest *download(const QString &videoIdOrUrl,
                                  const QString &destinationDir,
                                  const QString &fileStem,
                                  const DownloadOptions &options,
                                  QObject *parent);

    static QString normaliseToUrl(const QString &videoIdOrUrl);

    // Channel names are not artist names: "Adele - Topic" and "AdeleVEVO" are
    // both Adele.
    static QString cleanArtist(const QString &channel);

private:
    struct Invocation {
        QString program;
        QStringList prefixArgs;
        bool valid = false;
    };
    static QStringList toolDirectories();
    static QString findTool(const QString &name);
    static Invocation locate();
    static QStringList commonArguments();
    static YtDlpRequest *run(const QStringList &args, bool expectJson, QObject *parent);
};
