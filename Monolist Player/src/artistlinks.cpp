#include "artistlinks.h"
#include "appdatabase.h"

#include <QRegularExpression>
#include <QSqlQuery>
#include <QThread>

namespace {

QVariantMap piece(const QString &text, const QString &id, bool link)
{
    return {
        { QStringLiteral("text"), text },
        { QStringLiteral("id"), id },
        { QStringLiteral("link"), link }
    };
}

} // namespace

ArtistLinks::ArtistLinks(QObject *parent)
    : QObject(parent)
{
    // A page of search results names twenty artists at once; they are
    // written together, a moment later, in one transaction.
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(3000);
    connect(&m_saveTimer, &QTimer::timeout, this, &ArtistLinks::save);
}

ArtistLinks::~ArtistLinks()
{
    save();
}

QString ArtistLinks::looseKey(const QString &name)
{
    const QString decomposed = name.normalized(QString::NormalizationForm_KD);
    QString key;
    key.reserve(decomposed.size());
    for (const QChar c : decomposed) {
        if (c.isLetterOrNumber())
            key.append(c);
    }
    return key.toCaseFolded();
}

void ArtistLinks::load()
{
    QSqlQuery query(AppDatabase::connection());
    if (!query.exec(QStringLiteral("SELECT name, browse_id, artist_page FROM artist_links")))
        return;
    while (query.next()) {
        const QString name = query.value(0).toString();
        const QString browseId = query.value(1).toString();
        if (name.isEmpty() || browseId.isEmpty())
            continue;
        m_links.insert(name, { browseId, query.value(2).toBool() });
        const QString key = looseKey(name);
        if (!key.isEmpty())
            m_loose.insert(key, name);
    }
}

void ArtistLinks::remember(const QString &name, const QString &browseId, bool artistPage)
{
    // InnerTube reads its answers on the thread its objects live on, which
    // is this one; anything else is handed over rather than raced.
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this, name, browseId, artistPage]() {
            remember(name, browseId, artistPage);
        }, Qt::QueuedConnection);
        return;
    }
    if (name.isEmpty() || browseId.isEmpty())
        return;
    const auto found = m_links.constFind(name);
    if (found != m_links.cend()) {
        if (found->browseId == browseId && found->artistPage == artistPage)
            return;   // nothing new, and nothing to write
        if (found->artistPage && !artistPage)
            return;   // the artist's own page is the better answer
    }
    const Link link{ browseId, artistPage };
    m_links.insert(name, link);
    const QString key = looseKey(name);
    if (!key.isEmpty())
        m_loose.insert(key, name);
    m_unsaved.insert(name, link);
    if (!m_saveTimer.isActive())
        m_saveTimer.start();
}

void ArtistLinks::save()
{
    m_saveTimer.stop();
    if (m_unsaved.isEmpty())
        return;
    QSqlDatabase db = AppDatabase::connection();
    if (!db.isOpen())
        return;
    db.transaction();
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO artist_links (name, browse_id, artist_page, seen_at)"
        " VALUES (?, ?, ?, datetime('now'))"));
    for (auto it = m_unsaved.cbegin(); it != m_unsaved.cend(); ++it) {
        query.addBindValue(AppDatabase::text(it.key()));
        query.addBindValue(AppDatabase::text(it->browseId));
        query.addBindValue(it->artistPage ? 1 : 0);
        query.exec();
    }
    db.commit();
    m_unsaved.clear();
}

QString ArtistLinks::idFor(const QString &name) const
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty())
        return {};
    const auto exact = m_links.constFind(trimmed);
    if (exact != m_links.cend())
        return exact->browseId;
    const QString key = looseKey(trimmed);
    if (key.isEmpty())
        return {};
    const auto loose = m_loose.constFind(key);
    return loose != m_loose.cend() ? m_links.value(*loose).browseId : QString();
}

QVariantList ArtistLinks::credits(const QString &artist, const QVariant &known) const
{
    // The track's own: exact, and each name already has its page. A name it
    // gave without one may still be known here.
    const QVariantList given = known.toList();
    if (!given.isEmpty()) {
        QVariantList pieces;
        pieces.reserve(given.size());
        for (const QVariant &value : given) {
            QVariantMap map = value.toMap();
            if (map.value(QStringLiteral("link")).toBool()
                && map.value(QStringLiteral("id")).toString().isEmpty())
                map.insert(QStringLiteral("id"), idFor(map.value(QStringLiteral("text")).toString()));
            pieces.append(map);
        }
        return pieces;
    }

    if (artist.trimmed().isEmpty())
        return {};
    if (const QString id = idFor(artist); !id.isEmpty())
        return { piece(artist, id, true) };

    // The line cut at every joiner, each kept, so the pieces still add up
    // to exactly the line as written.
    static const QRegularExpression joiner(
        QStringLiteral(R"((\s*,\s+|\s+(?:&|and|x|×|vs\.?|feat\.?|ft\.?|featuring|with|/|\+)\s+|\s*[、・]\s*))"),
        QRegularExpression::CaseInsensitiveOption);
    QStringList names;
    QStringList joiners;
    qsizetype at = 0;
    for (auto matches = joiner.globalMatch(artist); matches.hasNext();) {
        const QRegularExpressionMatch match = matches.next();
        names << artist.mid(at, match.capturedStart() - at);
        joiners << match.captured();
        at = match.capturedEnd();
    }
    names << artist.mid(at);
    const QVariantList whole{ piece(artist, QString(), true) };
    if (names.size() < 2)
        return whole;

    // From each name, the longest run of names that is itself a known name:
    // "Earth, Wind & Fire" is found whole before "Earth" is tried alone.
    QVariantList pieces;
    for (qsizetype first = 0; first < names.size();) {
        qsizetype matched = -1;
        for (qsizetype last = names.size() - 1; last >= first; --last) {
            QString candidate = names.at(first);
            for (qsizetype k = first; k < last; ++k)
                candidate += joiners.at(k) + names.at(k + 1);
            const QString id = idFor(candidate);
            if (!id.isEmpty()) {
                pieces.append(piece(candidate, id, true));
                matched = last;
                break;
            }
        }
        if (matched < 0)
            return whole;   // a name nobody has linked: not split on a guess
        if (matched + 1 < names.size())
            pieces.append(piece(joiners.at(matched), QString(), false));
        first = matched + 1;
    }
    return pieces;
}
