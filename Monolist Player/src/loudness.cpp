#include "loudness.h"
#include "appdatabase.h"

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QSqlQuery>
#include <QtMath>

namespace {

// YouTube's reference is -14 LUFS: perceptualLoudnessDb -9 is a song 5 dB
// louder than it.
constexpr double kPerceptualOffset = 14.0;
constexpr double kMaxCutDb = 15.0;

QHash<QString, double> &known()
{
    static QHash<QString, double> values;
    return values;
}

QJsonObject object(const QJsonObject &from, const char *key)
{
    return from.value(QLatin1String(key)).toObject();
}

} // namespace

double Loudness::fromAnswer(double formatDb, const QJsonObject &player)
{
    if (!qIsNaN(formatDb))
        return formatDb;
    const QJsonObject audio = object(object(player, "playerConfig"), "audioConfig");
    const QJsonValue loudness = audio.value(QStringLiteral("loudnessDb"));
    if (loudness.isDouble())
        return loudness.toDouble();
    const QJsonValue perceptual = audio.value(QStringLiteral("perceptualLoudnessDb"));
    if (perceptual.isDouble())
        return perceptual.toDouble() + kPerceptualOffset;
    return qQNaN();
}

void Loudness::remember(const QString &videoId, double db)
{
    if (videoId.isEmpty() || qIsNaN(db) || qIsInf(db))
        return;
    auto it = known().find(videoId);
    if (it != known().end() && qFuzzyCompare(*it + 100.0, db + 100.0))
        return;
    known().insert(videoId, db);
    QSqlQuery query(AppDatabase::connection());
    query.prepare(QStringLiteral("INSERT OR REPLACE INTO loudness (video_id, db, measured_at) VALUES (?, ?, ?)"));
    query.addBindValue(videoId);
    query.addBindValue(db);
    query.addBindValue(QDateTime::currentSecsSinceEpoch());
    query.exec();
}

double Loudness::of(const QString &videoId)
{
    if (videoId.isEmpty())
        return qQNaN();
    const auto it = known().constFind(videoId);
    if (it != known().constEnd())
        return *it;
    QSqlQuery query(AppDatabase::connection());
    query.prepare(QStringLiteral("SELECT db FROM loudness WHERE video_id = ?"));
    query.addBindValue(videoId);
    if (query.exec() && query.next()) {
        const double db = query.value(0).toDouble();
        known().insert(videoId, db);
        return db;
    }
    return qQNaN();
}

double Loudness::gainFor(double db)
{
    if (qIsNaN(db) || qIsInf(db))
        return 0.0;
    return qBound(-kMaxCutDb, -db, 0.0);
}
