#include "saavnselftest.h"

#include "audioalign.h"
#include "des.h"
#include "jiosaavn.h"

#include <cmath>

#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUrl>

namespace {

// One line per check, and a count at the end, as the other self-tests do.
// Descriptions stay ASCII: the console these are read in is not always UTF-8.
class Checks
{
public:
    bool check(bool ok, const QString &what, const QString &detail = QString())
    {
        ++m_count;
        if (!ok)
            ++m_failed;
        const QString line = ok || detail.isEmpty() ? what : what + QStringLiteral("  -- ") + detail;
        qWarning("saavn-test: %s  %s", ok ? "ok  " : "FAIL", qPrintable(line));
        return ok;
    }

    int finish()
    {
        qWarning("saavn-test: %d checks, %d failed", m_count, m_failed);
        return m_failed;
    }

private:
    int m_count = 0;
    int m_failed = 0;
};

QString hex(quint64 value)
{
    return QStringLiteral("%1").arg(value, 16, 16, QLatin1Char('0')).toUpper();
}

Saavn::Target song(const QString &title, const QString &artist, int seconds)
{
    Saavn::Target target;
    target.videoId = QStringLiteral("selftest");
    target.title = title;
    target.artist = artist;
    target.durationMs = qint64(seconds) * 1000;
    return target;
}

Saavn::Row row(const QString &title, const QStringList &artists, int seconds,
               bool explicitContent = false, bool has320 = true, const QString &id = QString())
{
    Saavn::Row r;
    static int next = 0;
    r.id = id.isEmpty() ? QStringLiteral("row%1").arg(++next) : id;
    r.title = Saavn::decodeEntities(title);
    r.artists = artists;
    r.durationSec = seconds;
    r.explicitContent = explicitContent;
    r.has320 = has320;
    r.encryptedUrl = QStringLiteral("unused");
    return r;
}

// The same row or song, on an album and, for a row, in the language
// JioSaavn gives it.
Saavn::Row onAlbum(Saavn::Row r, const QString &album, const QString &language = QString())
{
    r.album = album;
    r.language = language;
    return r;
}

Saavn::Target onAlbum(Saavn::Target target, const QString &album)
{
    target.album = album;
    return target;
}

QJsonObject json(const char *text)
{
    return QJsonDocument::fromJson(QByteArray(text)).object();
}

void testDes(Checks &t)
{
    // FIPS 46-3's worked example, and the one in "The DES Algorithm
    // Illustrated", which encrypts to all zeros.
    const quint64 key = Q_UINT64_C(0x133457799BBCDFF1);
    const quint64 plain = Q_UINT64_C(0x0123456789ABCDEF);
    const quint64 cipher = Des::encryptBlock(key, plain);
    t.check(cipher == Q_UINT64_C(0x85E813540F0AB405),
            QStringLiteral("DES known answer: 133457799BBCDFF1 / 0123456789ABCDEF -> 85E813540F0AB405"),
            hex(cipher));
    t.check(Des::decryptBlock(key, Q_UINT64_C(0x85E813540F0AB405)) == plain,
            QStringLiteral("DES known answer decrypts back to 0123456789ABCDEF"));
    const quint64 zeros = Des::encryptBlock(Q_UINT64_C(0x0E329232EA6D0D73), Q_UINT64_C(0x8787878787878787));
    t.check(zeros == 0, QStringLiteral("DES known answer: 0E329232EA6D0D73 / 8787878787878787 -> 0000000000000000"),
            hex(zeros));

    // Round trips at every length across two blocks, padding included.
    bool roundTrips = true;
    bool sizesRight = true;
    const QByteArray keyBytes = QByteArrayLiteral("38346591");
    for (int length = 0; length <= 24; ++length) {
        QByteArray data(length, Qt::Uninitialized);
        for (int i = 0; i < length; ++i)
            data[i] = char((i * 37 + length * 11) & 0xFF);
        const QByteArray sealed = Des::encryptEcb(keyBytes, data);
        bool ok = false;
        const QByteArray opened = Des::decryptEcb(keyBytes, sealed, &ok);
        roundTrips = roundTrips && ok && opened == data;
        sizesRight = sizesRight && sealed.size() == (length / 8 + 1) * 8;
    }
    t.check(roundTrips, QStringLiteral("ECB with PKCS#5 round-trips every length from 0 to 24 bytes"));
    t.check(sizesRight, QStringLiteral("padding always adds 1 to 8 bytes, a whole block when the input is whole"));

    bool ok = true;
    Des::decryptEcb(keyBytes, QByteArray(7, 'x'), &ok);
    t.check(!ok, QStringLiteral("seven bytes are not a DES ciphertext"));
    const QByteArray sealed = Des::encryptEcb(keyBytes, QByteArrayLiteral("https://example.invalid/a_96.mp4"));
    const QByteArray wrong = Des::decryptEcb(QByteArrayLiteral("12345678"), sealed, &ok);
    t.check(!ok || !wrong.startsWith("https://"), QStringLiteral("the wrong key does not give the link back"));
}

void testLinks(Checks &t)
{
    // Made with another DES (.NET's System.Security.Cryptography.DES, ECB,
    // PKCS7) under the same key, so the two implementations are checked
    // against each other and not only against themselves.
    const QString foreign = QStringLiteral(
        "ID2ieOjCrwfgWvL5sXl4B1ImC5QfbsDyz3UyV/YLalw1cUlUmSS0ruFL/YJ1TKRIZLZvCat1fiU4wrMSvgh34xw7tS9a8Gtq");
    const QString link = QStringLiteral("https://aac.saavncdn.com/815/0f1e2d3c4b5a69788796a5b4c3d2e1f0_96.mp4");
    t.check(Saavn::decryptMediaUrl(foreign) == link,
            QStringLiteral("a link encrypted by another DES decrypts to the fixture"),
            Saavn::decryptMediaUrl(foreign));
    t.check(Saavn::encryptMediaUrl(link) == foreign,
            QStringLiteral("and this encryptor makes exactly the same bytes from it"));

    const QString own = QStringLiteral("https://aac.saavncdn.com/123/selftest-link_160.mp4");
    t.check(Saavn::decryptMediaUrl(Saavn::encryptMediaUrl(own)) == own,
            QStringLiteral("a fixture link made with this encryptor decrypts back"));
    t.check(Saavn::decryptMediaUrl(QStringLiteral("  ") + Saavn::encryptMediaUrl(own) + QStringLiteral("\n")) == own,
            QStringLiteral("whitespace around the encrypted link is ignored"));
    t.check(Saavn::decryptMediaUrl(QStringLiteral("not base64 at all!")).isEmpty(),
            QStringLiteral("text that is not Base64 gives no link"));
    t.check(Saavn::decryptMediaUrl(Saavn::encryptMediaUrl(QStringLiteral("ftp://x/y_96.mp4"))).isEmpty(),
            QStringLiteral("something that decrypts but is not a web link gives no link"));
    const QString plain = Saavn::decryptMediaUrl(
        Saavn::encryptMediaUrl(QStringLiteral("http://aac.saavncdn.com/815/abc_96.mp4")));
    t.check(plain == QStringLiteral("https://aac.saavncdn.com/815/abc_96.mp4"),
            QStringLiteral("a plain-HTTP link on JioSaavn's CDN is moved to HTTPS"), plain);
    t.check(Saavn::decryptMediaUrl(Saavn::encryptMediaUrl(QStringLiteral("https://example.invalid/abc_96.mp4"))).isEmpty(),
            QStringLiteral("a link on any other host gives no link"));
    t.check(Saavn::isCdnLink(QStringLiteral("https://aac.saavncdn.com/815/abc_320.mp4"))
                && Saavn::isCdnLink(QStringLiteral("https://saavncdn.com/abc_320.mp4"))
                && !Saavn::isCdnLink(QStringLiteral("http://aac.saavncdn.com/815/abc_320.mp4"))
                && !Saavn::isCdnLink(QStringLiteral("https://saavncdn.com.example.invalid/abc_320.mp4"))
                && !Saavn::isCdnLink(QStringLiteral("https://evilsaavncdn.com/abc_320.mp4"))
                && !Saavn::isCdnLink(QStringLiteral("https://user@aac.saavncdn.com/abc_320.mp4")),
            QStringLiteral("a remembered link is only taken back on JioSaavn's CDN, over HTTPS"));
}

void testBitrates(Checks &t)
{
    const QString base = QStringLiteral("https://aac.saavncdn.com/815/abc");
    struct Case {
        QString url;
        bool has320;
        QString expectUrl;
        int expectKbps;
        bool readable;
        bool accepted;
        const char *what;
    };
    const QList<Case> cases = {
        { base + "_96.mp4", true, base + "_320.mp4", 320, true, true, "_96 with 320 on offer is moved up to _320" },
        { base + "_96.mp4", false, base + "_96.mp4", 96, true, false, "_96 with no 320 stays 96, and 96 is refused" },
        { base + "_160.mp4", false, base + "_160.mp4", 160, true, true, "_160 with no 320 stays 160, and is taken" },
        { base + "_320.mp4", true, base + "_320.mp4", 320, true, true, "_320 stays as it is" },
        { base + "_48.mp4", false, base + "_48.mp4", 48, true, false, "_48 is refused" },
        { base + ".mp4", false, base + ".mp4", 0, false, true, "a link naming no bitrate is taken, unread" },
        { base + "_96.mp4?t=1", true, base + "_320.mp4?t=1", 320, true, true, "the query string is left alone" },
        { base + "_128.mp4", true, base + "_128.mp4", 0, false, true, "_128 is not one of JioSaavn's, so unread" },
        { base + "_96.mp3", true, base + "_320.mp3", 320, true, true, "an mp3 rendition moves up the same way" },
        { base + "_96.mp4/x.mp4", true, base + "_96.mp4/x.mp4", 0, false, true, "only the end of the path counts" },
    };
    for (const Case &c : cases) {
        const Saavn::Stream stream = Saavn::streamFor(c.url, c.has320);
        const bool accepted = Saavn::acceptable(stream);
        t.check(stream.url == c.expectUrl && stream.kbps == c.expectKbps && stream.readable == c.readable
                    && accepted == c.accepted,
                QString::fromLatin1(c.what),
                QStringLiteral("%1, %2 kbps, readable %3, accepted %4")
                    .arg(QUrl(stream.url).fileName()).arg(stream.kbps).arg(stream.readable).arg(accepted));
    }
}

void testParsing(Checks &t)
{
    // Entities as JioSaavn leaves them in titles.
    t.check(Saavn::decodeEntities(QStringLiteral("Tum Hi Ho (From &quot;Aashiqui 2&quot;)"))
                == QStringLiteral("Tum Hi Ho (From \"Aashiqui 2\")"),
            QStringLiteral("entities: &quot;"));
    t.check(Saavn::decodeEntities(QStringLiteral("Rock &amp; Roll")) == QStringLiteral("Rock & Roll"),
            QStringLiteral("entities: &amp;"));
    t.check(Saavn::decodeEntities(QStringLiteral("Don&#039;t Stop")) == QStringLiteral("Don't Stop"),
            QStringLiteral("entities: &#039;"));
    t.check(Saavn::decodeEntities(QStringLiteral("&#x41;&#66;&lt;&gt;&apos;")) == QStringLiteral("AB<>'"),
            QStringLiteral("entities: hexadecimal, decimal, &lt; &gt; &apos;"));
    t.check(Saavn::decodeEntities(QStringLiteral("&amp;quot;twice&amp;quot;")) == QStringLiteral("\"twice\""),
            QStringLiteral("entities: encoded twice"));
    t.check(Saavn::decodeEntities(QStringLiteral("AT&T & caf&eacute;")) == QStringLiteral("AT&T & caf&eacute;"),
            QStringLiteral("entities: a bare & and an unknown name are left alone"));

    // A search answer: songs kept, anything else skipped, fields read.
    const QList<Saavn::Row> rows = Saavn::rowsFromSearch(json(R"json({"total":2,"results":[
        {"id":"aRZbUYD7","title":"Tum Hi Ho (From &quot;Aashiqui 2&quot;)","type":"song","explicit_content":"1",
         "language":"Hindi",
         "more_info":{"album":"Aashiqui 2","duration":"262","320kbps":"true","encrypted_media_url":"ENC",
           "artistMap":{"primary_artists":[{"name":"Mithoon"},{"name":"Arijit Singh"}],
                        "featured_artists":[{"name":"Guest &amp; Friend"}],
                        "artists":[{"name":"Lyricist Only","role":"lyricist"}]}}},
        {"id":"alb1","title":"An album","type":"album","more_info":{}}]})json"));
    const bool oneRow = rows.size() == 1;
    t.check(oneRow, QStringLiteral("search answer: the song is read and the album skipped"),
            QString::number(rows.size()));
    if (oneRow) {
        const Saavn::Row &r = rows.first();
        t.check(r.title == QStringLiteral("Tum Hi Ho (From \"Aashiqui 2\")") && r.durationSec == 262 && r.has320
                    && r.explicitContent && r.encryptedUrl == QStringLiteral("ENC")
                    && r.album == QStringLiteral("Aashiqui 2") && r.language == QStringLiteral("hindi"),
                QStringLiteral("search row: title decoded, length, 320, explicit, link, album and language"));
        t.check(r.artists == QStringList{ QStringLiteral("Mithoon"), QStringLiteral("Arijit Singh"),
                                          QStringLiteral("Guest & Friend") },
                QStringLiteral("search row: primary and featured artists, not the lyricist"),
                r.artists.join(QStringLiteral(" | ")));
    }

    // An answer with no results list is not "nothing found".
    t.check(Saavn::isSearchAnswer(json(R"json({"total":0,"start":1,"results":[]})json")),
            QStringLiteral("search answer: an empty results list is an answer"));
    t.check(!Saavn::isSearchAnswer(json(R"json({"error":{"code":"RATE_LIMIT","msg":"slow down"}})json")),
            QStringLiteral("search answer: an error object sent as 200 is not"));

    // What a remembered answer was for.
    const Saavn::Target tum = song(QStringLiteral("Tum Hi Ho"), QStringLiteral("Arijit Singh"), 262);
    const QString sig = Saavn::signature(tum);
    t.check(sig.size() == 32 && sig == Saavn::signature(song(QStringLiteral("Tum  Hi Ho"), QStringLiteral("Arijit Singh"), 262))
                && sig != Saavn::signature(song(QStringLiteral("Tum Hi Ho"), QStringLiteral("Arijit Singh"), 275))
                && sig != Saavn::signature(song(QStringLiteral("Tum Hi Ho (Live)"), QStringLiteral("Arijit Singh"), 262))
                && sig != Saavn::signature(onAlbum(tum, QStringLiteral("Aashiqui 2"))),
            QStringLiteral("signature: the same song gives the same, another name, length or album another"), sig);

    // song.getDetails, both shapes.
    Saavn::Row details;
    bool ok = Saavn::rowFromDetails(json(R"json({"songs":[{"id":"AbC123","title":"Kesariya","more_info":{
        "encrypted_media_url":"E1","320kbps":"true","duration":"268",
        "artistMap":{"primary_artists":[{"name":"Arijit Singh"}]}}}]})json"), QStringLiteral("AbC123"), &details);
    t.check(ok && details.encryptedUrl == QStringLiteral("E1") && details.has320 && details.durationSec == 268
                && details.artists == QStringList{ QStringLiteral("Arijit Singh") },
            QStringLiteral("details as {\"songs\": [...]}"));
    ok = Saavn::rowFromDetails(json(R"json({"AbC123":{"id":"AbC123","song":"Tum Hi Ho","primary_artists":"Mithoon, Arijit Singh",
        "duration":"262","320kbps":"true","encrypted_media_url":"E2"}})json"), QStringLiteral("AbC123"), &details);
    t.check(ok && details.title == QStringLiteral("Tum Hi Ho") && details.encryptedUrl == QStringLiteral("E2")
                && details.artists == QStringList{ QStringLiteral("Mithoon"), QStringLiteral("Arijit Singh") }
                && details.has320 && details.durationSec == 262,
            QStringLiteral("details as {\"<id>\": {...}}, with the older flat fields"));
    ok = Saavn::rowFromDetails(json(R"json({"songs":[{"id":"Other","title":"Not this","more_info":{"encrypted_media_url":"E3"}}]})json"),
                               QStringLiteral("AbC123"), &details);
    t.check(!ok, QStringLiteral("details for another id are never taken for this one"));
    ok = Saavn::rowFromDetails(json(R"json({"Other":{"id":"Other","song":"Not this"}})json"), QStringLiteral("AbC123"), &details);
    t.check(!ok, QStringLiteral("nor in the keyed shape"));

    // Queries: the title, its version, the lead artist.
    const auto query = [](const char *title, const char *artist) {
        return Saavn::searchQuery(song(QString::fromUtf8(title), QString::fromUtf8(artist), 0));
    };
    QString q = query("Get Lucky (feat. Pharrell Williams and Nile Rodgers)", "Daft Punk, Pharrell Williams & Nile Rodgers");
    t.check(q == QStringLiteral("get lucky daft punk"), QStringLiteral("query: credits out, lead artist in"), q);
    q = query("Dancing with a Stranger", "Sam Smith & Normani");
    t.check(q == QStringLiteral("dancing with a stranger sam smith and normani"),
            QStringLiteral("query: a bare \"with\" stays in the title, \"&\" is spelt \"and\""), q);
    q = query("Yellow - Live in Buenos Aires", "Coldplay");
    t.check(q == QStringLiteral("yellow live coldplay"), QStringLiteral("query: the version is searched for too"), q);
    q = query("Simon & Garfunkel - The Boxer", "Simon & Garfunkel");
    t.check(q == QStringLiteral("the boxer simon and garfunkel"),
            QStringLiteral("query: an \"Artist - \" head is not part of the title"), q);
}

void testMatcher(Checks &t)
{
    struct Case {
        const char *what;
        Saavn::Target target;
        Saavn::Row row;
        bool expect;
    };
    const QString devanagariTitle = QString::fromUtf8("\xE0\xA4\x95\xE0\xA5\x87\xE0\xA4\xB8\xE0\xA4\xB0\xE0\xA4\xBF"
                                                      "\xE0\xA4\xAF\xE0\xA4\xBE");   // Kesariya
    const QString devanagariArtist = QString::fromUtf8("\xE0\xA4\x85\xE0\xA4\xB0\xE0\xA4\xBF\xE0\xA4\x9C\xE0\xA5\x80"
                                                       "\xE0\xA4\xA4 \xE0\xA4\xB8\xE0\xA4\xBF\xE0\xA4\x82\xE0\xA4\xB9");
    const QList<Case> cases = {
        { "same song, composer credited first on JioSaavn",
          song("Tum Hi Ho", "Arijit Singh", 262), row("Tum Hi Ho", { "Mithoon", "Arijit Singh" }, 262), true },
        { "same song, entities and a (From \"film\") aside",
          song("Tum Hi Ho", "Arijit Singh", 262),
          row("Tum Hi Ho (From &quot;Aashiqui 2&quot;)", { "Arijit Singh", "Mithoon" }, 261), true },
        { "remaster on our side",
          song("Bohemian Rhapsody (Remastered 2011)", "Queen", 355), row("Bohemian Rhapsody", { "Queen" }, 355), true },
        { "remaster on theirs, after a dash",
          song("Bohemian Rhapsody", "Queen", 355), row("Bohemian Rhapsody - Remastered 2011", { "Queen" }, 354), true },
        { "live row refused for a studio song",
          song("Yellow", "Coldplay", 269), row("Yellow (Live)", { "Coldplay" }, 270), false },
        { "studio row refused for a live song",
          song("Yellow - Live in Buenos Aires", "Coldplay", 300), row("Yellow", { "Coldplay" }, 300), false },
        { "two live takes from different places refused",
          song("Yellow (Live at Glastonbury)", "Coldplay", 280),
          row("Yellow (Live in Buenos Aires)", { "Coldplay" }, 280), false },
        { "remix refused for the original",
          song("Blinding Lights", "The Weeknd", 200),
          row("Blinding Lights (Chromatics Remix)", { "The Weeknd", "Chromatics" }, 200), false },
        { "the same remix, written two ways",
          song("Blinding Lights (Chromatics Remix)", "The Weeknd", 200),
          row("Blinding Lights - Chromatics Remix", { "The Weeknd" }, 201), true },
        { "another remixer's remix refused",
          song("Blinding Lights (Chromatics Remix)", "The Weeknd", 200),
          row("Blinding Lights (Major Lazer Remix)", { "The Weeknd" }, 200), false },
        { "wrong artist refused",
          song("Hallelujah", "Jeff Buckley", 413), row("Hallelujah", { "Leonard Cohen" }, 413), false },
        { "2 s off accepted",
          song("Smells Like Teen Spirit", "Nirvana", 302), row("Smells Like Teen Spirit", { "Nirvana" }, 300), true },
        { "3 s off accepted",
          song("Smells Like Teen Spirit", "Nirvana", 302), row("Smells Like Teen Spirit", { "Nirvana" }, 305), true },
        { "5 s off refused",
          song("Smells Like Teen Spirit", "Nirvana", 302), row("Smells Like Teen Spirit", { "Nirvana" }, 307), false },
        { "feat. credits in our title and artist line",
          song("Get Lucky (feat. Pharrell Williams and Nile Rodgers)", "Daft Punk, Pharrell Williams & Nile Rodgers", 369),
          row("Get Lucky", { "Daft Punk", "Pharrell Williams", "Nile Rodgers" }, 369), true },
        { "feat. credits only in their title",
          song("Get Lucky", "Daft Punk", 369),
          row("Get Lucky (feat. Pharrell Williams & Nile Rodgers)", { "Daft Punk" }, 369), true },
        { "\"with\" inside brackets is a credit",
          song("Stay", "The Kid LAROI & Justin Bieber", 141),
          row("Stay (with Justin Bieber)", { "The Kid LAROI" }, 141), true },
        { "a bare \"with\" is part of the title (S7)",
          song("Dancing with a Stranger", "Sam Smith & Normani", 171),
          row("Dancing With A Stranger (with Normani)", { "Sam Smith", "Normani" }, 171), true },
        { "and is not cut off there (S7)",
          song("Dancing with a Stranger", "Sam Smith", 171), row("Dancing", { "Sam Smith" }, 171), false },
        { "\"&\" in a name",
          song("The Boxer", "Simon & Garfunkel", 308), row("The Boxer", { "Simon & Garfunkel" }, 308), true },
        { "\"and\" against \"&\" (S7)",
          song("The Boxer", "Simon and Garfunkel", 308), row("The Boxer", { "Simon & Garfunkel" }, 308), true },
        { "\"Artist - Title\" video title",
          song("Simon & Garfunkel - The Boxer", "Simon & Garfunkel", 308),
          row("The Boxer", { "Simon & Garfunkel" }, 308), true },
        { "\"Simon & Garfunkel\" is not \"Paul Simon\"",
          song("The Boxer", "Simon & Garfunkel", 308), row("The Boxer", { "Paul Simon" }, 308), false },
        { "Devanagari title and artist on both sides",
          song(devanagariTitle, devanagariArtist, 268), row(devanagariTitle, { devanagariArtist }, 268), true },
        { "Devanagari title against its transliteration refused",
          song(devanagariTitle, "Arijit Singh", 268), row("Kesariya", { "Arijit Singh" }, 268), false },
        { "a cover by another artist refused",
          song("Kesariya", "Arijit Singh", 268), row("Kesariya (Cover)", { "Aditi Rao" }, 268), false },
        { "an unmarked cover by another artist refused",
          song("Kesariya", "Arijit Singh", 268), row("Kesariya", { "Jubin Nautiyal" }, 268), false },
        { "a cover credited to the same name still refused",
          song("Kesariya", "Arijit Singh", 268), row("Kesariya - Cover Version", { "Arijit Singh" }, 268), false },
        { "acoustic refused",
          song("Perfect", "Ed Sheeran", 263), row("Perfect (Acoustic)", { "Ed Sheeran" }, 263), false },
        { "sped up refused",
          song("Kesariya", "Arijit Singh", 268), row("Kesariya (Sped Up)", { "Arijit Singh" }, 268), false },
        { "slowed + reverb against slowed & reverb",
          song("Kesariya (Slowed + Reverb)", "Arijit Singh", 300),
          row("Kesariya (Slowed and Reverb)", { "Arijit Singh" }, 300), true },
        { "a style remix credited to the original artist refused",
          song("The Fate of Ophelia", "Taylor Swift", 227),
          row("The Fate of Ophelia (Garage)", { "Taylor Swift" }, 227), false },
        { "a song sung again in another language refused",
          song("Srivalli", "Sid Sriram", 224), row("Srivalli (Hindi)", { "Sid Sriram" }, 224), false },
        { "the same language version on both sides",
          song("Srivalli (Hindi)", "Javed Ali", 224), row("Srivalli (Hindi)", { "Javed Ali" }, 224), true },
        { "instrumental refused",
          song("Tum Hi Ho", "Mithoon", 262), row("Tum Hi Ho (Instrumental)", { "Mithoon" }, 262), false },
        { "karaoke refused",
          song("Tum Hi Ho", "Mithoon", 262), row("Tum Hi Ho (Karaoke Version)", { "Mithoon" }, 262), false },
        { "Taylor's Version refused for the original",
          song("Love Story", "Taylor Swift", 236),
          row(QString::fromUtf8("Love Story (Taylor\xE2\x80\x99s Version)"), { "Taylor Swift" }, 236), false },
        { "the same parts, written two ways",
          song("Shine On You Crazy Diamond (Pts. 1-5)", "Pink Floyd", 812),
          row("Shine On You Crazy Diamond, Pts. 1-5", { "Pink Floyd" }, 811), true },
        { "\"Part 1 - Part 5\" is \"Pts. 1-5\"",
          song("Shine On You Crazy Diamond (Pts. 1-5)", "Pink Floyd", 812),
          row("Shine On You Crazy Diamond (Part 1 - Part 5)", { "Pink Floyd" }, 812), true },
        { "other parts refused",
          song("Shine On You Crazy Diamond (Pts. 1-5)", "Pink Floyd", 812),
          row("Shine On You Crazy Diamond (Pts. 6-9)", { "Pink Floyd" }, 812), false },
        { "apostrophes",
          song("Don't Start Now", "Dua Lipa", 183), row("Dont Start Now", { "Dua Lipa" }, 183), true },
        { "accents",
          song("Halo", QString::fromUtf8("Beyonc\xC3\xA9"), 261), row("Halo", { "Beyonce" }, 261), true },
        { "Radio Edit is the same recording (the length decides)",
          song("Get Lucky (Radio Edit)", "Daft Punk", 248), row("Get Lucky", { "Daft Punk" }, 248), true },
        { "a video's title noise and a VEVO channel",
          song("Coldplay - Yellow (Official Video)", "ColdplayVEVO", 269), row("Yellow", { "Coldplay" }, 269), true },
        { "\"Official Video\" without brackets",
          song("Yellow Official Video", "Coldplay", 269), row("Yellow", { "Coldplay" }, 269), true },
        { "\"(Full Video Song)\" in brackets is noise too",
          song("Kesariya (Full Video Song)", "Arijit Singh", 268), row("Kesariya", { "Arijit Singh" }, 268), true },

        // Dubs. JioSaavn credits the composer, the lyricist and the Hindi
        // singer on every one, and they share the backing track, so neither
        // the artists nor the length tell them apart.
        { "a dub named inside the From aside refused for the original",
          song("Deva Deva", "Pritam, Arijit Singh & Jonita Gandhi", 279),
          row("Deva Deva (From &quot;Brahmastra (Telugu)&quot;)",
              { "Pritam", "Sreerama Chandra", "Arijit Singh", "Jonita Gandhi" }, 276), false },
        { "and the original refused for the dub named that way",
          song("Deva Deva (From \"Brahmastra (Telugu)\")", "Pritam, Sreerama Chandra & Jonita Gandhi", 276),
          row("Deva Deva", { "Pritam", "Arijit Singh", "Amitabh Bhattacharya", "Jonita Gandhi" }, 277), false },
        { "the dub named inside From is the dub named bare",
          song("Deva Deva (Telugu)", "Sreerama Chandra", 276),
          row("Deva Deva (From &quot;Brahmastra (Telugu)&quot;)",
              { "Pritam", "Sreerama Chandra", "Arijit Singh", "Jonita Gandhi" }, 276), true },
        { "a language our album names refuses a row JioSaavn lists in another",
          onAlbum(song("Deva Deva", "Pritam, Sreerama Chandra & Jonita Gandhi", 278),
                  "Brahmastra (Telugu) (Original Motion Picture Soundtrack)"),
          onAlbum(row("Deva Deva", { "Pritam", "Arijit Singh", "Amitabh Bhattacharya", "Jonita Gandhi" }, 279),
                  "Brahmastra (Original Motion Picture Soundtrack)", "hindi"), false },
        { "and takes one JioSaavn lists in that language",
          onAlbum(song("Deva Deva", "Pritam, Sreerama Chandra & Jonita Gandhi", 278),
                  "Brahmastra (Telugu) (Original Motion Picture Soundtrack)"),
          onAlbum(row("Deva Deva", { "Pritam", "Sreerama Chandra", "Arijit Singh", "Jonita Gandhi" }, 276),
                  "Brahmastra Part One Shiva", "telugu"), true },
        { "a row whose album names a language refused for a song that names none",
          song("Srivalli", "Sid Sriram", 224),
          onAlbum(row("Srivalli", { "Sid Sriram" }, 224), "Pushpa - The Rise (Tamil)", "tamil"), false },

        // Versions no marker knew.
        { "\"(Arijit Singh Version)\" refused for the original",
          song("Tum Hi Ho", "Arijit Singh", 262), row("Tum Hi Ho (Arijit Singh Version)", { "Arijit Singh" }, 262), false },
        { "\"(Film Version)\" refused for the album's",
          song("Deva Deva", "Pritam, Arijit Singh & Jonita Gandhi", 279),
          row("Deva Deva (Film Version)", { "Pritam", "Arijit Singh", "Jonita Gandhi" }, 279), false },
        { "\"(Version 2)\" refused",
          song("Deva Deva", "Pritam, Arijit Singh & Jonita Gandhi", 279),
          row("Deva Deva (Version 2)", { "Pritam", "Arijit Singh", "Jonita Gandhi" }, 279), false },
        { "\"(Re-Recording)\" by the same artist refused",
          song("Mr. Brightside", "The Killers", 222), row("Mr. Brightside (Re-Recording)", { "The Killers" }, 222), false },
        { "\"- Jhankar Beats\" refused",
          song("Pehla Nasha", "Udit Narayan & Sadhana Sargam", 291),
          row("Pehla Nasha - Jhankar Beats", { "Sadhana Sargam", "Udit Narayan" }, 291), false },
        { "\"(Synthwave)\" with the original artists credited refused",
          song("Deva Deva", "Pritam, Arijit Singh & Jonita Gandhi", 279),
          row("Deva Deva (Synthwave)", { "Amitabh Bhattacharya", "VDJ Fly", "Pritam", "Arijit Singh" }, 279), false },
        { "an alternate title the row does not have refused",
          song("Raabta (Kehte Hain Khuda)", "Pritam, Arijit Singh & Shreya Ghoshal", 243),
          row("Raabta (From &quot;Agent Vinod&quot;)", { "Arijit Singh", "Pritam", "Amitabh Bhattacharya" }, 243), false },
        { "a bare year the song does not have refused",
          song("Ek Do Teen", "Alka Yagnik", 300), row("Ek Do Teen (2018)", { "Alka Yagnik" }, 300), false },
        { "an aside the other side names elsewhere (its album) is fine",
          song("Tum Hi Ho (Aashiqui 2)", "Arijit Singh", 262),
          onAlbum(row("Tum Hi Ho", { "Mithoon", "Arijit Singh" }, 262), "Aashiqui 2"), true },
        { "a dash tail that is the artist is fine",
          song("Yellow - Coldplay", "Coldplay", 269), row("Yellow", { "Coldplay" }, 269), true },
        { "a dash inside the From quotes is not a split",
          song("Srivalli", "Sid Sriram", 224),
          row("Srivalli - From &quot;Pushpa - The Rise&quot;", { "Sid Sriram" }, 224), true },

        // Whose remix, which concert, named on one side only.
        { "someone's remix refused for a remix that names nobody",
          song("Pehla Nasha - Remix", "Udit Narayan & Sadhana Sargam", 250),
          row("Pehla Nasha (DJ Chetas Remix)", { "Udit Narayan", "Sadhana Sargam" }, 250), false },
        { "a concert refused for a live song from another album",
          onAlbum(song("Yellow (Live)", "Coldplay", 280), "Live 2003"),
          row("Yellow - Live in Buenos Aires", { "Coldplay" }, 280), false },
        { "the same concert, where our album names it",
          onAlbum(song("Yellow (Live)", "Coldplay", 280), "Live in Buenos Aires"),
          row("Yellow - Live in Buenos Aires", { "Coldplay" }, 280), true },
        { "a remixer credited among our artists is enough",
          song("Blinding Lights (Remix)", "The Weeknd, Chromatics", 200),
          row("Blinding Lights (Chromatics Remix)", { "The Weeknd" }, 200), true },

        { "a row with no length refused when ours is known",
          song("Tum Hi Ho", "Arijit Singh", 262), row("Tum Hi Ho", { "Arijit Singh" }, 0), false },
    };
    for (const Case &c : cases) {
        const Saavn::Judgement judgement = Saavn::judge(c.target, c.row);
        // A refusal says why, so a refusal for the wrong reason shows.
        QString what = QStringLiteral("match: ") + QString::fromLatin1(c.what);
        if (!judgement.accepted && !c.expect)
            what += QStringLiteral(" [") + judgement.reason + QLatin1Char(']');
        t.check(judgement.accepted == c.expect, what,
                judgement.accepted ? QStringLiteral("accepted") : QStringLiteral("refused: ") + judgement.reason);
    }

    // Ties between rows that all fit.
    const Saavn::Target tum = song("Tum Hi Ho", "Arijit Singh", 262);
    Saavn::Choice choice = Saavn::choose(tum, { row("Tum Hi Ho", { "Arijit Singh" }, 262, false),
                                                row("Tum Hi Ho", { "Arijit Singh" }, 262, true) });
    t.check(choice.accepted.value(0, -1) == 1, QStringLiteral("choose: the explicit row before the clean one"));
    choice = Saavn::choose(tum, { row("Tum Hi Ho", { "Arijit Singh" }, 262, false, false),
                                  row("Tum Hi Ho", { "Arijit Singh" }, 262, false, true) });
    t.check(choice.accepted.value(0, -1) == 1, QStringLiteral("choose: then the row offered at 320 kbps"));
    choice = Saavn::choose(tum, { row("Tum Hi Ho", { "Arijit Singh" }, 264), row("Tum Hi Ho", { "Arijit Singh" }, 263),
                                  row("Tum Hi Ho", { "Arijit Singh" }, 262) });
    t.check(choice.accepted == QList<int>{ 2, 1, 0 }, QStringLiteral("choose: then the closest in length"));
    choice = Saavn::choose(tum, { row("Tum Hi Ho", { "Arijit Singh" }, 264, true, false),
                                  row("Tum Hi Ho", { "Arijit Singh" }, 262, false, true) });
    t.check(choice.accepted.value(0, -1) == 0,
            QStringLiteral("choose: explicit outranks 320 kbps and length, in that order"));

    const Saavn::Target unknown = song("Tum Hi Ho", "Arijit Singh", 0);
    choice = Saavn::choose(unknown, { row("Tum Hi Ho", { "Arijit Singh" }, 262), row("Tum Hi Ho", { "Arijit Singh" }, 250) });
    t.check(choice.accepted.isEmpty(), QStringLiteral("choose: length unknown and two rows fit: neither is taken"),
            choice.reason);
    choice = Saavn::choose(unknown, { row("Tum Hi Ho", { "Arijit Singh" }, 262), row("Tum Hi Ho (Live)", { "Arijit Singh" }, 400) });
    t.check(choice.accepted == QList<int>{ 0 }, QStringLiteral("choose: length unknown and one row fits: it is taken"));
    choice = Saavn::choose(unknown, { row("Tum Hi Ho", { "Arijit Singh" }, 262, false, true, QStringLiteral("same")),
                                      row("Tum Hi Ho", { "Arijit Singh" }, 262, false, true, QStringLiteral("same")) });
    t.check(choice.accepted.size() == 1, QStringLiteral("choose: the same id listed twice counts once"));
    choice = Saavn::choose(tum, {});
    t.check(choice.accepted.isEmpty() && !choice.reason.isEmpty(), QStringLiteral("choose: no rows, no match, a reason"));
    choice = Saavn::choose(tum, { row("Tum Hi Ho (Live)", { "Arijit Singh" }, 262), row("Tum Hi Ho", { "Arijit Singh" }, 262) });
    t.check(choice.accepted == QList<int>{ 1 } && choice.refusals.size() == 1,
            QStringLiteral("choose: the refused row is listed with its reason"), choice.refusals.join(QStringLiteral(" | ")));
    choice = Saavn::choose(unknown, { row("Tum Hi Ho", { "Arijit Singh" }, 0) });
    t.check(choice.accepted == QList<int>{ 0 },
            QStringLiteral("choose: no length on either side and one row fits: it is taken"), choice.reason);

    // The dub, as JioSaavn lists it: the Hindi row credits all but the
    // Telugu singer, the Telugu row is refused for its language.
    const QList<Saavn::Row> deva = {
        onAlbum(row("Deva Deva", { "Pritam", "Arijit Singh", "Amitabh Bhattacharya", "Jonita Gandhi" }, 279),
                "Brahmastra (Original Motion Picture Soundtrack)", "hindi"),
        onAlbum(row("Deva Deva (From &quot;Brahmastra (Telugu)&quot;)",
                    { "Pritam", "Sreerama Chandra", "Arijit Singh", "Jonita Gandhi" }, 276),
                "Deva Deva (From &quot;Brahmastra (Telugu)&quot;)", "telugu"),
    };
    choice = Saavn::choose(song("Deva Deva", "Pritam, Sreerama Chandra & Jonita Gandhi", 278), deva);
    t.check(choice.accepted.isEmpty(),
            QStringLiteral("choose: a Telugu singer's \"Deva Deva\" takes neither the Hindi row nor the Telugu one"),
            choice.refusals.join(QStringLiteral(" | ")));
    choice = Saavn::choose(song("Deva Deva", "Pritam, Arijit Singh & Jonita Gandhi", 279), deva);
    t.check(choice.accepted == QList<int>{ 0 },
            QStringLiteral("choose: the Hindi singers' \"Deva Deva\" takes the Hindi row"),
            choice.refusals.join(QStringLiteral(" | ")));
    choice = Saavn::choose(song("Deva Deva (From \"Brahmastra (Telugu)\")", "Pritam, Sreerama Chandra & Jonita Gandhi", 276), deva);
    t.check(choice.accepted == QList<int>{ 1 },
            QStringLiteral("choose: the Telugu \"Deva Deva\" named as such takes the Telugu row"),
            choice.refusals.join(QStringLiteral(" | ")));

    // Two rows that fit, in two languages, neither named in its title.
    const QList<Saavn::Row> twoLanguages = {
        onAlbum(row("Deva Deva", { "Pritam", "Arijit Singh", "Jonita Gandhi" }, 279), "Brahmastra", "hindi"),
        onAlbum(row("Deva Deva", { "Pritam", "Arijit Singh", "Jonita Gandhi" }, 277), "Brahmastra Part One", "telugu"),
    };
    choice = Saavn::choose(song("Deva Deva", "Pritam, Arijit Singh & Jonita Gandhi", 278), twoLanguages);
    t.check(choice.accepted.isEmpty(), QStringLiteral("choose: rows in two languages that fit: neither is taken"),
            choice.reason);
    choice = Saavn::choose(onAlbum(song("Deva Deva", "Pritam, Arijit Singh & Jonita Gandhi", 278), "Brahmastra Part One"),
                           twoLanguages);
    t.check(choice.accepted == QList<int>{ 1 }, QStringLiteral("choose: unless our album names one of them"),
            choice.reason);

    // Two albums: the soundtrack and a compilation, credited a little
    // differently, as nearly every well-known song is.
    const Saavn::Target pehla = song("Pehla Nasha", "Udit Narayan & Sadhana Sargam", 291);
    choice = Saavn::choose(pehla, { onAlbum(row("Pehla Nasha", { "Udit Narayan", "Sadhana Sargam" }, 291, false, false),
                                            "Jo Jeeta Wohi Sikandar", "hindi"),
                                    onAlbum(row("Pehla Nasha", { "Jatin-Lalit", "Udit Narayan", "Sadhana Sargam" }, 292,
                                                false, true),
                                            "90s Love Songs", "hindi") });
    t.check(choice.accepted == QList<int>{ 1, 0 },
            QStringLiteral("choose: a soundtrack and a compilation in one language, no album of ours: both, 320 first"),
            choice.reason);
    choice = Saavn::choose(onAlbum(pehla, "Jo Jeeta Wohi Sikandar (Original Motion Picture Soundtrack)"),
                           { onAlbum(row("Pehla Nasha", { "Udit Narayan", "Sadhana Sargam" }, 291, false, false),
                                     "Jo Jeeta Wohi Sikandar"),
                             onAlbum(row("Pehla Nasha", { "Jatin-Lalit", "Udit Narayan", "Sadhana Sargam" }, 291, false, true),
                                     "90s Love Songs") });
    t.check(choice.accepted == QList<int>{ 0 },
            QStringLiteral("choose: our album picks its own row, 320 kbps elsewhere or not"), choice.reason);

    // Credits: a row lacking one of our names another listing credits.
    choice = Saavn::choose(song("Tum Hi Ho", "Mithoon & Arijit Singh", 262),
                           { row("Tum Hi Ho", { "Arijit Singh" }, 262, true, true),
                             row("Tum Hi Ho", { "Mithoon", "Arijit Singh" }, 262, false, false) });
    t.check(choice.accepted == QList<int>{ 1 },
            QStringLiteral("choose: the row crediting all our names, over an explicit 320 kbps one that does not"),
            choice.refusals.join(QStringLiteral(" | ")));
}

// How far apart two copies are (AudioAlign::bestLag), on invented sound: a
// noise with a few tones in it, at 4 kHz, the reference 4 s of it and the
// other copy the same sound shifted and a little quieter, with its own hiss,
// searched 3 s either way.
void testAlign(Checks &t)
{
    static constexpr int rate = 4000;
    static constexpr int range = 3 * rate;
    static constexpr double kPi = 3.14159265358979323846;
    const auto sound = [](int length, quint32 seed) {
        QVector<float> out(length);
        quint32 state = seed;
        for (int i = 0; i < length; ++i) {
            state = state * 1664525u + 1013904223u;
            const float noise = float(int(state >> 16) - 32768) / 32768.0f;
            out[i] = 3000.0f * noise + 2000.0f * float(std::sin(2.0 * kPi * 220.0 * i / rate))
                     + 1500.0f * float(std::sin(2.0 * kPi * 331.0 * i / rate + 0.3));
        }
        return out;
    };
    // The song as a whole, 12 s; the reference is 4 s of it from 4 s in.
    const QVector<float> song = sound(12 * rate, 7u);
    const QVector<float> reference = song.mid(4 * rate, 4 * rate);
    for (const double shiftMs : { 0.0, 137.75, -16.5, 840.0, -2500.0 }) {
        // The other copy's window starts `range` before the reference's, and
        // its music `shiftMs` later.
        const int shift = int(std::lround(shiftMs * rate / 1000.0));
        QVector<float> other(4 * rate + 2 * range);
        quint32 hiss = 99u;
        for (int i = 0; i < other.size(); ++i) {
            const int source = 4 * rate - range + i - shift;
            hiss = hiss * 1664525u + 1013904223u;
            const float noise = 60.0f * float(int(hiss >> 16) - 32768) / 32768.0f;
            other[i] = (source >= 0 && source < song.size() ? 0.8f * song[source] : 0.0f) + noise;
        }
        double offset = 0.0;
        double peak = 0.0;
        const bool ok = AudioAlign::bestLag(reference, other, rate, range, &offset, &peak);
        t.check(ok && qAbs(offset - shift * 1000.0 / rate) < 0.3 && peak > 0.95,
                QStringLiteral("align: the music %1 ms later in the other copy is found to a quarter of a ms")
                    .arg(shiftMs),
                QStringLiteral("found %1 ms, %2 alike").arg(offset).arg(peak));
    }
    // Two sounds that are not the same recording.
    const QVector<float> stranger = sound(4 * rate + 2 * range, 12345u);
    double offset = 0.0;
    double peak = 0.0;
    const bool ok = AudioAlign::bestLag(reference, stranger, rate, range, &offset, &peak);
    t.check(ok && peak < 0.8, QStringLiteral("align: a different sound is not taken for the same (below 0.8 alike)"),
            QStringLiteral("%1 alike at %2 ms").arg(peak).arg(offset));
    // Too little of the other to search.
    t.check(!AudioAlign::bestLag(reference, reference, rate, range, &offset, &peak),
            QStringLiteral("align: an other copy too short to search is refused, not guessed at"));
}

} // namespace

int runSaavnSelfTest()
{
    Checks t;
    testDes(t);
    testLinks(t);
    testBitrates(t);
    testParsing(t);
    testMatcher(t);
    testAlign(t);
    return t.finish();
}

int runSaavnLookup(const QString &title, const QString &artist, qint64 durationMs, bool indiaHeaders)
{
    JioSaavn client;
    client.setIndiaHeaders(indiaHeaders);
    Saavn::Target target;
    target.title = title;
    target.artist = artist;
    target.durationMs = durationMs;

    qWarning("saavn: \"%s\" by %s, %s; Indian headers %s; searching \"%s\"", qPrintable(title), qPrintable(artist),
             durationMs > 0 ? qPrintable(QStringLiteral("%1 s").arg(durationMs / 1000)) : "length unknown",
             indiaHeaders ? "on" : "off", qPrintable(Saavn::searchQuery(target)));

    QEventLoop loop;
    JioSaavn::Result answer;
    client.lookup(target, [&answer, &loop](const JioSaavn::Result &result) {
        answer = result;
        loop.quit();
    });
    loop.exec();

    qWarning("saavn: %d rows in %lld ms", answer.rows, static_cast<long long>(answer.elapsedMs));
    for (const QString &refused : std::as_const(answer.refusals))
        qWarning("saavn:   refused  %s", qPrintable(refused));

    switch (answer.kind) {
    case JioSaavn::Result::Match: {
        // The host and the rendition, never the whole link.
        const QUrl url(answer.url);
        static const QRegularExpression rendition(QStringLiteral("_\\d+\\.\\w+$"));
        const QString tail = rendition.match(url.path()).captured(0);
        qWarning("saavn: MATCH \"%s\" by %s (%s, %d s), %s from %s (...%s)", qPrintable(answer.title),
                 qPrintable(answer.artists), qPrintable(answer.saavnId), answer.durationSec,
                 answer.kbps > 0 ? qPrintable(QStringLiteral("%1 kbps").arg(answer.kbps)) : "bitrate not stated",
                 qPrintable(url.host()), qPrintable(tail.isEmpty() ? QStringLiteral("no rendition in the path") : tail));
        return 0;
    }
    case JioSaavn::Result::NoMatch:
        qWarning("saavn: NO MATCH: %s", qPrintable(answer.reason));
        return 1;
    case JioSaavn::Result::Failed:
        break;
    }
    qWarning("saavn: FAILED: %s", qPrintable(answer.reason));
    return 2;
}
