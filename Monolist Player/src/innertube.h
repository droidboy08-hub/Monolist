#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QNetworkCookie>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QVariant>

#include <functional>
#include <memory>
#include <utility>
#include <vector>

class QNetworkAccessManager;
class QNetworkReply;

// YouTube Music's own API (InnerTube): the one music.youtube.com itself calls.
//
// A yt-dlp search spends most of its ~8 seconds starting a Python runtime. The
// same search here is one HTTPS round trip, a few hundred milliseconds on a warm
// connection, and it comes back in YouTube Music's shape: songs with their
// artists, album and square cover art, rather than videos with channel names.
// Suggestions are fast enough to show while typing.
//
// Streams are still resolved by yt-dlp: InnerTube's player endpoint needs the
// signature and challenge work that yt-dlp already does well.
//
// The API is unofficial and changes without notice, so every parser here is
// defensive, and MediaExtractor falls back to yt-dlp whenever a search fails.
class InnerTube : public QObject
{
    Q_OBJECT
public:
    // One piece of an artist line, as YouTube Music wrote it: a name and the
    // page its link opens ("Bruno Mars", his channel id), or what stands
    // between two names (" & ", ", "), which links nowhere. A name that came
    // without a link has an empty id and `name` set, so it can still be
    // looked up by what it says.
    struct Credit {
        QString text;
        QString browseId;
        bool name = false;
    };

    struct Track {
        QString videoId;
        QString title;
        QString artist;
        QString album;
        QString artwork;
        qint64 durationMs = 0;
        // A real video, rather than YouTube Music's own audio track (which is
        // a still picture of the cover and not worth showing).
        bool isVideo = false;
        // The first artist credited, as its own link said it: what Last.fm
        // is sent. `artist` joins every credit with ", ", and splitting that
        // again would break "Tyler, The Creator" in two. Empty when the
        // answer had no credit to read it from.
        QString primaryArtist;
        // `artist` again, piece by piece with each name's page, so every
        // name in a joint credit can open its own. Joined, the texts are
        // `artist` exactly. Empty when the answer linked nothing.
        QList<Credit> credits;
        // The album's page, when the row linked to it.
        QString albumId;
        // A playlist row's own id. The same song twice in one playlist has
        // one video id and two of these, so it is what tells a later page's
        // rows from ones already shown. Empty outside playlists.
        QString setVideoId;
    };

    // An album, playlist, artist or video, as the home feed and charts show them.
    struct Card {
        QString type;       // "album", "playlist", "artist", "song" or "video"
        QString browseId;   // albums, playlists, artists
        QString videoId;    // songs, videos
        QString title;
        QString subtitle;   // "Album • Seth Ballad", "Nirvana, Radiohead, ..."
        QString artwork;
        // Songs and videos: who is credited, read from the subtitle's own
        // runs as a song row's are ("Rick Astley", not "Rick Astley • 1.6B
        // views"), and the first of them alone, for Last.fm. Empty when the
        // subtitle names no one.
        QString artist;
        QString primaryArtist;
    };

    // Where a link goes: a page, and the parameters that pick part of it
    // (an artist's albums rather than the whole artist). `pageType` is
    // YouTube Music's own name for the page ("MUSIC_PAGE_TYPE_PLAYLIST"),
    // where the link says.
    struct Link {
        QString browseId;
        QString params;
        QString pageType;
    };

    // One row of a browse page: a run of songs (Quick picks) or of cards.
    struct Shelf {
        QString title;
        QString strapline;  // the small line above the title
        QList<Track> songs;
        QList<Card> cards;
        // The shelf's own "more" button: the whole of what it shows a few
        // of. Empty when it has none.
        Link more;
    };

    // An album or playlist page.
    struct Collection {
        QString browseId;
        QString type;         // "album" or "playlist"
        QString title;
        QString subtitle;     // "Album • 2026"
        QString artist;       // an album's artist line
        QList<Credit> artistCredits;   // the same, with each name's page
        QString primaryArtist;         // its first credit alone
        QString details;      // "20 songs • 1 hour, 38 minutes"
        QString description;
        QString artwork;
        QList<Track> tracks;
        // A long playlist comes a hundred songs at a time; this asks for the
        // next hundred (continueBrowse). Empty when there are no more.
        QString continuation;
    };

    // The next part of a long list, from continueBrowse: songs (a playlist's
    // or a shelf's), cards (a grid's), or whole shelves (a page that goes on
    // below), and the token for the part after it, empty at the end.
    struct Continuation {
        QList<Track> tracks;
        QList<Card> cards;
        QList<Shelf> shelves;
        QString next;
    };

    // A page that is a list rather than an album or an artist: what a
    // shelf's "more" button opens — a grid of albums, runs of songs, or
    // shelves of cards — under the page's own title.
    struct Listing {
        QString title;
        QList<Shelf> sections;
        // More of the last section's cards or songs, and more sections
        // after it, each asked for with continueBrowse.
        QString itemsContinuation;
        QString sectionsContinuation;
    };

    // What one of YouTube Music's own buttons plays: a watch playlist, from
    // a song in it. An artist's Shuffle and Mix are two of these.
    struct Watch {
        QString videoId;
        QString playlistId;
        QString params;
    };

    // An artist's page (or a channel's, which YouTube Music lays out alike):
    // the header, the top songs, then shelves of cards in the page's own
    // order and under its own titles ("Albums", "Singles & EPs", "Videos",
    // "Featured on", "Fans might also like"…), in the language asked in.
    struct Artist {
        QString browseId;
        // An artist's own page, rather than a plain channel's (an uploader
        // a video credits), which has no photograph header, top songs or mix.
        bool artistPage = false;
        QString name;
        QString description;
        QString audience;     // "217M monthly audience", or subscribers
        QString artwork;      // the banner, wide
        Watch shuffle;
        Watch radio;
        QString songsTitle;   // "Top songs"
        QString songsId;      // the playlist behind "Show all"
        QList<Track> songs;
        QList<Shelf> shelves;
    };

    // One artist in a search for artists.
    struct ArtistHit {
        QString name;
        QString browseId;
        QString artwork;
    };

    // What a search asks YouTube Music for. Songs and videos come back as
    // songs (search), the rest as cards (searchCards). Its own playlists and
    // its listeners' are two separate searches there, as they are here.
    enum class Filter { Songs, Videos, Albums, Artists, FeaturedPlaylists, CommunityPlaylists };

    // Which of YouTube's front ends to ask. Music knows songs, albums and
    // artists; YouTube knows every video, and answers when Music does not.
    //
    // Player is neither: it is the client YouTube's visionOS app identifies
    // itself as, and it is the only one of the three that /player will hand a
    // plain, directly playable stream URL to. WEB_REMIX is refused without a
    // PO token and WEB answers with SABR, so the existing two cannot be
    // reused for this no matter how convenient that would be.
    //
    // PlayerFallback is the same app under another version (0.1), asked only
    // when Player's answer holds no plain stream (see player()).
    enum class Client { Music, YouTube, Player, PlayerFallback };

    // Which of the two /player asks as: Player, then PlayerFallback when
    // Player gives no plain stream (Both, the default); Player alone, as
    // before there were two (First); or PlayerFallback alone (Second), which
    // the canary and the tests use to try it on its own. Read once a launch
    // from the setting youtube.player_client ("first", "second"); setting it
    // here instead overrides that until the next setVisitorStore.
    enum class PlayerClients { Both, First, Second };
    static void setPlayerClients(PlayerClients clients);
    static PlayerClients playerClients();

    // Whether a player() call is bounded (see player()): on unless the
    // setting youtube.player_deadline is "off", read once a launch like
    // youtube.player_client; setting it here overrides that the same way.
    static void setPlayerDeadline(bool on);
    static bool playerDeadline();

    // Whether a call may carry the signed-in YouTube Music account.
    //
    // Anonymous is every call's default, and sends exactly what was sent
    // before there were accounts, byte for byte. IfSignedIn carries the
    // account while a session is active, and is otherwise Anonymous.
    // Checking is YtmSession's own check of a session it holds but has not
    // confirmed yet; nothing else uses it. Only the Music client ever carries
    // the account: playback (Player), youtube.com's search and yt-dlp never do.
    enum class Auth { Anonymous, IfSignedIn, Checking };

    // The account, as the calls see it. Static for the same reason as the
    // region: there are several InnerTube objects, each with its own network
    // manager, and all of them must agree on whether there is an account.
    // Set once at start (main.cpp), before any request, to YtmSession's.
    struct AccountHook {
        // Fills in the Cookie and Authorization headers for one call from
        // `origin`, and returns the session they belong to; 0, with nothing
        // filled in, when this call goes without.
        std::function<quint64(Auth auth, const QByteArray &origin, QByteArray *cookie,
                              QByteArray *authorization)> headers;
        // The server refused that session outright (401, 403).
        std::function<void(quint64 session, int httpStatus)> rejected;
        // Set-Cookie on an answer to that session: its cookies, rotated.
        std::function<void(quint64 session, const QList<QNetworkCookie> &cookies)> cookies;
    };
    static void setAccountHook(AccountHook hook);

    // Told of every artist name an answer links to a page, as it is read:
    // the name, the page, and whether it is an artist's page (rather than a
    // plain channel's). ArtistLinks keeps them, so a name that reaches the
    // interface later without its link — from the library, the history, a
    // suggestion — can still open the page. Set once at start (main.cpp);
    // unset, nothing is told.
    static void setArtistHook(std::function<void(const QString &name, const QString &browseId,
                                                 bool artistPage)> hook);
    // Credits as QML reads them: [{ text, id, link }], `link` for a name.
    static QVariantList creditsToVariant(const QList<Credit> &credits);

    // Where the visitor id /player needs is kept between launches, with the
    // time it was fetched: reads and writes of the settings table, set once
    // at start (main.cpp) before any InnerTube is made. Unset, the id lasts
    // the launch. Setting one starts the id afresh (the self-test does).
    // /player's switches (youtube.visitor, youtube.player_client) are read
    // through it too.
    struct VisitorStore {
        std::function<QString(const QString &key)> read;
        std::function<void(const QString &key, const QString &value)> write;
    };
    static void setVisitorStore(VisitorStore store);
    // Clear history: the anonymous visitor id, its stored copy and YouTube's
    // anonymous cookies are dropped, and the next call is a first visit.
    static void forgetVisitorData();
    // The signed-in account's own visitor id, for the sign-in work. It goes
    // on the calls that carry the account, where it outranks the anonymous
    // one, and on no other; the anonymous one never goes on those. Empty
    // takes it away again.
    static void setSessionVisitorData(const QString &visitorData);

    // `warmUp` opens the TLS connections, and has the one visitor id every
    // object shares fetched unless one is stored, for the objects that play
    // and search. One that only makes the odd call (YtmSession's check) goes
    // without. Every object on the application thread shares one cookie jar.
    explicit InnerTube(QObject *parent = nullptr, bool warmUp = true);

    // For the self-tests only: every request goes to this address
    // ("http://127.0.0.1:<port>") instead of YouTube, and objects made
    // afterwards do not warm up. Empty is YouTube again.
    static void setTestServer(const QString &baseUrl);

    // The country every request is made from ("US", "JP", …), which decides
    // what YouTube Music offers: new releases, charts and the ranking of
    // search results. Empty means the one the system is set to. YouTube also
    // reads the connection's own location, so this steers rather than decides.
    static void setRegion(const QString &code);
    static QString region();          // the code actually sent
    static QString systemRegion();    // what the system is set to

    // YouTube Music serves some countries and refuses the rest outright (400
    // Bad Request for every call, which looks exactly like being offline). A
    // refused country is dropped here and the request is made again from the
    // system's own; the handler is told, so the choice can be forgotten and
    // the user told why.
    static void setRegionRejectedHandler(std::function<void(const QString &code)> handler);

    // The countries it serves, by their ISO code. Asked of the API itself,
    // country by country, rather than taken from a page that goes stale.
    static const QSet<QString> &servedRegions();

    // One browse request (the home feed, charts, new releases, an album or a
    // playlist); `done` gets the response, or an error. Any number can run.
    // Anonymous unless the caller asks for the account.
    void browse(const QString &browseId,
                std::function<void(const QJsonObject &root, const QString &error)> done,
                Auth auth = Auth::Anonymous);
    // The same with the parameters a link carries (Link::params), which
    // pick part of a page: an artist's albums, say, rather than the artist.
    void browse(const QString &browseId, const QString &params,
                std::function<void(const QJsonObject &root, const QString &error)> done);
    // The next part of a long list, by the token its last part ended with
    // (Collection::continuation, Listing's, Continuation::next); read the
    // answer with parseContinuation.
    void continueBrowse(const QString &token,
                        std::function<void(const QJsonObject &root, const QString &error)> done);
    static QList<Shelf> parseShelves(const QJsonObject &root);
    static Collection parseCollection(const QString &browseId, const QJsonObject &root);
    // A playlist's song as its page would show it: what its row leaves out
    // (an album's artist, its cover) filled in from the page's header.
    static void completeTrack(Track &track, const Collection &collection);
    static Continuation parseContinuation(const QJsonObject &root);
    static Listing parseListing(const QJsonObject &root);
    static Artist parseArtist(const QString &browseId, const QJsonObject &root);
    static QList<ArtistHit> parseArtistSearch(const QJsonObject &root);
    static QList<Card> parseCardSearch(const QJsonObject &root);
    // A card as QML reads it: { type, browseId, videoId, title, subtitle,
    // artwork, artist, primaryArtist }.
    static QVariantMap cardToVariant(const Card &card);

    // YouTube Music's artists matching a name, best first. `done` gets an
    // error only when the request failed. Not cancellable: each caller
    // keeps its own answer or drops it.
    void searchArtists(const QString &query,
                       std::function<void(const QList<ArtistHit> &hits, const QString &error)> done);
    // Albums, artists or playlists matching a query, as cards, best first
    // (Filter::Albums, Artists, FeaturedPlaylists or CommunityPlaylists).
    // Not cancellable either: the caller drops an answer it no longer wants.
    void searchCards(const QString &query, Filter filter,
                     std::function<void(const QList<Card> &cards, const QString &error)> done);
    // Songs or videos matching a query, as search() finds them, answered to
    // `done` rather than by signal. Not cancellable, so several can be in
    // flight at once: the recommender looks a suggestion up for a press, for
    // a menu and for Play all, and none of them may silence another.
    void searchTracks(const QString &query, Filter filter,
                      std::function<void(const QList<Track> &tracks, const QString &error)> done);
    // The songs behind a Watch — an artist's Shuffle or Mix — as the queue
    // YouTube Music would play, the first song first.
    void watchPlaylist(const Watch &watch,
                       std::function<void(const QList<Track> &tracks, const QString &error)> done);

    // account/account_menu: who is signed in. Asked only with the account;
    // without one it only offers to sign in.
    void accountMenu(Auth auth, std::function<void(const QJsonObject &root, const QString &error)> done);
    // The signed-in account's name in account_menu's answer, or empty.
    static QString parseAccountName(const QJsonObject &root);
    // `logged_in` from any answer's responseContext.serviceTrackingParams:
    // "1", "0", or empty when the answer does not say. The one reliable sign
    // that a call was answered as the account: bad cookies are usually
    // answered 200 OK with the signed-out feed.
    static QString parseLoggedIn(const QJsonObject &root);

    // A song's lyrics as YouTube Music shows them: plain text, from a partner
    // it names ("Source: Musixmatch"). Two requests: the watch page says
    // where the lyrics are, a browse fetches them. `done` gets empty text
    // when the song has none, and an error only when a request failed.
    //
    // What it returns calls the lookup off: whichever request is out is
    // aborted, and `done` is never called. Each lookup has its own, rather
    // than a newer one replacing an older as a search does, because several
    // songs are looked up at once (the one playing and the one after it) and
    // a lookup that lost the lyrics race must stop without touching those.
    std::function<void()> lyrics(const QString &videoId,
                                 std::function<void(const QString &text, const QString &source,
                                                    const QString &error)> done);

    // A newer call of the same kind cancels the one still in flight. The kinds
    // are independent: a search starting must not cancel the suggestions for
    // what is being typed. Songs or videos; the other filters are cards, and
    // go to searchCards.
    void search(const QString &query, Filter filter);
    // The same query against youtube.com, for when Music fails or finds
    // nothing: videos with their channel, not songs with their album, but one
    // request rather than yt-dlp's Python start-up.
    void searchYouTube(const QString &query);
    void suggest(const QString &input);
    // The "radio" YouTube Music builds from one song: songs like it, the seed
    // itself first. What autoplay continues with when a queue runs out.
    void radio(const QString &videoId);
    void cancelSearch();
    void cancelSuggestions();
    void cancelRadio();

    // One /player call: the audio stream for a track, in one round trip and
    // in this process, where the alternative is starting yt-dlp.
    //
    // Of the formats offered, Opus is taken first (774, 251), then AAC (141,
    // 140), never a DRC copy while another is offered, and the log says which
    // itag, codec and bitrate were chosen from what (pickStream, innertube.cpp).
    // The setting youtube.format=bitrate takes the highest bitrate instead,
    // as before.
    //
    // `url` is empty whenever anything at all went wrong and `error` says
    // what, client by client; the caller is expected to fall through to
    // yt-dlp rather than show it, because yt-dlp still resolves things this
    // cannot — age-gated and made-for-kids tracks, and live streams.
    //
    // An answer without a plain stream, whatever its status, is asked once
    // more as PlayerFallback before that (PlayerClients). A request that got
    // no answer at all is not: the same server, asked again, would cost the
    // same wait again, and yt-dlp is what is left.
    //
    // The whole call has 3 s, from the call to `done`: the visitor id, the
    // second client and a renewed id included. A first request with no
    // answer after 1.2 s, or that fails before then, is sent once more
    // beside it on a connection of its own (a second network manager, so it
    // cannot be queued behind the stalled one); whichever answers first is
    // taken and the other dropped. youtube.player_deadline=off is the switch
    // back: 8 s a request, one retry 1.2 s after a failure on the same
    // connection, and no limit on the whole.
    //
    // Deliberately not cancellable: unlike search or suggestions there is no
    // "newer one of the same kind", and giving it a slot would make a prefetch
    // and a foreground resolve abort each other.
    void player(const QString &videoId,
                std::function<void(const QString &url, int itag, const QString &error)> done);

Q_SIGNALS:
    void searchFinished(const QString &query, const QList<InnerTube::Track> &tracks);
    void searchFailed(const QString &query, const QString &reason);
    void youtubeSearchFinished(const QString &query, const QList<InnerTube::Track> &tracks);
    void youtubeSearchFailed(const QString &query, const QString &reason);
    void suggestionsReady(const QString &input, const QStringList &suggestions);
    void radioReady(const QString &seedVideoId, const QList<InnerTube::Track> &tracks);
    void radioFailed(const QString &seedVideoId, const QString &reason);

private:
    // Where the one request of a kind (search, suggestions, radio) is kept,
    // so a newer one or a cancel can replace it. The reply alone is not
    // enough: between a failed attempt and its retry there is no reply in
    // flight, only a timer, and a retry that fired after a newer request had
    // started would take that request's place and silence it. So every new
    // request and every cancel also moves the generation on, and anything
    // started for an older generation — a reply, a retry waiting to be sent —
    // drops itself when it finds the slot has moved.
    struct Slot {
        QPointer<QNetworkReply> reply;
        quint64 generation = 0;
    };
    // Moves the slot on and aborts the reply it holds, if any.
    static void release(Slot &slot);

    // `session` is set to the account's session when the request carries it,
    // and to 0 when it goes anonymous. `network` is the manager it goes
    // through: this object's own unless another is given.
    QNetworkReply *post(Client client, const QString &endpoint, QJsonObject body, int timeoutMs,
                        Auth auth, quint64 *session, QNetworkAccessManager *network = nullptr);
    // One request, its answer as JSON. A dropped connection or a timeout is
    // ordinary on a home connection, so it is tried once more before failing;
    // `slot`, where given, holds the reply so a newer request can cancel it,
    // and a cancelled or replaced request is dropped silently rather than
    // retried. One that carried the account and was refused for it is sent
    // again without it.
    void send(Client client, const QString &endpoint, const QJsonObject &body, int timeoutMs,
              Slot *slot,
              std::function<void(const QJsonObject &root, const QString &error)> done,
              int retries = 1, Auth auth = Auth::Anonymous, QNetworkAccessManager *network = nullptr);

    static QList<Track> parseSearch(const QJsonObject &root);
    static QList<Track> parseYouTubeSearch(const QJsonObject &root);
    static QStringList parseSuggestions(const QJsonObject &root);
    static QList<Track> parseRadio(const QJsonObject &root);

    // /player is refused with LOGIN_REQUIRED for most music without an
    // anonymous visitor id. One is held for the whole process (innertube.cpp);
    // `then` runs at once when it is there, and otherwise once a fetch has
    // answered, whether or not it brought one.
    void withVisitorData(std::function<void()> then);
    // One player() call as it goes: the clients still to ask, the refusals
    // so far, and whether it has been asked again with a new visitor id,
    // which happens at most once (innertube.cpp).
    struct PlayerAsk;
    // One /player request as `client`.
    void askPlayer(Client client, std::shared_ptr<PlayerAsk> ask);
    // What `client` answered (`root`), or why it did not (`error`),
    // with `visitor` the id the request named.
    void playerAnswered(Client client, const QString &visitor, std::shared_ptr<PlayerAsk> ask,
                        const QJsonObject &root, const QString &error);
    // `client` gave no plain stream (`why`): the next client, or the end.
    void playerRefused(Client client, const QString &why, std::shared_ptr<PlayerAsk> ask);
    // A bounded call (see player()): one of its requests, the first or its
    // hedge, sent; one of them come back; the hedge sent; the time up.
    void sendPlayer(Client client, const QString &visitor, const QJsonObject &body,
                    std::shared_ptr<PlayerAsk> ask, bool hedge);
    void playerAttemptDone(Client client, const QString &visitor, const QJsonObject &body,
                           std::shared_ptr<PlayerAsk> ask, bool hedge, const QJsonObject &root,
                           const QString &error);
    void hedgePlayer(Client client, const QString &visitor, const QJsonObject &body,
                     std::shared_ptr<PlayerAsk> ask, const QString &why);
    void playerDeadlinePassed(std::shared_ptr<PlayerAsk> ask);
    // Calls the call's `done`, once, and lets go of what it held.
    void finishPlayer(std::shared_ptr<PlayerAsk> ask, const QString &url, int itag, const QString &error);

    QNetworkAccessManager *m_network;
    Slot m_search;
    Slot m_suggest;
    Slot m_radio;
};
