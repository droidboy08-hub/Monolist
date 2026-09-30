#include "trackfiltermodel.h"

#include <QCollator>
#include <QRegularExpression>

namespace {

QCollator &collator()
{
    static QCollator by = [] {
        QCollator c;
        c.setCaseSensitivity(Qt::CaseInsensitive);
        c.setNumericMode(true);
        return c;
    }();
    return by;
}

} // namespace

TrackFilterModel::TrackFilterModel(QObject *parent)
    : QSortFilterProxyModel(parent)
{
    setDynamicSortFilter(true);
    setSortCaseSensitivity(Qt::CaseInsensitive);
    connect(this, &QAbstractItemModel::rowsInserted, this, &TrackFilterModel::countChanged);
    connect(this, &QAbstractItemModel::rowsRemoved, this, &TrackFilterModel::countChanged);
    connect(this, &QAbstractItemModel::modelReset, this, &TrackFilterModel::countChanged);
    connect(this, &QAbstractItemModel::layoutChanged, this, &TrackFilterModel::countChanged);
}

QStringList TrackFilterModel::artistsIn(const QString &credit)
{
    // Separators YouTube Music writes between names. "and" is left alone:
    // too many one-act names hold it.
    static const QRegularExpression between(
        QStringLiteral(R"(\s*(?:,|&|\s+x\s+|\s+(?:feat\.?|ft\.?|featuring)\s+)\s*)"),
        QRegularExpression::CaseInsensitiveOption);
    QStringList names;
    for (const QString &part : credit.split(between, Qt::SkipEmptyParts)) {
        const QString name = part.trimmed();
        if (!name.isEmpty())
            names << name;
    }
    return names;
}

void TrackFilterModel::setFilterText(const QString &text)
{
    if (text == m_filterText)
        return;
    const bool was = rearranged();
    m_filterText = text;
    m_words = text.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    beginFilterChange();
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
    Q_EMIT filterTextChanged();
    if (was != rearranged())
        Q_EMIT rearrangedChanged();
}

void TrackFilterModel::setArtist(const QString &artist)
{
    if (artist == m_artist)
        return;
    const bool was = rearranged();
    m_artist = artist;
    beginFilterChange();
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
    Q_EMIT artistChanged();
    if (was != rearranged())
        Q_EMIT rearrangedChanged();
}

void TrackFilterModel::setSortKey(const QString &key)
{
    if (key == m_sortKey)
        return;
    const bool was = rearranged();
    m_sortKey = key;
    applySort();
    Q_EMIT sortChanged();
    if (was != rearranged())
        Q_EMIT rearrangedChanged();
}

void TrackFilterModel::setDescending(bool descending)
{
    if (descending == m_descending)
        return;
    m_descending = descending;
    applySort();
    Q_EMIT sortChanged();
}

void TrackFilterModel::applySort()
{
    // No key: the list's own order (the latest first, or the playlist's).
    // Back to it first either way: sort() does nothing when its column and
    // order are as they were, which a new key alone does not change.
    sort(-1);
    if (!m_sortKey.isEmpty())
        sort(0, m_descending ? Qt::DescendingOrder : Qt::AscendingOrder);
}

int TrackFilterModel::role(const char *name) const
{
    const QHash<int, QByteArray> roles = sourceModel() ? sourceModel()->roleNames() : QHash<int, QByteArray>();
    for (auto it = roles.cbegin(); it != roles.cend(); ++it) {
        if (it.value() == name)
            return it.key();
    }
    return -1;
}

bool TrackFilterModel::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    if (m_words.isEmpty() && m_artist.isEmpty())
        return true;
    const QModelIndex index = sourceModel()->index(sourceRow, 0, sourceParent);
    const QString title = index.data(role("title")).toString();
    const QString artist = index.data(role("artist")).toString();
    const QString album = index.data(role("album")).toString();
    if (!m_artist.isEmpty()) {
        const QStringList names = artistsIn(artist);
        const bool theirs = std::any_of(names.cbegin(), names.cend(), [this](const QString &name) {
            return name.compare(m_artist, Qt::CaseInsensitive) == 0;
        });
        if (!theirs)
            return false;
    }
    for (const QString &word : m_words) {
        if (!title.contains(word, Qt::CaseInsensitive) && !artist.contains(word, Qt::CaseInsensitive)
            && !album.contains(word, Qt::CaseInsensitive))
            return false;
    }
    return true;
}

bool TrackFilterModel::lessThan(const QModelIndex &left, const QModelIndex &right) const
{
    if (m_sortKey == QLatin1String("duration")) {
        const int ms = role("durationMs");
        return left.data(ms).toLongLong() < right.data(ms).toLongLong();
    }
    const QByteArray key = m_sortKey.toUtf8();
    const int by = role(key.constData());
    const int titleRole = role("title");
    const int order = collator().compare(left.data(by).toString(), right.data(by).toString());
    if (order != 0 || by == titleRole)
        return order < 0;
    // The same artist or album: by title within it.
    return collator().compare(left.data(titleRole).toString(), right.data(titleRole).toString()) < 0;
}

QVariantMap TrackFilterModel::get(int row) const
{
    QVariantMap map;
    const QModelIndex at = index(row, 0);
    if (!at.isValid())
        return map;
    const QHash<int, QByteArray> roles = roleNames();
    for (auto it = roles.cbegin(); it != roles.cend(); ++it)
        map.insert(QString::fromUtf8(it.value()), at.data(it.key()));
    return map;
}

QVariantList TrackFilterModel::all() const
{
    QVariantList rows;
    rows.reserve(rowCount());
    for (int row = 0; row < rowCount(); ++row)
        rows.append(get(row));
    return rows;
}
