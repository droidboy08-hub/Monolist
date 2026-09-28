#include "ytmlyricsprovider.h"

#include "innertube.h"

#include <QPointer>

namespace {

class YtmLookup : public LyricsLookup
{
public:
    YtmLookup(InnerTube *innerTube, const QString &videoId, QObject *parent)
        : LyricsLookup(parent)
        , m_innerTube(innerTube)
        , m_videoId(videoId)
    {
    }

    void start() override
    {
        const QPointer<YtmLookup> self(this);
        m_callOff = m_innerTube->lyrics(m_videoId, [self](const QString &text, const QString &source,
                                                          const QString &error) {
            if (!self || self->m_done)
                return;
            self->m_done = true;
            self->m_callOff = nullptr;
            LyricsOutcome outcome;
            if (!text.isEmpty()) {
                outcome.kind = LyricsOutcome::Found;
                outcome.answer.plain = text;
                outcome.answer.source = source.isEmpty() ? QStringLiteral("YouTube Music")
                                                         : source + QStringLiteral(" via YouTube Music");
            } else if (!error.isEmpty()) {
                outcome.kind = LyricsOutcome::Failed;
                outcome.error = QStringLiteral("YouTube Music: ") + error;
            } else {
                outcome.kind = LyricsOutcome::Missed;
            }
            self->finish(outcome);
        });
    }

    void cancel() override
    {
        if (m_done)
            return;
        m_done = true;
        onFinished = nullptr;
        if (auto callOff = std::exchange(m_callOff, nullptr))
            callOff();
    }

private:
    InnerTube *m_innerTube;
    const QString m_videoId;
    std::function<void()> m_callOff;
    bool m_done = false;
};

} // namespace

YtmLyricsProvider::YtmLyricsProvider(InnerTube *innerTube)
    : m_innerTube(innerTube)
{
}

LyricsLookup *YtmLyricsProvider::lookUp(const LyricsRequest &request, QObject *parent)
{
    return new YtmLookup(m_innerTube, request.videoId, parent);
}
