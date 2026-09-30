#pragma once

#include <QSortFilterProxyModel>
#include <QtQml/qqmlregistration.h>
#include <QVariantList>
#include <QVariantMap>

// A list of songs as the reader wants it: only those matching what was
// typed in its filter field, or one artist's, in the order a column's header
// asked for. Any list of songs can sit behind one (the library's songs, Liked
// songs, a playlist, Downloads): its rows keep the list's roles, so the song
// table and the player read it as they read the list itself.
class TrackFilterModel : public QSortFilterProxyModel
{
    Q_OBJECT
    QML_ELEMENT
    // Words, each of which must be in the song's title, artist or album.
    Q_PROPERTY(QString filterText READ filterText WRITE setFilterText NOTIFY filterTextChanged)
    // "" for the list's own order, or "title", "artist", "album", "duration".
    Q_PROPERTY(QString sortKey READ sortKey WRITE setSortKey NOTIFY sortChanged)
    Q_PROPERTY(bool descending READ descending WRITE setDescending NOTIFY sortChanged)
    // One artist's songs only: a name among the song's credits. Empty for all.
    Q_PROPERTY(QString artist READ artist WRITE setArtist NOTIFY artistChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    // Whether anything narrows or reorders the list; a playlist is not put
    // in order by hand while something does.
    Q_PROPERTY(bool rearranged READ rearranged NOTIFY rearrangedChanged)
public:
    explicit TrackFilterModel(QObject *parent = nullptr);

    QString filterText() const { return m_filterText; }
    void setFilterText(const QString &text);
    QString sortKey() const { return m_sortKey; }
    void setSortKey(const QString &key);
    bool descending() const { return m_descending; }
    void setDescending(bool descending);
    QString artist() const { return m_artist; }
    void setArtist(const QString &artist);
    int count() const { return rowCount(); }
    bool rearranged() const { return !m_words.isEmpty() || !m_artist.isEmpty() || !m_sortKey.isEmpty(); }

    // A row with every role, as SearchResultModel.get gives it.
    Q_INVOKABLE QVariantMap get(int row) const;
    // Every row shown, in order, in the shape Player.playTracks takes.
    Q_INVOKABLE QVariantList all() const;

    // The names a credit line holds: "A, B & C feat. D" is four. Public for
    // Library's artists and --library-edit-test.
    static QStringList artistsIn(const QString &credit);

Q_SIGNALS:
    void filterTextChanged();
    void sortChanged();
    void artistChanged();
    void countChanged();
    void rearrangedChanged();

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;
    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override;

private:
    int role(const char *name) const;
    void applySort();

    QString m_filterText;
    QStringList m_words;
    QString m_sortKey;
    bool m_descending = false;
    QString m_artist;
};
