#include "connectionselftest.h"

#include "lastfm.h"
#include "secretstore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include <QUrlQuery>

#include <iterator>

namespace {

// One line per check, and a count at the end. Descriptions stay ASCII: the
// console these are read in is not always UTF-8.
class Checks
{
public:
    explicit Checks(const char *prefix) : m_prefix(prefix) {}

    bool check(bool ok, const QString &what, const QString &detail = QString())
    {
        ++m_count;
        if (!ok)
            ++m_failed;
        const QString line = ok || detail.isEmpty() ? what : what + QStringLiteral("  -- ") + detail;
        qWarning("%s: %s  %s", m_prefix, ok ? "ok  " : "FAIL", qPrintable(line));
        return ok;
    }

    void note(const QString &text) { qWarning("%s: %s", m_prefix, qPrintable(text)); }

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

// — SecretStore —

using Status = SecretStore::Status;

QString said(Status status, const QString &error)
{
    return SecretStore::statusText(status) + (error.isEmpty() ? QString() : QStringLiteral(": ") + error);
}

// The file a file-backed store keeps `name` in, found by name rather than by
// knowing the backend's extension.
QString fileFor(const QString &name)
{
    const QDir folder(SecretStore::folderPath());
    const QStringList found = folder.entryList({ name + QStringLiteral(".*") }, QDir::Files);
    return found.isEmpty() ? QString() : folder.filePath(found.first());
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
}

// A fixed pseudo-random run of bytes: the same on every machine, so a
// failure can be repeated, and with every byte value in it.
QByteArray noise(int size)
{
    QByteArray bytes(size, Qt::Uninitialized);
    quint32 state = 0x2545F491u;
    for (int i = 0; i < size; ++i) {
        state = state * 1664525u + 1013904223u;
        bytes[i] = char(state >> 24);
    }
    return bytes;
}

}

int runSecretStoreSelfTest()
{
    Checks t("secret");

    if (!SecretStore::available()) {
        t.note(QStringLiteral("no secret store here: ") + SecretStore::unavailableReason());
        QString error;
        QByteArray value("stale");
        t.check(SecretStore::write(QStringLiteral("selftest.unavailable"), "TESTSECRET", &error) == Status::Unavailable,
                QStringLiteral("write says unavailable"), said(Status::Unavailable, error));
        t.check(SecretStore::read(QStringLiteral("selftest.unavailable"), &value) == Status::Unavailable && value.isEmpty(),
                QStringLiteral("read says unavailable and returns nothing"));
        t.check(SecretStore::remove(QStringLiteral("selftest.unavailable")) == Status::Unavailable,
                QStringLiteral("remove says unavailable"));
        return t.finish();
    }

    const bool fileBacked = !SecretStore::folderPath().isEmpty();
    t.note(QStringLiteral("store: %1%2").arg(SecretStore::backendName(),
                                            fileBacked ? QStringLiteral(" in ") + QDir::toNativeSeparators(SecretStore::folderPath())
                                                       : QString()));

    const QStringList names = { QStringLiteral("selftest.roundtrip"), QStringLiteral("selftest.large"),
                                QStringLiteral("selftest.empty"), QStringLiteral("selftest.swap-a"),
                                QStringLiteral("selftest.swap-b"), QStringLiteral("selftest.never-written") };
    // Left over from a run that crashed part-way.
    for (const QString &name : names)
        SecretStore::remove(name);

    // Names: every one becomes a file name.
    struct NameCase { const char *name; bool valid; };
    const NameCase nameCases[] = {
        { "lastfm.session", true }, { "ytmusic.cookies", true }, { "selftest-1_a", true },
        { "", false }, { "..", false }, { ".hidden", false }, { "a/b", false }, { "a\\b", false },
        { "Upper.case", false }, { "con", false }, { "nul.session", false }, { "com1.x", false },
        { "console.session", true },
    };
    int wrongNames = 0;
    for (const NameCase &c : nameCases) {
        if (SecretStore::validName(QString::fromLatin1(c.name)) != c.valid) {
            ++wrongNames;
            t.note(QStringLiteral("  name \"%1\" judged %2").arg(QString::fromLatin1(c.name),
                                                               c.valid ? QStringLiteral("invalid") : QStringLiteral("valid")));
        }
    }
    t.check(wrongNames == 0, QStringLiteral("name rules: %1 cases").arg(int(std::size(nameCases))));
    {
        QString error;
        t.check(SecretStore::write(QStringLiteral("../selftest.escape"), "TESTSECRET", &error) == Status::Failed
                    && !QFileInfo::exists(QDir(SecretStore::folderPath()).filePath(QStringLiteral("../selftest.escape.dpapi"))),
                QStringLiteral("a name with a path in it is refused, and nothing is written"));
    }

    // Round trip: text, a NUL, control and high bytes, and UTF-8.
    const QByteArray marker = QByteArrayLiteral("TESTSECRET-not-a-real-key");
    const QByteArray secret = marker + QByteArray("\0\x01\xff", 3) + QStringLiteral("Sigur Rós ✓").toUtf8();
    {
        QString error;
        const Status wrote = SecretStore::write(QStringLiteral("selftest.roundtrip"), secret, &error);
        t.check(wrote == Status::Ok, QStringLiteral("write %1 bytes").arg(secret.size()), said(wrote, error));
        QByteArray back;
        const Status read = SecretStore::read(QStringLiteral("selftest.roundtrip"), &back, &error);
        t.check(read == Status::Ok && back == secret, QStringLiteral("read back the same %1 bytes").arg(secret.size()),
                said(read, error) + QStringLiteral(", %1 bytes").arg(back.size()));
    }
    if (fileBacked) {
        const QByteArray onDisk = readFile(fileFor(QStringLiteral("selftest.roundtrip")));
        t.check(!onDisk.isEmpty() && !onDisk.contains(marker) && !onDisk.contains(secret),
                QStringLiteral("the file (%1 bytes) does not contain the secret in the clear").arg(onDisk.size()));
    }

    // Replaced, not appended to.
    {
        const QByteArray second = QByteArrayLiteral("TESTSECRET-second-value");
        QByteArray back;
        t.check(SecretStore::write(QStringLiteral("selftest.roundtrip"), second) == Status::Ok
                    && SecretStore::read(QStringLiteral("selftest.roundtrip"), &back) == Status::Ok && back == second,
                QStringLiteral("a second write replaces the first"));
        SecretStore::write(QStringLiteral("selftest.roundtrip"), secret);
    }

    // The size of a cookie jar, several times over.
    {
        const QByteArray large = noise(256 * 1024);
        QString error;
        QByteArray back;
        const Status wrote = SecretStore::write(QStringLiteral("selftest.large"), large, &error);
        const Status read = wrote == Status::Ok ? SecretStore::read(QStringLiteral("selftest.large"), &back, &error) : wrote;
        t.check(read == Status::Ok && back == large, QStringLiteral("256 KiB of every byte value round-trips"),
                said(read, error));
    }

    // Nothing at all, which is still something stored.
    {
        QString error;
        QByteArray back("stale");
        const Status wrote = SecretStore::write(QStringLiteral("selftest.empty"), QByteArray(), &error);
        const Status read = wrote == Status::Ok ? SecretStore::read(QStringLiteral("selftest.empty"), &back, &error) : wrote;
        t.check(read == Status::Ok && back.isEmpty(), QStringLiteral("an empty secret round-trips as empty"),
                said(read, error));
    }

    // Never written.
    {
        QByteArray back("stale");
        const Status read = SecretStore::read(QStringLiteral("selftest.never-written"), &back);
        t.check(read == Status::NotFound && back.isEmpty(), QStringLiteral("a name never written is not found, and returns nothing"),
                SecretStore::statusText(read));
    }

    // Tampering. Every way the file can be damaged must fail as Corrupt and
    // return nothing, never a wrong value; and the untouched file must still
    // read afterwards, so it was the damage that failed it.
    if (fileBacked) {
        const QString path = fileFor(QStringLiteral("selftest.roundtrip"));
        const QByteArray original = readFile(path);
        const auto corrupt = [&](const QString &what, const QByteArray &damaged) {
            writeFile(path, damaged);
            QString error;
            QByteArray back("stale");
            const Status read = SecretStore::read(QStringLiteral("selftest.roundtrip"), &back, &error);
            t.check(read == Status::Corrupt && back.isEmpty(), what, said(read, error));
            if (read == Status::Corrupt)
                t.note(QStringLiteral("  said: ") + error);
        };
        QByteArray flipped = original;
        flipped[flipped.size() / 2] = char(flipped.at(flipped.size() / 2) ^ 0x01);
        corrupt(QStringLiteral("one bit changed in the middle of the blob is refused"), flipped);
        QByteArray lastByte = original;
        lastByte[lastByte.size() - 1] = char(lastByte.at(lastByte.size() - 1) ^ 0x80);
        corrupt(QStringLiteral("one bit changed in the last byte is refused"), lastByte);
        QByteArray header = original;
        header[0] = 'X';
        corrupt(QStringLiteral("a changed header is refused"), header);
        corrupt(QStringLiteral("a file cut to half is refused"), original.left(original.size() / 2));
        corrupt(QStringLiteral("an empty file is refused"), QByteArray());
        corrupt(QStringLiteral("a file of something else entirely is refused"), noise(int(original.size())));

        writeFile(path, original);
        QByteArray back;
        t.check(SecretStore::read(QStringLiteral("selftest.roundtrip"), &back) == Status::Ok && back == secret,
                QStringLiteral("the original file, put back, reads again"));

        // One secret's file copied over another's name does not open as it.
        QByteArray swapped("stale");
        QString error;
        SecretStore::write(QStringLiteral("selftest.swap-a"), "TESTSECRET-A");
        SecretStore::write(QStringLiteral("selftest.swap-b"), "TESTSECRET-B");
        writeFile(fileFor(QStringLiteral("selftest.swap-b")), readFile(fileFor(QStringLiteral("selftest.swap-a"))));
        const Status read = SecretStore::read(QStringLiteral("selftest.swap-b"), &swapped, &error);
        t.check(read == Status::Corrupt && swapped.isEmpty(),
                QStringLiteral("a file copied over another name's is refused"), said(read, error));
    } else {
        t.note(QStringLiteral("tamper checks skipped: this store keeps no files"));
    }

    // Delete: the file goes, the name reads as never written, and deleting
    // again is not an error.
    {
        const QString path = fileBacked ? fileFor(QStringLiteral("selftest.roundtrip")) : QString();
        QString error;
        const Status removed = SecretStore::remove(QStringLiteral("selftest.roundtrip"), &error);
        t.check(removed == Status::Ok, QStringLiteral("remove"), said(removed, error));
        if (fileBacked)
            t.check(!path.isEmpty() && !QFileInfo::exists(path), QStringLiteral("the file is gone from disk"));
        QByteArray back("stale");
        t.check(SecretStore::read(QStringLiteral("selftest.roundtrip"), &back) == Status::NotFound && back.isEmpty(),
                QStringLiteral("after remove, it reads as not found"));
        t.check(SecretStore::remove(QStringLiteral("selftest.roundtrip")) == Status::Ok,
                QStringLiteral("removing it again is not an error"));
    }

    for (const QString &name : names)
        SecretStore::remove(name);
    if (fileBacked) {
        const QStringList left = QDir(SecretStore::folderPath()).entryList({ QStringLiteral("selftest.*") }, QDir::Files);
        t.check(left.isEmpty(), QStringLiteral("no test secrets left behind"), left.join(QStringLiteral(", ")));
    }
    return t.finish();
}

// — Last.fm —

namespace {

using Params = LastFmApi::Params;
using Outcome = LastFmApi::Outcome;

// Invented, and shaped like the real thing so the lengths are realistic.
const QString kKey = QStringLiteral("TESTAPIKEY0123456789abcdef012345");
const QByteArray kSecret = QByteArrayLiteral("TESTSHAREDSECRET0123456789abcdef");
const QString kSession = QStringLiteral("TESTSESSIONKEY0000");

// What a server does with an application/x-www-form-urlencoded body: '+' is
// a space, then %XX is a byte, then the bytes are UTF-8.
Params decodeForm(const QByteArray &body)
{
    Params params;
    for (const QByteArray &pair : body.split('&')) {
        const int equals = pair.indexOf('=');
        const auto decode = [](QByteArray part) {
            part.replace('+', ' ');
            return QString::fromUtf8(QByteArray::fromPercentEncoding(part));
        };
        params.append({ decode(pair.left(equals)), decode(equals < 0 ? QByteArray() : pair.mid(equals + 1)) });
    }
    return params;
}

// The server's side of a signed request: take away api_sig, sign what is left
// with the same secret, and compare.
bool serverAccepts(const Params &received)
{
    QString sent;
    Params rest;
    for (const auto &param : received) {
        if (param.first == QLatin1String("api_sig"))
            sent = param.second;
        else
            rest.append(param);
    }
    return !sent.isEmpty() && LastFmApi::signature(rest, kSecret) == sent.toLatin1();
}

}

int runLastFmSelfTest()
{
    Checks t("lastfm");

    {
        LastFmApi api;
        t.note(api.hasKey() ? QStringLiteral("this build has a Last.fm key")
                            : QStringLiteral("this build has no Last.fm key"));
        t.note(QStringLiteral("secret store: %1").arg(SecretStore::available() ? SecretStore::backendName()
                                                                              : SecretStore::unavailableReason()));
        t.note(api.available() ? QStringLiteral("connecting can work here")
                               : QStringLiteral("the Settings row says: ") + api.unavailableReason());
    }

    // — api_sig, against signatures computed apart from this code: each
    // signature string written out by hand in sorted order, as the spec
    // describes it, and hashed with .NET's MD5 —
    const Params getSession = {
        { QStringLiteral("method"), QStringLiteral("auth.getSession") },
        { QStringLiteral("api_key"), kKey },
        { QStringLiteral("token"), QStringLiteral("TESTTOKEN6789") },
    };
    t.check(LastFmApi::signature(getSession, kSecret) == "721270434e532ecf0edc2740d2ebd57d",
            QStringLiteral("api_sig, auth.getSession (ASCII)"), QString::fromLatin1(LastFmApi::signature(getSession, kSecret)));

    Params withFormat = getSession;
    withFormat.append({ QStringLiteral("format"), QStringLiteral("json") });
    withFormat.append({ QStringLiteral("callback"), QStringLiteral("cb") });
    t.check(LastFmApi::signature(withFormat, kSecret) == "721270434e532ecf0edc2740d2ebd57d",
            QStringLiteral("api_sig leaves out format and callback"));

    const Params nowPlaying = {
        { QStringLiteral("method"), QStringLiteral("track.updateNowPlaying") },
        { QStringLiteral("artist"), QStringLiteral("Sigur Rós") },
        { QStringLiteral("track"), QStringLiteral("Hoppípolla") },
        { QStringLiteral("album"), QStringLiteral("Takk…") },
        { QStringLiteral("duration"), QStringLiteral("268") },
        { QStringLiteral("api_key"), kKey },
        { QStringLiteral("sk"), kSession },
    };
    t.check(LastFmApi::signature(nowPlaying, kSecret) == "26781332d6d4419a3671b14fd130351c",
            QStringLiteral("api_sig, track.updateNowPlaying (UTF-8: o-acute, i-acute, an ellipsis)"),
            QString::fromLatin1(LastFmApi::signature(nowPlaying, kSecret)));

    const Params batch = {
        { QStringLiteral("method"), QStringLiteral("track.scrobble") },
        { QStringLiteral("api_key"), kKey },
        { QStringLiteral("sk"), kSession },
        { QStringLiteral("artist[0]"), QStringLiteral("Simon & Garfunkel") },
        { QStringLiteral("track[0]"), QStringLiteral("1 + 1 = 2") },
        { QStringLiteral("timestamp[0]"), QStringLiteral("1790000000") },
        { QStringLiteral("artist[1]"), QStringLiteral("AC/DC") },
        { QStringLiteral("track[1]"), QStringLiteral("T.N.T.") },
        { QStringLiteral("timestamp[1]"), QStringLiteral("1790000300") },
        { QStringLiteral("chosenByUser[1]"), QStringLiteral("0") },
        { QStringLiteral("artist[10]"), QStringLiteral("Tyler, The Creator") },
        { QStringLiteral("track[10]"), QStringLiteral("See You Again") },
        { QStringLiteral("timestamp[10]"), QStringLiteral("1790000600") },
    };
    const QByteArray batchSig = LastFmApi::signature(batch, kSecret);
    t.check(batchSig == "5ceaacc6ee2424f5860afcbc4a9c89f5",
            QStringLiteral("api_sig, track.scrobble batch ('&', '+', '=', and [10] sorted before [1])"),
            QString::fromLatin1(batchSig));
    t.check(batchSig != "a832ce643801ddf817679440311f54a7",
            QStringLiteral("  and not the signature of the same batch sorted 0, 1, 10"));

    // — sign(): api_sig, then format=json, whatever came in —
    {
        Params stale = getSession;
        stale.append({ QStringLiteral("api_sig"), QStringLiteral("0000") });
        stale.append({ QStringLiteral("format"), QStringLiteral("xml") });
        const Params sent = LastFmApi::sign(stale, kSecret);
        const bool shape = sent.size() == getSession.size() + 2
                           && sent.at(sent.size() - 2) == qMakePair(QStringLiteral("api_sig"),
                                                                   QStringLiteral("721270434e532ecf0edc2740d2ebd57d"))
                           && sent.last() == qMakePair(QStringLiteral("format"), QStringLiteral("json"));
        t.check(shape, QStringLiteral("sign() replaces a stale api_sig and format, and ends with format=json"));
    }

    // — the body —
    {
        const Params some = {
            { QStringLiteral("artist[0]"), QStringLiteral("Simon & Garfunkel") },
            { QStringLiteral("track[0]"), QStringLiteral("1 + 1 = 2") },
            { QStringLiteral("artist"), QStringLiteral("Sigur Rós") },
            { QStringLiteral("album"), QStringLiteral("Takk…") },
        };
        const QByteArray body = LastFmApi::formBody(some);
        const QByteArray expected = "artist%5B0%5D=Simon%20%26%20Garfunkel&track%5B0%5D=1%20%2B%201%20%3D%202"
                                    "&artist=Sigur%20R%C3%B3s&album=Takk%E2%80%A6";
        t.check(body == expected, QStringLiteral("form body, byte for byte"), QString::fromLatin1(body));
        t.check(!body.contains('+') && body.contains("%2B"), QStringLiteral("'+' is sent as %2B, never bare"));
        t.check(decodeForm(body) == some, QStringLiteral("the body decodes, as a server decodes it, to what was sent"));
    }
    for (const Params &request : { getSession, nowPlaying, batch }) {
        const QByteArray body = LastFmApi::formBody(LastFmApi::sign(request, kSecret));
        const Params received = decodeForm(body);
        t.check(serverAccepts(received) && received.last().second == QLatin1String("json"),
                QStringLiteral("%1: the server's own check of the signature passes").arg(request.first().second));
    }
    {
        // Why not QUrlQuery. Reported, not counted: it is Qt's behaviour
        // being shown, and the check above is what guards against it.
        QUrlQuery query;
        for (const auto &param : LastFmApi::sign(batch, kSecret))
            query.addQueryItem(param.first, param.second);
        const Params received = decodeForm(query.query(QUrl::FullyEncoded).toUtf8());
        t.note(QStringLiteral("(QUrlQuery's body for the batch would %1 the server's signature check)")
                   .arg(serverAccepts(received) ? QStringLiteral("pass") : QStringLiteral("FAIL")));
    }

    // — answers —
    struct Answer {
        const char *what;
        int http;
        const char *body;
        bool transportFailed;
        Outcome outcome;
        int error;
        int accepted;
        int ignored;
        QList<int> codes;
    };
    const Answer answers[] = {
        { "scrobble, one accepted (an object, not an array)", 200,
          R"({"scrobbles":{"scrobble":{"artist":{"corrected":"0","#text":"AC/DC"},"album":{"corrected":"0","#text":""},)"
          R"("track":{"corrected":"0","#text":"T.N.T."},"ignoredMessage":{"code":"0","#text":""},)"
          R"("albumArtist":{"corrected":"0","#text":""},"timestamp":"1790000300"},"@attr":{"ignored":0,"accepted":1}}})",
          false, Outcome::Ok, 0, 1, 0, { 0 } },
        { "scrobble, a batch of three, two ignored (counts as strings)", 200,
          R"({"scrobbles":{"scrobble":[)"
          R"({"track":{"#text":"A"},"ignoredMessage":{"code":"0","#text":""},"timestamp":"1790000000"},)"
          R"({"track":{"#text":"B"},"ignoredMessage":{"code":"1","#text":"Artist was ignored"},"timestamp":"1790000100"},)"
          R"({"track":{"#text":"C"},"ignoredMessage":{"code":"3","#text":"Timestamp was too old"},"timestamp":"1590000000"}],)"
          R"("@attr":{"ignored":"2","accepted":"1"}}})",
          false, Outcome::Ok, 0, 1, 2, { 0, 1, 3 } },
        { "scrobble ignored 1, artist ignored", 200,
          R"({"scrobbles":{"scrobble":{"ignoredMessage":{"code":"1","#text":"Artist was ignored"}},"@attr":{"ignored":1,"accepted":0}}})",
          false, Outcome::Ok, 0, 0, 1, { 1 } },
        { "scrobble ignored 2, track ignored", 200,
          R"({"scrobbles":{"scrobble":{"ignoredMessage":{"code":"2","#text":"Track was ignored"}},"@attr":{"ignored":1,"accepted":0}}})",
          false, Outcome::Ok, 0, 0, 1, { 2 } },
        { "scrobble ignored 3, timestamp too old", 200,
          R"({"scrobbles":{"scrobble":{"ignoredMessage":{"code":"3","#text":"Timestamp was too old"}},"@attr":{"ignored":1,"accepted":0}}})",
          false, Outcome::Ok, 0, 0, 1, { 3 } },
        { "scrobble ignored 4, timestamp too new", 200,
          R"({"scrobbles":{"scrobble":{"ignoredMessage":{"code":"4","#text":"Timestamp was too new"}},"@attr":{"ignored":1,"accepted":0}}})",
          false, Outcome::Ok, 0, 0, 1, { 4 } },
        { "scrobble ignored 5, daily limit", 200,
          R"({"scrobbles":{"scrobble":{"ignoredMessage":{"code":"5","#text":"Daily scrobble limit exceeded"}},"@attr":{"ignored":1,"accepted":0}}})",
          false, Outcome::Ok, 0, 0, 1, { 5 } },
        { "now playing, accepted", 200,
          R"({"nowplaying":{"artist":{"corrected":"0","#text":"AC/DC"},"track":{"corrected":"0","#text":"T.N.T."},)"
          R"("ignoredMessage":{"code":"0","#text":""}}})",
          false, Outcome::Ok, 0, 0, 0, { 0 } },
        { "now playing, ignored 1", 200,
          R"({"nowplaying":{"ignoredMessage":{"code":"1","#text":"Artist was ignored"}}})",
          false, Outcome::Ok, 0, 0, 0, { 1 } },
        { "error 4, authentication failed", 403,
          R"({"error":4,"message":"Invalid authentication token supplied"})",
          false, Outcome::Rejected, 4, 0, 0, {} },
        { "error 5, invalid format (a request error, not the daily limit)", 400,
          R"({"error":5,"message":"Invalid format - This service doesn't exist in that format"})",
          false, Outcome::Rejected, 5, 0, 0, {} },
        { "error 6, invalid parameters", 400,
          R"({"error":6,"message":"Invalid parameters - Your request is missing a required parameter"})",
          false, Outcome::Rejected, 6, 0, 0, {} },
        { "error 8, operation failed", 500,
          R"({"error":8,"message":"Operation failed - Most likely the backend service failed. Please try again."})",
          false, Outcome::Rejected, 8, 0, 0, {} },
        { "error 9, invalid session key (with a 403)", 403,
          R"({"error":9,"message":"Invalid session key - Please re-authenticate"})",
          false, Outcome::Reauthenticate, 9, 0, 0, {} },
        { "error 9 sent as a string", 403,
          R"({"error":"9","message":"Invalid session key - Please re-authenticate"})",
          false, Outcome::Reauthenticate, 9, 0, 0, {} },
        { "error 10, invalid API key", 403,
          R"({"error":10,"message":"Invalid API key - You must be granted a valid key by last.fm"})",
          false, Outcome::Hold, 10, 0, 0, {} },
        { "error 11, service offline", 503,
          R"({"error":11,"message":"Service Offline - This service is temporarily offline. Try again later."})",
          false, Outcome::Retry, 11, 0, 0, {} },
        { "error 13, invalid method signature", 403,
          R"({"error":13,"message":"Invalid method signature supplied"})",
          false, Outcome::Hold, 13, 0, 0, {} },
        { "error 14, token not yet approved", 403,
          R"({"error":14,"message":"Unauthorized Token - This token has not been authorized"})",
          false, Outcome::KeepWaiting, 14, 0, 0, {} },
        { "error 15, token expired", 403,
          R"({"error":15,"message":"This token has expired"})",
          false, Outcome::RestartSignIn, 15, 0, 0, {} },
        { "error 16, temporary error", 500,
          R"({"error":16,"message":"There was a temporary error processing your request. Please try again"})",
          false, Outcome::Retry, 16, 0, 0, {} },
        { "error 26, API key suspended", 403,
          R"({"error":26,"message":"Suspended API key - Access for your account has been suspended, please contact Last.fm"})",
          false, Outcome::Hold, 26, 0, 0, {} },
        { "error 29, rate limit", 429,
          R"({"error":29,"message":"Rate Limit Exceeded - Your IP has made too many requests in a short period"})",
          false, Outcome::RateLimited, 29, 0, 0, {} },
        { "no answer at all (network down)", 0, "", true, Outcome::Retry, 0, 0, 0, {} },
        { "a proxy's HTML error page (502)", 502, "<html><body>Bad Gateway</body></html>",
          false, Outcome::Retry, 0, 0, 0, {} },
        { "a body cut off mid-way (200)", 200, R"({"scrobbles":{"scrob)", false, Outcome::Retry, 0, 0, 0, {} },
        { "JSON that is not an object (200)", 200, "[]", false, Outcome::Retry, 0, 0, 0, {} },
        { "an empty object with a 404", 404, "{}", false, Outcome::Retry, 0, 0, 0, {} },
        { "a captive portal's 200 page", 200, "<html>Sign in to the hotel Wi-Fi</html>",
          false, Outcome::Retry, 0, 0, 0, {} },
    };
    int answerFailures = 0;
    for (const Answer &a : answers) {
        const LastFmApi::Reply reply = LastFmApi::parseReply(a.http, QByteArray(a.body), a.transportFailed);
        const bool ok = reply.outcome == a.outcome && reply.error == a.error && reply.accepted == a.accepted
                        && reply.ignored == a.ignored && reply.ignoredCodes == a.codes;
        answerFailures += !ok;
        QStringList codes;
        for (int code : reply.ignoredCodes)
            codes << QString::number(code);
        t.check(ok, QStringLiteral("%1 -> %2").arg(QString::fromLatin1(a.what), LastFmApi::outcomeName(a.outcome)),
                QStringLiteral("got %1, error %2, accepted %3, ignored %4, codes [%5]")
                    .arg(LastFmApi::outcomeName(reply.outcome)).arg(reply.error).arg(reply.accepted)
                    .arg(reply.ignored).arg(codes.join(QLatin1Char(','))));
    }

    // Each scrobble's own fate.
    struct Item { int code; LastFmApi::ItemOutcome outcome; };
    const Item items[] = {
        { 0, LastFmApi::ItemOutcome::Accepted },
        { 1, LastFmApi::ItemOutcome::Dropped }, { 2, LastFmApi::ItemOutcome::Dropped },
        { 3, LastFmApi::ItemOutcome::Dropped }, { 4, LastFmApi::ItemOutcome::Dropped },
        { 5, LastFmApi::ItemOutcome::TryTomorrow },
        { 99, LastFmApi::ItemOutcome::Dropped },
    };
    for (const Item &item : items) {
        const LastFmApi::ItemOutcome got = LastFmApi::itemOutcome(item.code);
        t.check(got == item.outcome, QStringLiteral("ignored code %1 -> %2").arg(item.code)
                                         .arg(LastFmApi::itemOutcomeName(item.outcome)),
                QStringLiteral("got ") + LastFmApi::itemOutcomeName(got));
    }

    // The sign-in answers. The session key is compared, never printed.
    {
        const LastFmApi::Reply reply = LastFmApi::parseReply(200, R"({"token":"TESTTOKEN6789"})");
        t.check(LastFmApi::token(reply) == QLatin1String("TESTTOKEN6789"), QStringLiteral("auth.getToken: the token is read"));
    }
    {
        const LastFmApi::Reply reply = LastFmApi::parseReply(
            200, R"({"session":{"name":"monolist-test","key":"TESTSESSIONKEY0000","subscriber":0}})");
        QString user;
        QByteArray key;
        t.check(LastFmApi::session(reply, &user, &key) && user == QLatin1String("monolist-test")
                    && key == kSession.toUtf8(),
                QStringLiteral("auth.getSession: the user name and session key are read"));
    }
    {
        QString user;
        QByteArray key;
        const LastFmApi::Reply waiting = LastFmApi::parseReply(
            403, R"({"error":14,"message":"Unauthorized Token - This token has not been authorized"})");
        const LastFmApi::Reply partial = LastFmApi::parseReply(200, R"({"session":{"name":"monolist-test"}})");
        t.check(!LastFmApi::session(waiting, &user, &key) && !LastFmApi::session(partial, &user, &key)
                    && user.isEmpty() && key.isEmpty(),
                QStringLiteral("auth.getSession: an error, or a session with no key, gives no session"));
    }

    if (answerFailures == 0)
        t.note(QStringLiteral("every answer read as expected (%1 canned answers)").arg(int(std::size(answers))));
    return t.finish();
}
