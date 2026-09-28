#include "lyricsstore.h"

#include "appdatabase.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace {

// How long "none found" stands before a song is looked up again, and found
// lyrics before they are checked again behind the ones shown.
const QString kAskAgainAfter = QStringLiteral("-3 days");
const QString kCheckAgainAfter = QStringLiteral("-60 days");

} // namespace

namespace LyricsStore {

Stored read(const QString &videoId, LyricsAnswer *answer)
{
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral(
        "SELECT synced, plain, source, provisional,"
        " fetched_at > datetime('now', ?), fetched_at > datetime('now', ?)"
        " FROM lyrics WHERE video_id = ?"));
    q.addBindValue(kAskAgainAfter);
    q.addBindValue(kCheckAgainAfter);
    q.addBindValue(videoId);
    if (!q.exec() || !q.next())
        return Stored::Nothing;
    *answer = LyricsAnswer();
    answer->synced = q.value(0).toString();
    answer->plain = q.value(1).toString();
    answer->source = q.value(2).toString();
    answer->instrumental = answer->source == QLatin1String("instrumental");
    if (answer->instrumental)
        answer->source.clear();
    answer->provisional = q.value(3).toBool();
    const bool recent = q.value(4).toBool();
    const bool fresh = q.value(5).toBool();
    if (answer->synced.isEmpty() && answer->plain.isEmpty() && !answer->instrumental)
        return recent ? Stored::Show : Stored::Nothing;   // none were found; long enough ago, look again
    return answer->provisional || !fresh ? Stored::ShowAndCheck : Stored::Show;
}

void write(const QString &videoId, const LyricsAnswer &answer)
{
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral(
        "INSERT INTO lyrics (video_id, synced, plain, source, provisional, fetched_at)"
        " VALUES (?, ?, ?, ?, ?, datetime('now'))"
        " ON CONFLICT(video_id) DO UPDATE SET synced = excluded.synced, plain = excluded.plain,"
        " source = excluded.source, provisional = excluded.provisional, fetched_at = excluded.fetched_at"));
    q.addBindValue(videoId);
    q.addBindValue(AppDatabase::text(answer.synced));
    q.addBindValue(AppDatabase::text(answer.plain));
    q.addBindValue(answer.instrumental ? QStringLiteral("instrumental") : AppDatabase::text(answer.source));
    q.addBindValue(answer.provisional ? 1 : 0);
    if (!q.exec())
        qWarning("Monolist: could not keep lyrics: %s", qPrintable(q.lastError().text()));
}

void touch(const QString &videoId)
{
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral("UPDATE lyrics SET fetched_at = datetime('now'), provisional = 0 WHERE video_id = ?"));
    q.addBindValue(videoId);
    q.exec();
}

QHash<QString, LyricsOutcome> results(const QString &videoId)
{
    QHash<QString, LyricsOutcome> fresh;
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral(
        "SELECT provider, synced, plain, source, instrumental, loose, duration,"
        " fetched_at > datetime('now', ?), fetched_at > datetime('now', ?)"
        " FROM lyrics_results WHERE video_id = ?"));
    q.addBindValue(kAskAgainAfter);
    q.addBindValue(kCheckAgainAfter);
    q.addBindValue(videoId);
    if (!q.exec())
        return fresh;
    while (q.next()) {
        LyricsOutcome outcome;
        LyricsAnswer &answer = outcome.answer;
        answer.synced = q.value(1).toString();
        answer.plain = q.value(2).toString();
        answer.source = q.value(3).toString();
        answer.instrumental = q.value(4).toBool();
        answer.loose = q.value(5).toBool();
        answer.durationS = q.value(6).toDouble();
        const bool found = answer.found();
        if (!(found ? q.value(8).toBool() : q.value(7).toBool()))
            continue;   // too old: that provider is asked again
        outcome.kind = found ? LyricsOutcome::Found : LyricsOutcome::Missed;
        if (!found)
            outcome.answer = LyricsAnswer();
        fresh.insert(q.value(0).toString(), outcome);
    }
    return fresh;
}

void writeResult(const QString &videoId, const QString &provider, const LyricsOutcome &outcome)
{
    if (outcome.kind == LyricsOutcome::Failed || outcome.partial)
        return;
    const LyricsAnswer none;
    const LyricsAnswer &answer = outcome.kind == LyricsOutcome::Found ? outcome.answer : none;
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral(
        "INSERT INTO lyrics_results (video_id, provider, synced, plain, source, instrumental, loose, duration,"
        " fetched_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, datetime('now'))"
        " ON CONFLICT(video_id, provider) DO UPDATE SET synced = excluded.synced, plain = excluded.plain,"
        " source = excluded.source, instrumental = excluded.instrumental, loose = excluded.loose,"
        " duration = excluded.duration, fetched_at = excluded.fetched_at"));
    q.addBindValue(videoId);
    q.addBindValue(provider);
    q.addBindValue(AppDatabase::text(answer.synced));
    q.addBindValue(AppDatabase::text(answer.plain));
    q.addBindValue(AppDatabase::text(answer.source));
    q.addBindValue(answer.instrumental ? 1 : 0);
    q.addBindValue(answer.loose ? 1 : 0);
    q.addBindValue(answer.durationS);
    if (!q.exec())
        qWarning("Monolist: could not keep a lyrics answer: %s", qPrintable(q.lastError().text()));
}

} // namespace LyricsStore
