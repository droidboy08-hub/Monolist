#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QVariantMap>

// One entry in the play queue. Built from whichever list the play came from —
// library, search results, downloads, an album page — by fromMap(), which
// accepts the role names all of those models use.
struct QueueTrack {
    QString videoId;
    QString title;
    QString artist;
    QString album;
    QString artwork;
    QString sourceUrl;        // a local file or direct URL, when the row had one
    qint64 durationMs = 0;
    int trackId = 0;          // the library row; 0 when not in the library
    bool fromRadio = false;   // added by autoplay rather than chosen
    bool isVideo = false;     // has a picture worth showing
    quint64 uid = 0;          // identity within the queue, stamped by QueueModel

    static QueueTrack fromMap(const QVariantMap &map);
    QVariantMap toMap() const;
};

// What plays, in the order it will play.
//
// The prototype played "the library": next and previous walked the library,
// and a search result had no neighbours at all. Now playing from a list queues
// that list, Play next and Add to queue edit it, and autoplay extends it with
// YouTube Music's radio when it runs out. Shuffle reorders the upcoming part in
// place, so the queue always shows what will actually play; turning shuffle
// off puts the original order back.
class QueueModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(int currentIndex READ currentIndex NOTIFY currentIndexChanged)
    Q_PROPERTY(int upcomingCount READ upcomingCount NOTIFY upcomingChanged)
    // The first upcoming song autoplay added, where the queue panel marks the
    // radio taking over; -1 when none.
    Q_PROPERTY(int radioStartIndex READ radioStartIndex NOTIFY upcomingChanged)
public:
    enum Roles { VideoIdRole = Qt::UserRole + 1, TitleRole, ArtistRole, AlbumRole, ArtworkRole,
                 DurationRole, DurationTextRole, FromRadioRole, IsCurrentRole, IsPastRole };

    explicit QueueModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int currentIndex() const { return m_current; }
    int upcomingCount() const;
    int radioStartIndex() const;
    const QueueTrack *at(int row) const;
    const QueueTrack *current() const { return at(m_current); }
    bool containsVideo(const QString &videoId) const;

    void setCurrentIndex(int row);
    void replace(QList<QueueTrack> tracks, int current);
    void insert(int row, QList<QueueTrack> tracks);
    void removeAt(int row);           // not the current row
    void clearUpcoming();
    void shuffleUpcoming();
    void restoreOrder();

    Q_INVOKABLE QVariantMap get(int row) const;

Q_SIGNALS:
    void countChanged();
    void currentIndexChanged();
    void upcomingChanged();

private:
    void stamp(QList<QueueTrack> &tracks);

    QList<QueueTrack> m_items;
    int m_current = -1;
    quint64 m_nextUid = 1;
    // The upcoming tracks' order from before shuffling, by uid; empty when
    // not shuffled.
    QList<quint64> m_originalOrder;
};
