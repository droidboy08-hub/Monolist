#pragma once

#include <QHash>
#include <QObject>
#include <QTimer>
#include <QVariant>
#include <QVariantList>

// Which page an artist's name opens.
//
// A song fresh from YouTube Music carries its credits with their links, and
// the interface uses those as they are. Most songs reach the interface from
// somewhere that kept only the name: Liked songs, a playlist, the history, a
// download, a suggestion from the catalogue. For those, this remembers every
// name an answer has linked (InnerTube tells it as it reads them) and keeps
// what it learnt in the database, so a name seen once opens its page from
// then on — and a joint credit ("Bruno Mars & Lady Gaga") can be split into
// names that are each known, rather than guessed at by its commas.
//
// Exposed to QML as the "Artists" singleton.
class ArtistLinks : public QObject
{
    Q_OBJECT
public:
    explicit ArtistLinks(QObject *parent = nullptr);
    ~ArtistLinks() override;

    // What earlier sessions learnt. Once, after the database is open.
    void load();

    // This name opens this page. An artist's own page is kept over a plain
    // channel that goes by the same name; otherwise the latest seen wins.
    void remember(const QString &name, const QString &browseId, bool artistPage);

    // An artist line as pieces to show: [{ text, id, link }], `link` set for
    // a name and `id` its page where one is known. `known` is the track's own
    // credits, used as they are when there are any. Otherwise the line is
    // split into names this knows; a line that cannot be split wholly into
    // known names stays one piece, linked by its text alone, rather than cut
    // at a comma that may be part of a name ("Tyler, The Creator").
    Q_INVOKABLE QVariantList credits(const QString &artist, const QVariant &known = QVariant()) const;
    // The page for one name: as written, or failing that in another case or
    // with other accents and punctuation. Empty when no answer linked it.
    Q_INVOKABLE QString idFor(const QString &name) const;

    // Letters and digits only, case-folded and without accents: how two
    // spellings of one name ("Guns N' Roses", "Guns N’ Roses") are matched.
    static QString looseKey(const QString &name);

private:
    void save();

    struct Link {
        QString browseId;
        bool artistPage = false;
    };
    QHash<QString, Link> m_links;        // by the name as written
    QHash<QString, QString> m_loose;     // looseKey → the name as written
    QHash<QString, Link> m_unsaved;
    QTimer m_saveTimer;
};
