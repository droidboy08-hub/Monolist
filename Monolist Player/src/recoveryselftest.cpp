#include "recoveryselftest.h"

#include "appdatabase.h"
#include "downloadmanager.h"
#include "library.h"
#include "mpvengine.h"
#include "playbackcontroller.h"
#include "streamresolver.h"

#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QScopeGuard>
#include <QSet>
#include <QSqlQuery>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QtEndian>

#include <cmath>
#include <functional>
#include <memory>

namespace {

// One line per check, and a count at the end, as in connectionselftest.cpp.
// Descriptions stay ASCII: the console these are read in is not always UTF-8.
// The details quote the player (its "yt-dlp · muxed", its toasts' quotes),
// so they go out as UTF-8, which the message handler expects.
class Checks
{
public:
    explicit Checks(const char *prefix) : m_prefix(prefix) {}

    bool check(bool ok, const QString &what, const QString &detail = QString())
    {
        ++m_count;
        if (!ok)
            ++m_failed;
        const QString line = detail.isEmpty() ? what : what + QStringLiteral("  -- ") + detail;
        qWarning("%s: %s  %s", m_prefix, ok ? "ok  " : "FAIL", qUtf8Printable(line));
        return ok;
    }

    void note(const QString &text) { qWarning("%s: %s", m_prefix, qUtf8Printable(text)); }

    int finish()
    {
        qWarning("%s: %d checks, %d failed", m_prefix, m_count, m_failed);
        return m_failed;
    }

private:
    const char *m_prefix;
    int m_count = 0;
    int m_failed = 0;
};

// Runs the event loop until `done` holds or `timeoutMs` passes.
bool waitUntil(const std::function<bool()> &done, int timeoutMs)
{
    if (done())
        return true;
    QEventLoop loop;
    QTimer poll;
    poll.setInterval(20);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&]() {
        if (done())
            loop.quit();
    });
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    poll.start();
    loop.exec();
    return done();
}

void settle(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// The log, kept: several checks are about what the player says it did.
QStringList *g_log = nullptr;
QtMessageHandler g_previousHandler = nullptr;

void keepMessage(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (g_log)
        g_log->append(message);
    if (g_previousHandler)
        g_previousHandler(type, context, message);
}

// A tone as a WAV file: 16-bit mono PCM at 8 kHz, quiet. Plain PCM, so the
// length mpv reads is the header's, exactly, and any byte past the header is
// a place to start from.
constexpr int kToneSeconds = 30;
constexpr int kRate = 8000;

QByteArray toneWav(int seconds)
{
    constexpr double kPi = 3.14159265358979323846;
    const quint32 samples = quint32(kRate * seconds);
    const quint32 dataBytes = samples * 2;
    QByteArray wav;
    wav.reserve(int(44 + dataBytes));
    const auto u32 = [&wav](quint32 value) {
        char bytes[4];
        qToLittleEndian(value, bytes);
        wav.append(bytes, 4);
    };
    const auto u16 = [&wav](quint16 value) {
        char bytes[2];
        qToLittleEndian(value, bytes);
        wav.append(bytes, 2);
    };
    wav.append("RIFF", 4);
    u32(36 + dataBytes);
    wav.append("WAVE", 4);
    wav.append("fmt ", 4);
    u32(16);            // the format chunk's size
    u16(1);             // PCM
    u16(1);             // one channel
    u32(kRate);
    u32(kRate * 2);     // bytes a second
    u16(2);             // bytes a frame
    u16(16);            // bits a sample
    wav.append("data", 4);
    u32(dataBytes);
    for (quint32 i = 0; i < samples; ++i)
        u16(quint16(qint16(900.0 * std::sin(2.0 * kPi * 440.0 * double(i) / kRate))));
    return wav;
}

// The stand-in for googlevideo, and for the host of a row's own link. Every
// path serves the same tone, whole or from a Range. A path that starts
// "/cut" is cut the way googlevideo can cut a link: the first request for its
// first link stops at 60% of the body with the connection closed, and every
// later request for that link (the same g=) is refused with 403, which is
// what mpv's reconnect then meets. A link with another g= is a fresh one and
// served whole. A cut path with no g= at all (a row's own link, which nothing
// can refresh) is refused for kRowRefusedMs after its cut, then served. A
// link with expire=1 is a spoiled one (StreamResolver::spoilNextStream): 403.
class StandIn : public QObject
{
public:
    static constexpr qint64 kRowRefusedMs = 15000;

    struct Request {
        QString path;
        QString g;
        qint64 from = 0;
        int status = 0;
        qint64 bytes = 0;
        qint64 atMs = 0;
    };
    QList<Request> requests;

    explicit StandIn(const QByteArray &body) : m_body(body)
    {
        m_clock.start();
        QObject::connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket *socket = m_server.nextPendingConnection())
                serve(socket);
        });
    }

    bool listen() { return m_server.listen(QHostAddress::LocalHost, 0); }
    QString url(const QString &path) const
    {
        return QStringLiteral("http://127.0.0.1:%1%2").arg(m_server.serverPort()).arg(path);
    }
    qint64 size() const { return m_body.size(); }
    qint64 cutAt() const { return m_body.size() * 6 / 10; }

    QString describe(const QString &path) const
    {
        QStringList parts;
        for (const Request &r : requests) {
            if (r.path == path)
                parts << QStringLiteral("%1%2 from %3: %4 (%5 bytes)")
                             .arg(r.g.isEmpty() ? QString() : QStringLiteral("g=%1 ").arg(r.g))
                             .arg(r.atMs).arg(r.from).arg(r.status).arg(r.bytes);
        }
        return parts.isEmpty() ? QStringLiteral("no requests") : parts.join(QStringLiteral("; "));
    }

private:
    void serve(QTcpSocket *socket)
    {
        auto head = std::make_shared<QByteArray>();
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        QObject::connect(socket, &QTcpSocket::readyRead, this, [this, socket, head]() {
            head->append(socket->readAll());
            const qsizetype end = head->indexOf("\r\n\r\n");
            if (end < 0)
                return;
            QObject::disconnect(socket, &QTcpSocket::readyRead, this, nullptr);
            answer(socket, head->left(end));
        });
    }

    void answer(QTcpSocket *socket, const QByteArray &head)
    {
        const QList<QByteArray> lines = head.split('\n');
        const QUrl target(QString::fromLatin1(lines.value(0).trimmed().split(' ').value(1)));
        const QUrlQuery query(target);
        Request r;
        r.path = target.path();
        r.g = query.queryItemValue(QStringLiteral("g"));
        r.atMs = m_clock.elapsed();
        bool ranged = false;
        for (const QByteArray &line : lines) {
            const QByteArray field = line.trimmed();
            if (field.toLower().startsWith("range: bytes=")) {
                ranged = true;
                r.from = field.mid(13).split('-').value(0).toLongLong();
            }
        }

        const qint64 size = m_body.size();
        bool refuse = query.queryItemValue(QStringLiteral("expire")) == QLatin1String("1") || r.from >= size;
        qint64 stop = size;   // one past the last byte sent
        if (!refuse && r.path.startsWith(QLatin1String("/cut"))) {
            if (!r.g.isEmpty()) {
                if (!m_firstLink.contains(r.path))
                    m_firstLink.insert(r.path, r.g);
                if (m_firstLink.value(r.path) == r.g) {
                    const QString link = r.path + QLatin1Char('|') + r.g;
                    if (!m_cut.contains(link)) {
                        m_cut.insert(link);
                        stop = cutAt();
                    } else {
                        refuse = true;
                    }
                }
            } else if (!m_rowCutAt.contains(r.path)) {
                m_rowCutAt.insert(r.path, m_clock.elapsed());
                stop = cutAt();
            } else if (m_clock.elapsed() - m_rowCutAt.value(r.path) < kRowRefusedMs) {
                refuse = true;
            }
        }

        if (refuse) {
            r.status = 403;
            requests.append(r);
            socket->write("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            socket->disconnectFromHost();
            return;
        }
        r.status = ranged ? 206 : 200;
        r.bytes = qMax(r.from, stop) - r.from;
        requests.append(r);
        QByteArray reply = QByteArray("HTTP/1.1 ") + (ranged ? "206 Partial Content" : "200 OK")
                           + "\r\nContent-Type: audio/wav\r\nAccept-Ranges: bytes\r\nConnection: close\r\n"
                           + "Content-Length: " + QByteArray::number(size - r.from) + "\r\n";
        if (ranged) {
            reply += "Content-Range: bytes " + QByteArray::number(r.from) + '-' + QByteArray::number(size - 1) + '/'
                     + QByteArray::number(size) + "\r\n";
        }
        reply += "\r\n";
        reply += m_body.mid(r.from, r.bytes);
        socket->write(reply);
        // Closed once what was written has gone: for a cut request, short of
        // the length it announced.
        socket->disconnectFromHost();
    }

    QTcpServer m_server;
    QByteArray m_body;
    QElapsedTimer m_clock;
    QHash<QString, QString> m_firstLink;   // path -> the g= of its first link
    QSet<QString> m_cut;                   // "path|g" already cut once
    QHash<QString, qint64> m_rowCutAt;     // a row's own link: when it was cut
};

QVariantMap row(const QString &videoId, const QString &title, const QString &sourceUrl = QString())
{
    QVariantMap map{ { QStringLiteral("title"), title },
                     { QStringLiteral("artist"), QStringLiteral("Selftest") },
                     { QStringLiteral("durationMs"), qint64(kToneSeconds) * 1000 } };
    if (!videoId.isEmpty())
        map.insert(QStringLiteral("sourceId"), videoId);
    if (!sourceUrl.isEmpty())
        map.insert(QStringLiteral("sourceUrl"), sourceUrl);
    return map;
}

QString tierNames(const QList<int> &tiers)
{
    QStringList names;
    for (const int tier : tiers) {
        switch (tier) {
        case StreamResolver::TierInnerTube: names << QStringLiteral("InnerTube"); break;
        case StreamResolver::TierYtDlp:     names << QStringLiteral("yt-dlp"); break;
        case StreamResolver::TierMuxed:     names << QStringLiteral("muxed"); break;
        case StreamResolver::TierPiped:     names << QStringLiteral("Piped"); break;
        case StreamResolver::TierInvidious: names << QStringLiteral("Invidious"); break;
        default:                            names << QString::number(tier); break;
        }
    }
    return names.isEmpty() ? QStringLiteral("none") : names.join(QStringLiteral(", "));
}

} // namespace

int runRecoverySelfTest(Library *library)
{
    Checks t("recovery");
    const QString data = qEnvironmentVariable("MONOLIST_DATA_DIR");
    if (data.isEmpty()) {
        t.check(false, QStringLiteral("MONOLIST_DATA_DIR is set"),
                QStringLiteral("refusing to run: this test writes a downloads row and a file there"));
        return t.finish();
    }
    QTemporaryDir scratch(QDir(data).filePath(QStringLiteral("recovery-test-XXXXXX")));
    if (!t.check(scratch.isValid(), QStringLiteral("a scratch folder in the data folder"), scratch.errorString()))
        return t.finish();

    StandIn standIn(toneWav(kToneSeconds));
    if (!t.check(standIn.listen(), QStringLiteral("a stand-in server on this computer")))
        return t.finish();
    t.note(QStringLiteral("the stand-in serves a %1 s tone of %2 bytes; a cut stops at byte %3")
               .arg(kToneSeconds).arg(standIn.size()).arg(standIn.cutAt()));

    // A song downloaded, whose file is junk: its row is written before the
    // downloads are opened, so they find it. The file is in the scratch
    // folder, never the download folder.
    const QString junkId = QStringLiteral("selftestR06");
    const QString junkPath = QDir(scratch.path()).filePath(QStringLiteral("Selftest - Junk [selftestR06].opus"));
    {
        QFile junk(junkPath);
        const bool written = junk.open(QIODevice::WriteOnly) && junk.write(QByteArray(4096, 'x')) == 4096;
        junk.close();
        QSqlQuery insert(AppDatabase::connection());
        insert.prepare(QStringLiteral("INSERT OR REPLACE INTO downloads (video_id, title, artist, file_path)"
                                      " VALUES (?, ?, ?, ?)"));
        insert.addBindValue(junkId);
        insert.addBindValue(QStringLiteral("Junk download"));
        insert.addBindValue(QStringLiteral("Selftest"));
        insert.addBindValue(junkPath);
        if (!t.check(written && insert.exec(), QStringLiteral("a junk file, recorded as a download")))
            return t.finish();
    }

    QStringList log;
    g_log = &log;
    g_previousHandler = qInstallMessageHandler(keepMessage);
    const auto restoreLog = qScopeGuard([]() {
        qInstallMessageHandler(g_previousHandler);
        g_log = nullptr;
    });
    const auto logged = [&log](int from, const QString &text) {
        for (int i = from; i < log.size(); ++i) {
            if (log.at(i).contains(text))
                return i;
        }
        return -1;
    };

    MpvEngine engine;
    if (!engine.isValid()) {
        t.check(false, QStringLiteral("an audio engine to drive"), engine.lastError());
        return t.finish();
    }
    StreamResolver resolver;
    resolver.setSaavnEnabled(false);
    DownloadManager downloads;
    t.check(downloads.localPathFor(junkId) == junkPath, QStringLiteral("the downloads know the junk file"));
    PlaybackController player(&engine, &resolver, &downloads);
    player.setLibrary(library);
    // A switch set as Settings would keep it, and read as a launch reads it.
    // JioSaavn stays off whatever the scratch database says: the songs here
    // are invented, and nothing may ask it about them.
    const auto setSwitch = [library, &player, &resolver](const QString &key, const QString &value) {
        library->setSetting(key, value);
        player.restoreSettings();
        resolver.setSaavnEnabled(false);
    };
    setSwitch(QStringLiteral("playback.refused"), QString());
    setSwitch(QStringLiteral("playback.rescue_link"), QString());
    setSwitch(QStringLiteral("playback.early_end"), QString());
    player.setAutoplay(false);   // only the songs each part plays: no radio
    player.setVolume(0.35);

    // What the player and the resolver did, as it happened.
    QElapsedTimer clock;
    clock.start();
    QList<QPair<QString, int>> tiers;
    struct Answer {
        QString videoId;
        int tier;
        bool fromCache;
    };
    QList<Answer> answers;
    int audioStarts = 0;
    qint64 audioStartedAt = -1;
    QStringList notices;
    QStringList titles;
    QHash<QString, qint64> furthest;   // title -> the furthest position reached
    QObject::connect(&resolver, &StreamResolver::tierChanged, &resolver, [&tiers](const QString &id, int tier) {
        tiers.append({ id, tier });
    });
    QObject::connect(&resolver, &StreamResolver::resolved, &resolver,
                     [&answers](const QString &id, const QString &, int tier, bool fromCache) {
                         answers.append({ id, tier, fromCache });
                     });
    QObject::connect(&engine, &MpvEngine::audioStarted, &engine, [&]() {
        ++audioStarts;
        audioStartedAt = clock.elapsed();
    });
    QObject::connect(&player, &PlaybackController::notice, &player, [&notices](const QString &text) {
        notices << text;
    });
    QObject::connect(&player, &PlaybackController::currentTrackChanged, &player, [&player, &titles]() {
        const QString title = player.currentTrack().value(QStringLiteral("title")).toString();
        if (titles.isEmpty() || titles.last() != title)
            titles << title;
    });
    QObject::connect(&player, &PlaybackController::positionChanged, &player, [&player, &furthest]() {
        const QString title = player.currentTrack().value(QStringLiteral("title")).toString();
        furthest[title] = qMax(furthest.value(title), player.position());
    });

    const auto tiersOf = [&tiers](const QString &id, int from) {
        QList<int> walked;
        for (int i = from; i < tiers.size(); ++i) {
            if (tiers.at(i).first == id)
                walked << tiers.at(i).second;
        }
        return walked;
    };
    const auto title = [&player]() { return player.currentTrack().value(QStringLiteral("title")).toString(); };
    // Sound from this song, a second of it: what "it plays" means below.
    const auto playing = [&](const QString &name, int since) {
        return audioStarts > since && title() == name && player.position() >= 1000;
    };
    const auto answer = [&resolver](const QString &id, int tier, const QString &path, StandIn &server) {
        resolver.setTestAnswer(id, tier, server.url(path));
    };
    const auto failEveryTier = [&resolver](const QString &id) {
        for (const int tier : { StreamResolver::TierInnerTube, StreamResolver::TierYtDlp, StreamResolver::TierMuxed,
                                StreamResolver::TierPiped, StreamResolver::TierInvidious })
            resolver.setTestAnswer(id, tier, QString());
    };

    // — 1: a refused InnerTube link is asked for afresh, once, before the
    // muxed stream —
    {
        const QString id = QStringLiteral("selftestR01");
        answer(id, StreamResolver::TierInnerTube, QStringLiteral("/r1.wav"), standIn);
        answer(id, StreamResolver::TierMuxed, QStringLiteral("/r1-muxed.wav"), standIn);
        resolver.spoilNextStream(id, 1);
        const int fromTier = tiers.size();
        const int fromLog = log.size();
        const int starts = audioStarts;
        const qint64 began = clock.elapsed();
        player.playSource(id, QStringLiteral("Refused once"), QStringLiteral("Selftest"), QString(), 30000);
        const bool played = waitUntil([&]() { return playing(QStringLiteral("Refused once"), starts); }, 20000);
        const QList<int> walked = tiersOf(id, fromTier);
        t.check(played && walked == QList<int>{ StreamResolver::TierInnerTube, StreamResolver::TierInnerTube }
                    && player.sourceLabel() == QLatin1String("InnerTube"),
                QStringLiteral("a refused InnerTube link: InnerTube once more, not the muxed stream, and it plays"),
                QStringLiteral("rungs %1; source \"%2\"; sound %3 ms after Play")
                    .arg(tierNames(walked), player.sourceLabel()).arg(audioStartedAt - began));
        const int rescued = logged(fromLog, QStringLiteral("selftestR01 rescued: its sound came from InnerTube"));
        t.check(logged(fromLog, QStringLiteral("asking InnerTube once more for selftestR01")) >= 0 && rescued >= 0,
                QStringLiteral("the log says InnerTube was asked once more, and what rescued the song"),
                rescued >= 0 ? log.at(rescued) : QStringLiteral("no rescue line"));
        // One refusal, rescued by the fresh link, is that link's alone: not
        // remembered against the song.
        t.check(!resolver.innerTubeRefusedLately(id) && logged(fromLog, QStringLiteral("googlevideo refused its")) < 0,
                QStringLiteral("a refusal the fresh link rescued is not held against the song"));
    }

    // — 1b: the switch back, playback.refused=muxed —
    {
        setSwitch(QStringLiteral("playback.refused"), QStringLiteral("muxed"));
        const QString id = QStringLiteral("selftestR1b");
        answer(id, StreamResolver::TierInnerTube, QStringLiteral("/r1b.wav"), standIn);
        answer(id, StreamResolver::TierMuxed, QStringLiteral("/r1b-muxed.wav"), standIn);
        resolver.spoilNextStream(id, 1);
        const int fromTier = tiers.size();
        const int starts = audioStarts;
        player.playSource(id, QStringLiteral("Refused, muxed first"), QStringLiteral("Selftest"), QString(), 30000);
        const bool played = waitUntil([&]() { return playing(QStringLiteral("Refused, muxed first"), starts); }, 20000);
        const QList<int> walked = tiersOf(id, fromTier);
        t.check(played && walked == QList<int>{ StreamResolver::TierInnerTube, StreamResolver::TierMuxed },
                QStringLiteral("playback.refused=muxed: the muxed stream straight after the refusal, as before"),
                QStringLiteral("rungs %1; source \"%2\"").arg(tierNames(walked), player.sourceLabel()));
        setSwitch(QStringLiteral("playback.refused"), QString());
    }

    // — 2: both InnerTube links refused: the muxed stream rescues this play
    // and no other —
    {
        const QString id = QStringLiteral("selftestR02");
        const QString name = QStringLiteral("Rescued");
        answer(id, StreamResolver::TierInnerTube, QStringLiteral("/r2.wav"), standIn);
        answer(id, StreamResolver::TierMuxed, QStringLiteral("/r2-muxed.wav"), standIn);
        resolver.spoilNextStream(id, 2);
        int fromTier = tiers.size();
        int starts = audioStarts;
        player.playSource(id, name, QStringLiteral("Selftest"), QString(), 30000);
        bool played = waitUntil([&]() { return playing(name, starts); }, 20000);
        QList<int> walked = tiersOf(id, fromTier);
        t.check(played && walked == QList<int>{ StreamResolver::TierInnerTube, StreamResolver::TierInnerTube,
                                                StreamResolver::TierMuxed }
                    && player.sourceLabel() == QStringLiteral("yt-dlp · muxed"),
                QStringLiteral("both InnerTube links refused: the muxed stream rescues the play"),
                QStringLiteral("rungs %1; source \"%2\"").arg(tierNames(walked), player.sourceLabel()));

        // The same play going back to it (as the sound does after its
        // picture): at once, from the rescue link.
        int fromAnswer = answers.size();
        fromTier = tiers.size();
        resolver.resolveVia(id, { StreamResolver::TierMuxed, StreamResolver::TierYtDlp },
                            StreamResolver::TierInnerTube);
        waitUntil([&]() { return answers.size() > fromAnswer; }, 5000);
        t.check(answers.size() == fromAnswer + 1 && answers.last().tier == StreamResolver::TierMuxed
                    && answers.last().fromCache && tiersOf(id, fromTier).isEmpty(),
                QStringLiteral("the same play goes back to its rescue link at once, asking no one"));

        const auto answeredFrom = [&](int from) {
            return QStringLiteral("answered from %1%2; source \"%3\"; rungs asked %4")
                .arg(answers.size() > from ? tierNames({ answers.at(from).tier }) : QStringLiteral("nothing"))
                .arg(answers.size() > from && answers.at(from).fromCache ? QStringLiteral(", remembered") : QString())
                .arg(player.sourceLabel(), tierNames(tiersOf(id, fromTier)));
        };

        // Played again within the hour: both its InnerTube links were
        // refused, which is the song's refusal and not one link's, so it
        // starts from the link that rescued it, at once, asking no one.
        fromAnswer = answers.size();
        fromTier = tiers.size();
        starts = audioStarts;
        player.playSource(id, name, QStringLiteral("Selftest"), QString(), 30000);
        played = waitUntil([&]() { return playing(name, starts); }, 20000);
        t.check(played && answers.size() > fromAnswer && answers.at(fromAnswer).tier == StreamResolver::TierMuxed
                    && answers.at(fromAnswer).fromCache && tiersOf(id, fromTier).isEmpty()
                    && player.sourceLabel() == QStringLiteral("yt-dlp · muxed"),
                QStringLiteral("played again within the hour, it starts from the link that rescued it, asking no one "
                               "(InnerTube refused both its links)"),
                answeredFrom(fromAnswer));

        // Once the hour is over: from InnerTube again, Opus again.
        resolver.forgetInnerTubeRefusals();
        fromAnswer = answers.size();
        fromTier = tiers.size();
        starts = audioStarts;
        player.playSource(id, name, QStringLiteral("Selftest"), QString(), 30000);
        played = waitUntil([&]() { return playing(name, starts); }, 20000);
        const bool fromTop = answers.size() > fromAnswer && answers.at(fromAnswer).tier == StreamResolver::TierInnerTube;
        t.check(played && fromTop && player.sourceLabel() == QLatin1String("InnerTube"),
                QStringLiteral("after the hour, played again, it is InnerTube's link again, not the rescue link"),
                answeredFrom(fromAnswer));
    }

    // — 2c: a song whose InnerTube links were refused lately, refused again
    // (its kept link gone): straight to the muxed stream, InnerTube not
    // asked a second time —
    {
        const QString id = QStringLiteral("selftestR2c");
        const QString name = QStringLiteral("Refused lately");
        answer(id, StreamResolver::TierInnerTube, QStringLiteral("/r2c.wav"), standIn);
        answer(id, StreamResolver::TierMuxed, QStringLiteral("/r2c-muxed.wav"), standIn);
        resolver.noteInnerTubeRefused(id);
        resolver.spoilNextStream(id, 1);
        const int fromTier = tiers.size();
        const int fromLog = log.size();
        const int starts = audioStarts;
        player.playSource(id, name, QStringLiteral("Selftest"), QString(), 30000);
        const bool played = waitUntil([&]() { return playing(name, starts); }, 20000);
        const QList<int> walked = tiersOf(id, fromTier);
        t.check(played && walked == QList<int>{ StreamResolver::TierInnerTube, StreamResolver::TierMuxed }
                    && logged(fromLog, QStringLiteral("selftestR2c's InnerTube links were refused lately")) >= 0,
                QStringLiteral("refused lately, and its InnerTube link refused again: the muxed stream next, "
                               "no fresh InnerTube link"),
                QStringLiteral("rungs %1; source \"%2\"").arg(tierNames(walked), player.sourceLabel()));
        resolver.forgetInnerTubeRefusals();
    }

    // — 2b: the switch back, playback.rescue_link=keep —
    {
        setSwitch(QStringLiteral("playback.rescue_link"), QStringLiteral("keep"));
        const QString id = QStringLiteral("selftestR2b");
        const QString name = QStringLiteral("Rescued, kept");
        answer(id, StreamResolver::TierInnerTube, QStringLiteral("/r2b.wav"), standIn);
        answer(id, StreamResolver::TierMuxed, QStringLiteral("/r2b-muxed.wav"), standIn);
        resolver.spoilNextStream(id, 2);
        int starts = audioStarts;
        player.playSource(id, name, QStringLiteral("Selftest"), QString(), 30000);
        waitUntil([&]() { return playing(name, starts); }, 20000);
        const int fromAnswer = answers.size();
        starts = audioStarts;
        player.playSource(id, name, QStringLiteral("Selftest"), QString(), 30000);
        const bool played = waitUntil([&]() { return playing(name, starts); }, 20000);
        t.check(played && answers.size() > fromAnswer && answers.at(fromAnswer).tier == StreamResolver::TierMuxed
                    && answers.at(fromAnswer).fromCache,
                QStringLiteral("playback.rescue_link=keep: played again, it is the kept muxed link, as before"),
                QStringLiteral("source \"%1\"").arg(player.sourceLabel()));
        setSwitch(QStringLiteral("playback.rescue_link"), QString());
    }

    // — 3: a stream cut short at 60%, its reconnect refused —
    {
        const QString id = QStringLiteral("selftestR03");
        const QString name = QStringLiteral("Cut short");
        const QString after = QStringLiteral("After the cut");
        answer(id, StreamResolver::TierInnerTube, QStringLiteral("/cut3.wav"), standIn);
        answer(QStringLiteral("selftestR3n"), StreamResolver::TierInnerTube, QStringLiteral("/r3n.wav"), standIn);
        const int fromLog = log.size();
        const int fromTitles = titles.size();
        player.playTracks({ row(id, name), row(QStringLiteral("selftestR3n"), after) }, 0);
        // Its end, and the next song's start.
        const bool moved = waitUntil([&]() { return title() == after && player.position() >= 500; }, 75000);
        const int picked = logged(fromLog, QStringLiteral("selftestR03 ended at"));
        t.check(picked >= 0, QStringLiteral("the cut is seen as the stream stopping short"),
                picked >= 0 ? log.at(picked) : QStringLiteral("no such line"));
        t.check(moved && furthest.value(name) >= (kToneSeconds - 5) * 1000,
                QStringLiteral("it carries on from where it stopped, and moves on only at its end"),
                QStringLiteral("furthest %1 ms of %2; the stand-in saw: %3")
                    .arg(furthest.value(name)).arg(kToneSeconds * 1000).arg(standIn.describe(QStringLiteral("/cut3.wav"))));
        t.check(titles.mid(fromTitles) == QStringList{ name, after },
                QStringLiteral("and nothing else played in between"), titles.mid(fromTitles).join(QStringLiteral(" / ")));
    }

    // — 3b: the same on a row's own link, which is loaded again —
    {
        const QString name = QStringLiteral("Cut row");
        const QString after = QStringLiteral("After the cut row");
        answer(QStringLiteral("selftestR3r"), StreamResolver::TierInnerTube, QStringLiteral("/r3r.wav"), standIn);
        const int fromLog = log.size();
        player.playTracks({ row(QString(), name, standIn.url(QStringLiteral("/cutrow.wav"))),
                            row(QStringLiteral("selftestR3r"), after) }, 0);
        const bool moved = waitUntil([&]() { return title() == after && player.position() >= 500; }, 75000);
        const int picked = logged(fromLog, QStringLiteral("Cut row ended at"));
        t.check(picked >= 0 && moved && furthest.value(name) >= (kToneSeconds - 5) * 1000,
                QStringLiteral("a row's own link cut short: loaded again from where it stopped, played to its end"),
                QStringLiteral("furthest %1 ms; %2; the stand-in saw: %3")
                    .arg(furthest.value(name))
                    .arg(picked >= 0 ? log.at(picked) : QStringLiteral("no early end seen"))
                    .arg(standIn.describe(QStringLiteral("/cutrow.wav"))));
    }

    // — 3c: the switch back, playback.early_end=next —
    {
        setSwitch(QStringLiteral("playback.early_end"), QStringLiteral("next"));
        const QString name = QStringLiteral("Cut, and let go");
        const QString after = QStringLiteral("After letting go");
        answer(QStringLiteral("selftestR3c"), StreamResolver::TierInnerTube, QStringLiteral("/cut3c.wav"), standIn);
        answer(QStringLiteral("selftestR3d"), StreamResolver::TierInnerTube, QStringLiteral("/r3d.wav"), standIn);
        player.playTracks({ row(QStringLiteral("selftestR3c"), name), row(QStringLiteral("selftestR3d"), after) }, 0);
        const bool moved = waitUntil([&]() { return title() == after; }, 75000);
        t.check(moved && furthest.value(name) < (kToneSeconds - 5) * 1000,
                QStringLiteral("playback.early_end=next: the cut ends the song and the next one plays, as before"),
                QStringLiteral("furthest %1 ms").arg(furthest.value(name)));
        setSwitch(QStringLiteral("playback.early_end"), QString());
    }

    // — 4: a downloaded file mpv will not open —
    {
        const QString name = QStringLiteral("Junk download");
        answer(junkId, StreamResolver::TierInnerTube, QStringLiteral("/r6.wav"), standIn);
        const int fromLog = log.size();
        const int starts = audioStarts;
        player.playSource(junkId, name, QStringLiteral("Selftest"), QString(), 30000);
        const bool played = waitUntil([&]() { return playing(name, starts); }, 20000);
        t.check(played && logged(fromLog, QStringLiteral("streaming selftestR06 instead of its file")) >= 0
                    && player.sourceLabel() == QLatin1String("InnerTube"),
                QStringLiteral("a downloaded file mpv refuses: the song streams instead, and plays"),
                QStringLiteral("source \"%1\", status \"%2\"").arg(player.sourceLabel(), player.statusText()));
    }

    // — 5: a file with nothing to stream it from —
    {
        const QString name = QStringLiteral("Junk file");
        const QString after = QStringLiteral("After the junk file");
        answer(QStringLiteral("selftestR07"), StreamResolver::TierInnerTube, QStringLiteral("/r7.wav"), standIn);
        const int fromNotices = notices.size();
        const int starts = audioStarts;
        player.playTracks({ row(QString(), name, junkPath), row(QStringLiteral("selftestR07"), after) }, 0);
        const bool played = waitUntil([&]() { return playing(after, starts); }, 20000);
        t.check(played && notices.mid(fromNotices).join(QLatin1Char('|')).contains(QLatin1String("skipping")),
                QStringLiteral("a file mpv refuses, with no source id to stream: passed over, and the next plays"),
                notices.mid(fromNotices).join(QStringLiteral(" / ")));
    }

    // — 6: three in a row that will not play, and the count starting again
    // only once a song's sound has —
    {
        const QStringList failing{ QStringLiteral("selftestF01"), QStringLiteral("selftestF02"),
                                   QStringLiteral("selftestF03"), QStringLiteral("selftestF04"),
                                   QStringLiteral("selftestF05") };
        for (const QString &id : failing)
            failEveryTier(id);
        const QString good = QStringLiteral("selftestG01");
        answer(good, StreamResolver::TierInnerTube, QStringLiteral("/g1.wav"), standIn);
        answer(good, StreamResolver::TierMuxed, QStringLiteral("/g1-muxed.wav"), standIn);
        answer(QStringLiteral("selftestG02"), StreamResolver::TierInnerTube, QStringLiteral("/g2.wav"), standIn);
        // Refused first, so its first link arrives, and is refused, before
        // any sound: the count must still be 2 when its sound starts.
        resolver.spoilNextStream(good, 1);
        const int fromLog = log.size();
        const int fromTitles = titles.size();
        const int starts = audioStarts;
        player.playTracks({ row(failing.at(0), QStringLiteral("Fails 1")), row(failing.at(1), QStringLiteral("Fails 2")),
                            row(good, QStringLiteral("Plays")), row(failing.at(2), QStringLiteral("Fails 3")),
                            row(failing.at(3), QStringLiteral("Fails 4")), row(failing.at(4), QStringLiteral("Fails 5")),
                            row(QStringLiteral("selftestG02"), QStringLiteral("Never reached")) }, 0);
        const bool played = waitUntil([&]() { return playing(QStringLiteral("Plays"), starts); }, 20000);
        const int reset = logged(fromLog, QStringLiteral("selftestG01 has started, so the count"));
        t.check(played && reset >= 0 && log.at(reset).contains(QLatin1String("(it was 2)")),
                QStringLiteral("two that would not play, then one that does: the count goes back only when its sound"
                               " starts, still 2 after its link came and was refused"),
                reset >= 0 ? log.at(reset) : QStringLiteral("no reset line"));
        player.next();
        const bool stopped = waitUntil([&]() { return player.statusError(); }, 20000);
        settle(500);
        t.check(stopped && title() == QLatin1String("Fails 5")
                    && player.statusText() == QLatin1String("Nothing here will play")
                    && !titles.mid(fromTitles).contains(QStringLiteral("Never reached")),
                QStringLiteral("then three in a row that will not play: the queue stops at the third"),
                QStringLiteral("on \"%1\", status \"%2\"; played: %3")
                    .arg(title(), player.statusText(), titles.mid(fromTitles).join(QStringLiteral(" / "))));
    }

    engine.stop();
    resolver.clearTestAnswers();
    QSqlQuery remove(AppDatabase::connection());
    remove.prepare(QStringLiteral("DELETE FROM downloads WHERE video_id = ?"));
    remove.addBindValue(junkId);
    remove.exec();
    return t.finish();
}
