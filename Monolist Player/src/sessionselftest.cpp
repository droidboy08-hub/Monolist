#include "sessionselftest.h"

#include "downloadmanager.h"
#include "library.h"
#include "mpvengine.h"
#include "playbackcontroller.h"
#include "queuemodel.h"
#include "streamresolver.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QtEndian>

#include <cmath>
#include <functional>
#include <memory>

namespace {

// One line per check, and a count at the end, as in recoveryselftest.cpp.
class Checks
{
public:
    bool check(bool ok, const QString &what, const QString &detail = QString())
    {
        ++m_count;
        if (!ok)
            ++m_failed;
        const QString line = detail.isEmpty() ? what : what + QStringLiteral("  -- ") + detail;
        qWarning("session: %s  %s", ok ? "ok  " : "FAIL", qUtf8Printable(line));
        return ok;
    }

    void note(const QString &text) { qWarning("session: %s", qUtf8Printable(text)); }

    int finish()
    {
        qWarning("session: %d checks, %d failed", m_count, m_failed);
        return m_failed;
    }

private:
    int m_count = 0;
    int m_failed = 0;
};

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

void pause(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// A quiet tone as a WAV file: 16-bit mono PCM at 8 kHz.
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
    u32(16);
    u16(1);
    u16(1);
    u32(kRate);
    u32(kRate * 2);
    u16(2);
    u16(16);
    wav.append("data", 4);
    u32(dataBytes);
    for (quint32 i = 0; i < samples; ++i)
        u16(quint16(qint16(700.0 * std::sin(2.0 * kPi * 440.0 * double(i) / kRate))));
    return wav;
}

// The stand-in for googlevideo: every path serves the tone, whole or from a
// Range.
class StandIn : public QObject
{
public:
    explicit StandIn(const QByteArray &body) : m_body(body)
    {
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
        qint64 from = 0;
        bool ranged = false;
        for (const QByteArray &line : head.split('\n')) {
            const QByteArray field = line.trimmed();
            if (field.toLower().startsWith("range: bytes=")) {
                ranged = true;
                from = field.mid(13).split('-').value(0).toLongLong();
            }
        }
        const qint64 size = m_body.size();
        if (from >= size) {
            socket->write("HTTP/1.1 416 Range Not Satisfiable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            socket->disconnectFromHost();
            return;
        }
        QByteArray reply = QByteArray("HTTP/1.1 ") + (ranged ? "206 Partial Content" : "200 OK")
                           + "\r\nContent-Type: audio/wav\r\nAccept-Ranges: bytes\r\nConnection: close\r\n"
                           + "Content-Length: " + QByteArray::number(size - from) + "\r\n";
        if (ranged) {
            reply += "Content-Range: bytes " + QByteArray::number(from) + '-' + QByteArray::number(size - 1) + '/'
                     + QByteArray::number(size) + "\r\n";
        }
        reply += "\r\n";
        reply += m_body.mid(from);
        socket->write(reply);
        socket->disconnectFromHost();
    }

    QTcpServer m_server;
    QByteArray m_body;
};

QueueTrack queued(const QString &id, bool fromRadio = false)
{
    QueueTrack track;
    track.videoId = id;
    track.title = QStringLiteral("Song ") + id;
    track.artist = QStringLiteral("Selftest");
    track.durationMs = 200000;
    track.fromRadio = fromRadio;
    return track;
}

QString idsOf(const QueueModel &queue, int from = 0)
{
    QStringList ids;
    for (int row = from; row < queue.rowCount(); ++row)
        ids << queue.at(row)->videoId;
    return ids.join(QLatin1Char(' '));
}

// Through JSON, as the settings table keeps it.
QVariantMap throughJson(const QVariantMap &map)
{
    const QByteArray json = QJsonDocument(QJsonObject::fromVariantMap(map)).toJson(QJsonDocument::Compact);
    return QJsonDocument::fromJson(json).object().toVariantMap();
}

QVariantMap setting(Library *library, const QString &key)
{
    return QJsonDocument::fromJson(library->settingValue(key).toUtf8()).object().toVariantMap();
}

void testSnapshot(Checks &t)
{
    t.note(QStringLiteral("- the queue's snapshot"));

    QueueModel queue;
    QueueTrack credited = queued(QStringLiteral("c"));
    credited.album = QStringLiteral("An album");
    credited.albumId = QStringLiteral("MPREb_selftest");
    credited.artwork = QStringLiteral("https://example.invalid/c.jpg");
    credited.primaryArtist = QStringLiteral("First");
    credited.isVideo = true;
    credited.trackId = 42;
    credited.credits = { QVariantMap{ { QStringLiteral("name"), QStringLiteral("First") },
                                      { QStringLiteral("browseId"), QStringLiteral("UCselftest") } } };
    queue.replace({ queued(QStringLiteral("a")), queued(QStringLiteral("b")), credited,
                    queued(QStringLiteral("d")), queued(QStringLiteral("e"), true) }, 2);

    QueueModel back;
    t.check(back.restoreSnapshot(throughJson(queue.snapshot(500))), QStringLiteral("a snapshot through JSON is put back"));
    t.check(idsOf(back) == QLatin1String("a b c d e") && back.currentIndex() == 2,
            QStringLiteral("  in its order, at the song that was playing"),
            QStringLiteral("%1, at %2").arg(idsOf(back)).arg(back.currentIndex()));
    const QueueTrack *c = back.at(2);
    t.check(c && c->title == credited.title && c->album == credited.album && c->albumId == credited.albumId
                && c->artwork == credited.artwork && c->primaryArtist == credited.primaryArtist && c->isVideo
                && c->trackId == 42 && c->durationMs == credited.durationMs && c->credits == credited.credits,
            QStringLiteral("  with every field of its songs"));
    t.check(back.at(4) && back.at(4)->fromRadio && back.radioStartIndex() == 4,
            QStringLiteral("  and the radio's songs still the radio's"));

    // Shuffled: the order stays as it was left, and shuffle off brings back
    // the one before it.
    const QList<QueueTrack> six = { queued(QStringLiteral("a")), queued(QStringLiteral("b")), queued(QStringLiteral("c")),
                                    queued(QStringLiteral("d")), queued(QStringLiteral("e")), queued(QStringLiteral("f")) };
    QString shuffled;
    for (int round = 0; round < 5 && (shuffled.isEmpty() || shuffled == QLatin1String("a b c d e f")); ++round) {
        queue.replace(six, 0);
        queue.shuffleUpcoming();
        shuffled = idsOf(queue);
    }
    QueueModel reshuffled;
    reshuffled.restoreSnapshot(throughJson(queue.snapshot(500)));
    t.check(idsOf(reshuffled) == shuffled, QStringLiteral("a shuffled queue comes back in its shuffled order"),
            idsOf(reshuffled));
    reshuffled.restoreOrder();
    t.check(idsOf(reshuffled) == QLatin1String("a b c d e f"),
            QStringLiteral("  and shuffle off then puts the order back"), idsOf(reshuffled));

    // A long queue: 500 rows around the one playing, a fifth of them behind,
    // or more where fewer are still to come.
    QList<QueueTrack> many;
    for (int i = 0; i < 1000; ++i)
        many.append(queued(QStringLiteral("t%1").arg(i)));
    const auto window = [&queue, &many](int current) {
        queue.replace(many, current);
        QueueModel kept;
        kept.restoreSnapshot(throughJson(queue.snapshot(500)));
        return QStringLiteral("%1 rows, %2 to %3, at %4 (%5)")
            .arg(kept.rowCount())
            .arg(kept.at(0) ? kept.at(0)->videoId : QString())
            .arg(kept.at(kept.rowCount() - 1) ? kept.at(kept.rowCount() - 1)->videoId : QString())
            .arg(kept.currentIndex())
            .arg(kept.current() ? kept.current()->videoId : QString());
    };
    QString got = window(300);
    t.check(got == QLatin1String("500 rows, t200 to t699, at 100 (t300)"),
            QStringLiteral("a long queue keeps 500 rows, 100 of them behind the song playing"), got);
    got = window(995);
    t.check(got == QLatin1String("500 rows, t500 to t999, at 495 (t995)"),
            QStringLiteral("  near its end, the last 500"), got);
    got = window(3);
    t.check(got == QLatin1String("500 rows, t0 to t499, at 3 (t3)"), QStringLiteral("  near its start, the first 500"), got);

    // Nothing to put back.
    queue.replace(six, 1);
    t.check(!queue.restoreSnapshot({}) && idsOf(queue) == QLatin1String("a b c d e f") && queue.currentIndex() == 1,
            QStringLiteral("an empty snapshot puts nothing back and leaves the queue be"));
    QVariantMap outOfRange = throughJson(queue.snapshot(500));
    outOfRange.insert(QStringLiteral("current"), 9);
    t.check(!back.restoreSnapshot(outOfRange), QStringLiteral("a snapshot whose song is not in it is refused"));
    QueueModel empty;
    t.check(empty.snapshot(500).isEmpty(), QStringLiteral("an empty queue gives an empty snapshot"));
}

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

QString titlesOf(PlaybackController &player)
{
    QStringList titles;
    for (int i = 0; i < player.queue()->rowCount(); ++i)
        titles << player.queue()->at(i)->title;
    return titles.join(QStringLiteral(", "));
}

} // namespace

int runSessionSelfTest(Library *library)
{
    Checks t;
    const QString data = qEnvironmentVariable("MONOLIST_DATA_DIR");
    if (data.isEmpty() || !library) {
        t.check(false, QStringLiteral("MONOLIST_DATA_DIR set to a scratch folder"));
        return t.finish();
    }

    testSnapshot(t);

    t.note(QStringLiteral("- a launch opens where the last one left off"));
    const QByteArray tone = toneWav(kToneSeconds);
    const QString tonePath = QDir(data).filePath(QStringLiteral("session-tone.wav"));
    QFile toneFile(tonePath);
    if (!toneFile.open(QIODevice::WriteOnly) || toneFile.write(tone) != tone.size()) {
        t.check(false, QStringLiteral("a tone written to the scratch folder"), tonePath);
        return t.finish();
    }
    toneFile.close();
    StandIn server(tone);
    if (!t.check(server.listen(), QStringLiteral("a stand-in server on this computer")))
        return t.finish();

    MpvEngine engine;
    if (!engine.isValid()) {
        t.check(false, QStringLiteral("an audio engine to drive"), engine.lastError());
        return t.finish();
    }
    StreamResolver resolver;
    resolver.setSaavnEnabled(false);
    for (const QString &id : { QStringLiteral("sessA"), QStringLiteral("sessB") })
        resolver.setTestAnswer(id, StreamResolver::TierInnerTube, server.url(QStringLiteral("/") + id));
    DownloadManager downloads;

    // Each launch its own player, on the one engine, as each launch reads the
    // settings the last one wrote.
    const auto launch = [&]() {
        auto player = std::make_unique<PlaybackController>(&engine, &resolver, &downloads);
        player->setLibrary(library);
        player->restoreSettings();
        resolver.setSaavnEnabled(false);
        player->setAutoplay(false);
        player->setVolume(0.2);
        return player;
    };
    library->setSetting(QStringLiteral("player.queue"), QString());
    library->setSetting(QStringLiteral("player.place"), QString());
    library->setSetting(QStringLiteral("player.shuffle"), QStringLiteral("0"));

    const qint64 left = 12000;
    {
        auto first = launch();
        t.check(!first->restoreSession(), QStringLiteral("a first launch has nothing to put back"));
        first->playTracks({ row(QStringLiteral("sessA"), QStringLiteral("Song A")),
                            row(QStringLiteral("sessB"), QStringLiteral("Song B")),
                            row(QString(), QStringLiteral("Song C"), tonePath) },
                          1, QStringLiteral("selftest"));
        const bool started = waitUntil([&]() { return engine.hasAudioStarted() && first->playing(); }, 8000);
        t.check(started, QStringLiteral("Song B streams from the stand-in"), first->statusText());
        first->setPosition(left);
        waitUntil([&]() { return first->position() >= left + 500; }, 4000);
        first->pause();
        pause(300);
        first->saveSession();
        const QVariantMap queue = setting(library, QStringLiteral("player.queue"));
        const QVariantMap place = setting(library, QStringLiteral("player.place"));
        t.check(queue.value(QStringLiteral("tracks")).toList().size() == 3
                    && queue.value(QStringLiteral("current")).toInt() == 1
                    && queue.value(QStringLiteral("origin")).toString() == QLatin1String("selftest"),
                QStringLiteral("the queue is written: three songs, the second playing, where they came from"));
        t.check(place.value(QStringLiteral("key")).toString() == QLatin1String("sessB")
                    && place.value(QStringLiteral("ms")).toLongLong() >= left,
                QStringLiteral("  and the place in Song B"),
                QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(place)).toJson(QJsonDocument::Compact)));
    }
    const qint64 placeB = setting(library, QStringLiteral("player.place")).value(QStringLiteral("ms")).toLongLong();

    {
        // Shuffle was left on: the queue comes back as it was, not shuffled
        // again.
        library->setSetting(QStringLiteral("player.shuffle"), QStringLiteral("1"));
        auto second = launch();
        t.check(second->restoreSession(), QStringLiteral("the next launch puts the queue back"));
        t.check(titlesOf(*second) == QLatin1String("Song A, Song B, Song C") && second->currentIndex() == 1,
                QStringLiteral("  in its order, shuffle on or not, at Song B"), titlesOf(*second));
        t.check(second->currentTrack().value(QStringLiteral("title")).toString() == QLatin1String("Song B")
                    && !second->playing(),
                QStringLiteral("  with Song B loaded and not playing"));
        t.check(second->position() == placeB, QStringLiteral("  showing the place it was left at"),
                QStringLiteral("%1 ms").arg(second->position()));
        waitUntil([&]() { return engine.hasLoadedFile(); }, 8000);
        pause(500);
        t.check(!second->playing() && qAbs(second->position() - placeB) < 1000,
                QStringLiteral("  loaded there, paused"), QStringLiteral("%1 ms").arg(second->position()));
        second->play();
        const bool playedOn = waitUntil([&]() { return second->position() >= placeB + 1000; }, 6000);
        t.check(playedOn && second->position() < placeB + 5000,
                QStringLiteral("Play carries on from there"), QStringLiteral("%1 ms").arg(second->position()));

        // The queue is written again a moment after it changes.
        second->playIndex(2);
        const bool rewritten = waitUntil([&]() {
            return setting(library, QStringLiteral("player.queue")).value(QStringLiteral("current")).toInt() == 2;
        }, 5000);
        t.check(rewritten, QStringLiteral("a change of song is written a moment later"));
        waitUntil([&]() { return engine.hasAudioStarted() && second->playing(); }, 6000);
        second->setPosition(8000);
        waitUntil([&]() { return second->position() >= 8300; }, 4000);
        second->pause();
        pause(300);
        second->saveSession();
    }
    library->setSetting(QStringLiteral("player.shuffle"), QStringLiteral("0"));
    const qint64 placeC = setting(library, QStringLiteral("player.place")).value(QStringLiteral("ms")).toLongLong();

    {
        auto third = launch();
        t.check(third->restoreSession() && third->currentIndex() == 2,
                QStringLiteral("a file left playing comes back as the song playing"));
        waitUntil([&]() { return engine.hasLoadedFile(); }, 5000);
        pause(400);
        t.check(qAbs(third->position() - placeC) < 1000 && placeC >= 8000,
                QStringLiteral("  loaded at its place"), QStringLiteral("%1 ms, left at %2").arg(third->position()).arg(placeC));
        third->play();
        const bool playedOn = waitUntil([&]() { return third->position() >= placeC + 1000; }, 6000);
        t.check(playedOn && third->position() < placeC + 5000, QStringLiteral("  and plays on from it"),
                QStringLiteral("%1 ms").arg(third->position()));
        third->pause();
        pause(200);
    }

    // Places that are not worth going back to.
    const auto openedAt = [&](const QString &key, qint64 ms) {
        library->setSetting(QStringLiteral("player.place"),
                            QString::fromUtf8(QJsonDocument(QJsonObject{ { QStringLiteral("key"), key },
                                                                         { QStringLiteral("ms"), ms } })
                                                  .toJson(QJsonDocument::Compact)));
        auto player = launch();
        if (!player->restoreSession())
            return qint64(-1);
        return player->position();
    };
    qint64 at = openedAt(QStringLiteral("someOtherSong"), 9000);
    t.check(at == 0, QStringLiteral("a place written for another song starts this one from the top"),
            QStringLiteral("%1 ms").arg(at));
    at = openedAt(tonePath, 25000);
    t.check(at == 0, QStringLiteral("a place in a song's last 10 s starts it from the top"), QStringLiteral("%1 ms").arg(at));
    at = openedAt(tonePath, 3000);
    t.check(at == 0, QStringLiteral("a place in its first 5 s too"), QStringLiteral("%1 ms").arg(at));
    at = openedAt(tonePath, 15000);
    t.check(at == 15000, QStringLiteral("  and one in between is gone back to"), QStringLiteral("%1 ms").arg(at));

    // Offline at launch: the song that will not load stays where it is, and
    // Play, once it can load, begins it from its place.
    {
        const QVariantMap queue{ { QStringLiteral("tracks"),
                                   QVariantList{ row(QStringLiteral("sessA"), QStringLiteral("Song A")),
                                                 row(QStringLiteral("sessB"), QStringLiteral("Song B")) } },
                                 { QStringLiteral("current"), 1 } };
        library->setSetting(QStringLiteral("player.queue"),
                            QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(queue)).toJson(QJsonDocument::Compact)));
        library->setSetting(QStringLiteral("player.place"), QStringLiteral(R"({"key":"sessB","ms":14000})"));
        for (const int tier : { int(StreamResolver::TierInnerTube), int(StreamResolver::TierYtDlp),
                                int(StreamResolver::TierMuxed), int(StreamResolver::TierPiped),
                                int(StreamResolver::TierInvidious), int(StreamResolver::TierSignedIn),
                                int(StreamResolver::TierJioSaavn) })
            resolver.setTestAnswer(QStringLiteral("sessB"), tier, QString());
        resolver.invalidate(QStringLiteral("sessB"));   // its link from before, kept
        auto offline = launch();
        t.check(offline->restoreSession(), QStringLiteral("offline, the queue is put back all the same"));
        const bool failed = waitUntil([&]() { return offline->statusText() == QLatin1String("Source unavailable"); }, 8000);
        t.check(failed && offline->currentIndex() == 1 && !offline->playing(),
                QStringLiteral("  its song, which will not load, stays the song, not playing"),
                QStringLiteral("%1, at song %2").arg(offline->statusText()).arg(offline->currentIndex() + 1));
        t.check(offline->position() == 14000, QStringLiteral("  at its place"),
                QStringLiteral("%1 ms").arg(offline->position()));
        resolver.setTestAnswer(QStringLiteral("sessB"), StreamResolver::TierInnerTube, server.url(QStringLiteral("/sessB")));
        offline->play();
        const bool playedOn = waitUntil([&]() { return offline->playing() && offline->position() >= 15000; }, 8000);
        t.check(playedOn && offline->position() < 19000,
                QStringLiteral("Play, once it can load, begins it from there"),
                QStringLiteral("%1 ms, %2").arg(offline->position()).arg(offline->statusText()));
        offline->pause();
        pause(200);
    }

    // A queue that does not read.
    library->setSetting(QStringLiteral("player.queue"), QStringLiteral("{not json"));
    t.check(!launch()->restoreSession(), QStringLiteral("a queue that does not read is not put back"));
    library->setSetting(QStringLiteral("player.queue"), QString());
    t.check(!launch()->restoreSession(), QStringLiteral("  nor an empty one"));

    engine.stop();
    library->setSetting(QStringLiteral("player.place"), QString());
    QFile::remove(tonePath);
    return t.finish();
}
