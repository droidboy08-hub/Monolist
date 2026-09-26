#pragma once

#include <QByteArray>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <QVector>

class QNetworkAccessManager;
class QNetworkReply;
class Recommender;

// The recommendation data, downloaded from inside the app.
//
// The catalogue and the country charts cannot ship with Monolist: they are
// licensed for non-commercial use only, and the code is MIT. So they live in a
// repository of their own, droidboy08-hub/Monolist-data, and this fetches one
// tagged version of it into the app's data folder, the way "Update components"
// fetches the tools. manifest.json at the tag lists every file with its size
// and SHA-256; each file is streamed to "<name>.part", hashed as it arrives,
// and only renamed into place once both match. A file already here with the
// right size and hash is kept, so a download stopped half-way — by Cancel, a
// dropped connection or a closed app — carries on from the last whole file.
//
// Exposed to QML as "RecData".
class RecData : public QObject
{
    Q_OBJECT
    // A download (or the check of what is already here) is running.
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    // Every file of this version is here, whole.
    Q_PROPERTY(bool installed READ installed NOTIFY changed)
    // Some of it is here but not all: stopped part-way. Downloading again
    // keeps what is here.
    Q_PROPERTY(bool partial READ partial NOTIFY changed)
    Q_PROPERTY(bool removing READ removing NOTIFY changed)
    // The last attempt failed, and `status` says why.
    Q_PROPERTY(bool failed READ failed NOTIFY changed)
    // ...and it was a Remove: what could not be deleted is still there.
    Q_PROPERTY(bool removeFailed READ removeFailed NOTIFY changed)
    // The recommender is reading its catalogue from the download.
    Q_PROPERTY(bool inUse READ inUse NOTIFY changed)
    // What is happening, or what happened: one line for the interface.
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(qreal progress READ progress NOTIFY changed)
    // "42.1 of 100.4 MB".
    Q_PROPERTY(QString progressText READ progressText NOTIFY changed)
    // The size of what is on disk: all of it when installed, the part
    // downloaded so far when not.
    Q_PROPERTY(QString sizeText READ sizeText NOTIFY changed)
    Q_PROPERTY(int version READ version CONSTANT)
    Q_PROPERTY(QString published READ published NOTIFY changed)
    // Where it goes, for showing: <data>/recommendations/v1.
    Q_PROPERTY(QString folder READ displayFolder CONSTANT)
    Q_PROPERTY(QString catalogueDirectory READ catalogueDirectory CONSTANT)

public:
    explicit RecData(QObject *parent = nullptr);
    ~RecData() override;

    // Points the recommender at the download once it is whole, and lets go of
    // it before Remove deletes it. Also adopts a download already here when no
    // catalogue is set at all — one finished by a run that was closed just
    // before it could say so — unless that run was removing it, in which case
    // the removal is finished instead.
    void setRecommender(Recommender *recommender);

    bool busy() const { return m_busy; }
    bool installed() const { return m_installed; }
    bool partial() const { return !m_installed && m_bytesOnDisk > 0; }
    bool removing() const { return m_removing; }
    bool failed() const { return m_failed; }
    bool removeFailed() const { return m_removeFailed; }
    bool inUse() const;
    QString status() const { return m_status; }
    qreal progress() const;
    QString progressText() const;
    QString sizeText() const;
    int version() const;
    QString published() const { return m_published; }
    QString displayFolder() const;
    QString catalogueDirectory() const;

    // Where the files are fetched from: the pinned tag on GitHub, or
    // MONOLIST_REC_DATA_URL (a URL or a local folder) for testing.
    static QString baseUrl();
    // <data>/recommendations/v<version>, the one folder this creates.
    static QString folderPath();

    // What the last download did with each file, for the self-test.
    int keptFiles() const { return m_kept; }
    int fetchedFiles() const { return m_fetched; }
    int refusedFiles() const { return m_refused; }
    qint64 doneBytes() const { return m_completedBytes + m_currentBytes; }
    qint64 totalBytes() const { return m_totalBytes; }

public Q_SLOTS:
    void download();
    void cancel();
    // Deletes the folder this created, and nothing else: first making the
    // recommender let go of the files, since a mapped file cannot be deleted
    // on Windows. Recorded in settings until done, so a quit in between is
    // finished at the next launch.
    void remove();
    // Points the recommender at the download.
    void use();

Q_SIGNALS:
    void changed();
    // A download ended: whole and checked, or not.
    void finished(bool ok);

private:
    struct Entry {
        QString path;       // relative, "GraphData/US.sqlite"
        qint64 size = 0;
        QByteArray sha256;  // lowercase hex
    };

    QUrl urlFor(const QString &path) const;
    void manifestArrived(QNetworkReply *reply);
    // Reads and checks a manifest; false, and `error` saying why, when it
    // cannot be used.
    static bool parseManifest(const QByteArray &bytes, QVector<Entry> *entries, qint64 *total,
                              QString *published, QString *error);
    QString destination(const Entry &entry) const;
    // "Downloading the country charts, 12 of 72".
    QString stepText(const QString &verb) const;
    void next();
    void startVerify();
    void verifyStep(int generation);
    void startFetch();
    void pump();
    void finishFetch();
    void complete();
    void fail(const QString &message);
    void stopTransfers();
    // Remove's two halves: waiting until the recommender holds nothing in
    // the folder, then deleting it.
    void removeWhenReleased();
    void removeFiles();
    // Reads what is on disk: installed or partial, and how much. A folder
    // holding nothing worth keeping is deleted, so a failed first try leaves
    // no empty folders behind.
    void refreshDiskState(bool tidy = false);
    void setStatus(const QString &text);
    // Progress moves many times a second; the interface hears of it at most
    // ten times a second, and whenever the status line changes.
    void noteProgress();
    static QString describe(QNetworkReply *reply, const QString &what);

    QNetworkAccessManager *m_network = nullptr;
    QPointer<Recommender> m_recommender;
    QPointer<QNetworkReply> m_reply;

    // A download's own number: a verify slice or a queued pump from a
    // cancelled one is recognised as stale by it.
    int m_generation = 0;
    QVector<Entry> m_entries;
    int m_index = -1;
    qint64 m_totalBytes = 0;
    // Bytes of whole files, kept or fetched; plus the file under way.
    qint64 m_completedBytes = 0;
    qint64 m_currentBytes = 0;
    QFile m_file;               // the .part being written, or a file being checked
    QCryptographicHash m_hash{ QCryptographicHash::Sha256 };
    bool m_pumpQueued = false;
    QElapsedTimer m_sinceNotify;

    bool m_busy = false;
    bool m_installed = false;
    bool m_removing = false;
    bool m_failed = false;
    bool m_removeFailed = false;
    qint64 m_bytesOnDisk = 0;
    QString m_status;
    QString m_published;
    int m_kept = 0;
    int m_fetched = 0;
    int m_refused = 0;
};
