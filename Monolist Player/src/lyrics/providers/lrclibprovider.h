#pragma once

#include "lyrics/lyricsprovider.h"

class QNetworkAccessManager;

// LRCLIB (lrclib.net), a free, open database of time-synced lyrics, matched
// by title, artist, album and length so the timing fits this recording.
//
// First its exact lookup (/api/get); on a 404, or when that has not answered
// 250 ms after it was sent, its search, the exact answer still winning if it
// comes within a second of being sent; the search's entries are checked
// against the song (LyricsQuery::match) and the closest in length is taken.
// An entry 3-10 s off is kept as another recording's words. One deadline for
// all of it, 6 s on the wall clock. Every lookup logs one line:
// "lyrics: LRCLIB <outcome> in N ms for <id>: <each request>; <bytes> bytes".
class LrclibProvider : public LyricsProvider
{
public:
    explicit LrclibProvider(QNetworkAccessManager *network);

    // LRCLIB is open source and can be run anywhere; this is where to ask.
    void setUrl(const QString &url);
    // On (the default): the exact lookup first, and the search's entries
    // checked against the song. Off (lyrics.lrclib=search): its search
    // alone, taken by length, as before LY-2.
    void setExact(bool on) { m_exact = on; }
    bool exact() const { return m_exact; }

    QString id() const override { return QStringLiteral("lrclib"); }
    QString name() const override { return QStringLiteral("LRCLIB"); }
    Timing bestTiming() const override { return Timing::Line; }
    bool background() const override { return true; }
    LyricsLookup *lookUp(const LyricsRequest &request, QObject *parent) override;

private:
    QNetworkAccessManager *m_network;
    QString m_url = QStringLiteral("https://lrclib.net");
    bool m_exact = true;
};
