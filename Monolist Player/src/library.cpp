#include "library.h"
#include "appdatabase.h"

#include <QSqlQuery>
#include <QVariant>

Library::Library(QObject *parent)
    : QObject(parent) {}

void Library::load()
{
    m_playlists.reload();
    m_albums.reload();
    m_tracks.reload();
    reloadFeatured();
}

// The poster statement is content, not layout: it lives in the featured
// table so a release can be swapped without touching QML.
void Library::reloadFeatured()
{
    QSqlQuery q(AppDatabase::connection());
    q.exec(QStringLiteral("SELECT kicker, title_line1, title_line2, meta FROM featured"
                          " WHERE active = 1 ORDER BY id DESC LIMIT 1"));
    QVariantMap featured;
    if (q.next()) {
        featured.insert(QStringLiteral("kicker"), q.value(0).toString());
        featured.insert(QStringLiteral("titleLine1"), q.value(1).toString());
        featured.insert(QStringLiteral("titleLine2"), q.value(2).toString());
        featured.insert(QStringLiteral("meta"), q.value(3).toString());
    }
    m_featured = featured;
    Q_EMIT featuredChanged();
}

QString Library::userName() const
{
    return settingValue(QStringLiteral("user.name"), QStringLiteral("J. Krause"));
}

QString Library::userInitials() const
{
    const QStringList parts = userName().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QString initials;
    for (const QString &part : parts)
        initials.append(part.left(1).toUpper());
    return initials.left(2);
}

QString Library::userPlan() const
{
    return settingValue(QStringLiteral("user.plan"), QStringLiteral("PREMIUM"));
}

QString Library::settingValue(const QString &key, const QString &fallback) const
{
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral("SELECT value FROM settings WHERE key = ?"));
    q.addBindValue(key);
    if (q.exec() && q.next())
        return q.value(0).toString();
    return fallback;
}

void Library::setSetting(const QString &key, const QString &value)
{
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral("INSERT INTO settings (key, value) VALUES (?, ?)"
                             " ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
    q.addBindValue(key);
    q.addBindValue(value);
    q.exec();
}
