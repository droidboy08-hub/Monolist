#include "systempip.h"

// No system picture in picture here (Windows, Linux): the app's own small
// panel, MiniVideo, is used instead, and none of this is ever asked for.
struct SystemPip::Private {};

SystemPip::SystemPip(PlaybackController *player, MpvEngine *engine, QObject *parent)
    : QObject(parent)
    , m_player(player)
    , m_engine(engine)
{
}

SystemPip::~SystemPip() = default;

bool SystemPip::supported() const
{
    return false;
}

void SystemPip::start()
{
    Q_EMIT failed(QStringLiteral("Picture in picture is not available here"));
}

void SystemPip::stop() {}

QString SystemPip::diagnostics() const
{
    return QStringLiteral("not supported");
}

void SystemPip::setActive(bool active)
{
    if (active == m_active)
        return;
    m_active = active;
    Q_EMIT activeChanged();
}
