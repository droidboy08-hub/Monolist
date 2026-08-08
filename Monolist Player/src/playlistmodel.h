#pragma once

#include <QAbstractListModel>

struct PlaylistItem {
    int id = 0;
    QString name;
    int trackCount = 0;
};

class PlaylistModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
public:
    enum Roles { IdRole = Qt::UserRole + 1, NameRole, TrackCountRole, NumberRole };

    explicit PlaylistModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void reload();
    Q_INVOKABLE QVariantMap get(int row) const;

Q_SIGNALS:
    void countChanged();

private:
    QList<PlaylistItem> m_items;
};
