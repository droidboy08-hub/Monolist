#pragma once

#include "lyrics/lyricsprovider.h"

class InnerTube;

// YouTube Music's own lyrics: plain text from its partners (Musixmatch,
// LyricFind), credited as such, for the video itself, so never another
// song's. Two requests (InnerTube::lyrics), called off together.
class YtmLyricsProvider : public LyricsProvider
{
public:
    explicit YtmLyricsProvider(InnerTube *innerTube);

    QString id() const override { return QStringLiteral("ytmusic"); }
    QString name() const override { return QStringLiteral("YouTube Music"); }
    Timing bestTiming() const override { return Timing::Plain; }
    bool background() const override { return true; }
    LyricsLookup *lookUp(const LyricsRequest &request, QObject *parent) override;

private:
    InnerTube *m_innerTube;
};
