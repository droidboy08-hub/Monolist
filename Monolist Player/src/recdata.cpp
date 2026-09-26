#include "recdata.h"

#include "appdatabase.h"
#include "recommender.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSqlQuery>
#include <QStorageInfo>
#include <QTimer>

namespace {

// The version of the data this build reads, which is also the git tag it is
// published under in droidboy08-hub/Monolist-data. To publish a new version:
// commit the new files there with a manifest.json saying "version": N (every
// file's path, size and SHA-256), tag that commit vN and push the tag, and
// only then raise this to N in the build that can read it. A tag is never
// moved or reused once pushed, so every build fetches exactly the files it
// was written for, whatever is published after it.
constexpr int kVersion = 1;
const QString kDefaultBase = QStringLiteral("https://raw.githubusercontent.com/droidboy08-hub/Monolist-data/");
const QString kManifestFile = QStringLiteral("manifest.json");
// What the manifest calls itself: an address that answers with some other
// JSON is the wrong address, not damaged data.
const QString kManifestName = QStringLiteral("monolist-recommendation-data");

// How much is read, hashed and written per turn of the event loop. A megabyte
// is a few milliseconds of SHA-256 even under emulation, so the interface
// never waits on it; and a reply is held to a few of these, so a 50 MB file is
// never sitting in memory whole.
constexpr qint64 kSlice = 1 << 20;
// A connection that sends nothing for this long is given up on.
constexpr int kTransferTimeoutMs = 60000;
// The manifest is 18 KB; anything past this is not one.
constexpr qint64 kManifestLimit = 4 << 20;
// Left free beyond what the download needs: a disk filled to the last byte
// fails in other programs first.
constexpr qint64 kSpareBytes = 64LL << 20;

QString tag()
{
    return QStringLiteral("v%1").arg(kVersion);
}

// "100.4 MB". Traditional units, so the 105,301,396 bytes of version 1 read
// as the "100 MB" the button promises.
QString sizeOf(qint64 bytes)
{
    return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}

// Every path the manifest names becomes a file written on this computer, so a
// path is taken only in the one shape the data has: a file directly inside
// one of its two folders. Nothing absolute, nothing with "..", nothing
// elsewhere.
bool safePath(const QString &path)
{
    static const QRegularExpression shape(
        QStringLiteral("^(EmbeddingData|GraphData)/[A-Za-z0-9][A-Za-z0-9._-]*$"));
    return shape.match(path).hasMatch() && !path.contains(QLatin1String(".."))
           && !path.endsWith(QLatin1String(".part"));
}

bool isSha256(const QByteArray &hex)
{
    static const QRegularExpression shape(QStringLiteral("^[0-9a-f]{64}$"));
    return shape.match(QString::fromLatin1(hex)).hasMatch();
}

QNetworkRequest makeRequest(const QUrl &url)
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Monolist/%1").arg(QCoreApplication::applicationVersion()));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(kTransferTimeoutMs);
    return request;
}

int httpStatus(QNetworkReply *reply)
{
    return reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
}

// Set while a Remove waits for the recommender to let go, and cleared once
// the files are gone. A quit in between would otherwise leave the files, and
// the next launch would find them whole with no folder set, and adopt them.
const QString kRemovePendingKey = QStringLiteral("rec.removePending");

bool removePending()
{
    QSqlQuery query(AppDatabase::connection());
    query.prepare(QStringLiteral("SELECT value FROM settings WHERE key = ?"));
    query.addBindValue(kRemovePendingKey);
    return query.exec() && query.next() && query.value(0).toString() == QLatin1String("1");
}

void setRemovePending(bool pending)
{
    QSqlQuery query(AppDatabase::connection());
    if (pending) {
        query.prepare(QStringLiteral("INSERT OR REPLACE INTO settings (key, value) VALUES (?, '1')"));
    } else {
        query.prepare(QStringLiteral("DELETE FROM settings WHERE key = ?"));
    }
    query.addBindValue(kRemovePendingKey);
    query.exec();
}

} // namespace

RecData::RecData(QObject *parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
{
    // A stat of each file, and a read of an 18 KB list: cheap enough for
    // start-up. The hashes are checked when the files arrive, not every launch.
    refreshDiskState();
}

// A download under way when the app quits leaves its last whole file behind
// and deletes the .part, which is what the next run expects to find.
RecData::~RecData()
{
    stopTransfers();
}

void RecData::setRecommender(Recommender *recommender)
{
    m_recommender = recommender;
    if (!recommender)
        return;
    // Whether the download is the folder in use follows the settings.
    connect(recommender, &Recommender::stateChanged, this, &RecData::changed);
    // A Remove the last run could not finish is finished now, before
    // anything could adopt the files again.
    if (removePending() && QFileInfo::exists(folderPath())) {
        qInfo("Monolist: recommendation data: finishing a removal the last run could not complete");
        m_removing = true;
        setStatus(QStringLiteral("Removing…"));
        removeWhenReleased();
        return;
    }
    setRemovePending(false);
    if (m_installed && recommender->dataDirectory().isEmpty() && recommender->graphDirectory().isEmpty())
        use();
}

// --------------------------------------------------------------- properties

bool RecData::inUse() const
{
    return m_recommender && Recommender::isInside(m_recommender->dataDirectory(), folderPath());
}

qreal RecData::progress() const
{
    if (m_totalBytes <= 0)
        return 0;
    const qint64 done = m_busy ? doneBytes() : m_bytesOnDisk;
    return qBound(0.0, qreal(done) / qreal(m_totalBytes), 1.0);
}

QString RecData::progressText() const
{
    const qint64 done = m_busy ? doneBytes() : m_bytesOnDisk;
    if (m_totalBytes <= 0)
        return sizeOf(done);
    return QStringLiteral("%1 of %2").arg(sizeOf(done), sizeOf(m_totalBytes));
}

QString RecData::sizeText() const
{
    return sizeOf(m_bytesOnDisk);
}

int RecData::version() const
{
    return kVersion;
}

QString RecData::displayFolder() const
{
    return QDir::toNativeSeparators(folderPath());
}

QString RecData::catalogueDirectory() const
{
    return QDir(folderPath()).filePath(QStringLiteral("EmbeddingData"));
}

QString RecData::baseUrl()
{
    QString base = qEnvironmentVariable("MONOLIST_REC_DATA_URL").trimmed();
    if (base.isEmpty())
        base = kDefaultBase + tag() + QLatin1Char('/');
    else if (!base.contains(QLatin1String("://")))
        base = QUrl::fromLocalFile(base).toString();   // a plain folder
    if (!base.endsWith(QLatin1Char('/')))
        base += QLatin1Char('/');
    return base;
}

// Beside the database, so MONOLIST_DATA_DIR moves it too and a test never
// touches the real one. Versioned, so the "GraphData beside the catalogue"
// rule finds the graph, and a later version never lands on top of this one.
QString RecData::folderPath()
{
    const QString data = QFileInfo(AppDatabase::databaseFilePath()).absolutePath();
    return QDir(data).filePath(QStringLiteral("recommendations/") + tag());
}

QUrl RecData::urlFor(const QString &path) const
{
    return QUrl(baseUrl()).resolved(QUrl(path));
}

QString RecData::destination(const Entry &entry) const
{
    return QDir(folderPath()).filePath(entry.path);
}

// ----------------------------------------------------------------- download

void RecData::download()
{
    if (m_busy || m_removing)
        return;
    ++m_generation;
    m_busy = true;
    m_failed = false;
    m_removeFailed = false;
    // Wanted again after all: a removal left unfinished is called off.
    setRemovePending(false);
    m_entries.clear();
    m_index = -1;
    m_completedBytes = 0;
    m_currentBytes = 0;
    m_kept = m_fetched = m_refused = 0;
    setStatus(QStringLiteral("Fetching the list of files…"));

    if (!QDir().mkpath(folderPath())) {
        fail(QStringLiteral("Could not create %1.").arg(displayFolder()));
        return;
    }

    QNetworkReply *reply = m_network->get(makeRequest(urlFor(kManifestFile)));
    m_reply = reply;
    connect(reply, &QNetworkReply::downloadProgress, this, [reply](qint64 received, qint64) {
        if (received > kManifestLimit) {
            reply->setProperty("tooLarge", true);
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply]() { manifestArrived(reply); });
}

bool RecData::parseManifest(const QByteArray &bytes, QVector<Entry> *entries, qint64 *total,
                            QString *published, QString *error)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (!document.isObject()) {
        *error = QStringLiteral("The list of files could not be read (%1).").arg(parseError.errorString());
        return false;
    }
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("name")).toString() != kManifestName) {
        *error = QStringLiteral("That address does not hold Monolist's recommendation data.");
        return false;
    }
    const int version = root.value(QStringLiteral("version")).toInt(-1);
    if (version != kVersion) {
        *error = QStringLiteral("That is version %1 of the recommendation data; this build reads version %2.")
                     .arg(version).arg(kVersion);
        return false;
    }

    const QJsonArray files = root.value(QStringLiteral("files")).toArray();
    QVector<Entry> out;
    QSet<QString> seen;
    qint64 sum = 0;
    for (const QJsonValue &value : files) {
        const QJsonObject file = value.toObject();
        Entry entry;
        entry.path = file.value(QStringLiteral("path")).toString();
        entry.size = file.value(QStringLiteral("size")).toInteger(-1);
        entry.sha256 = file.value(QStringLiteral("sha256")).toString().toLatin1();
        // One bad line refuses the whole list: a manifest that is wrong in
        // one place cannot be trusted in the others.
        if (!safePath(entry.path) || seen.contains(entry.path.toLower()) || entry.size <= 0
            || entry.size > (qint64(2) << 30) || !isSha256(entry.sha256)) {
            *error = QStringLiteral("The list of files names something it should not (%1), so none of it was used.")
                         .arg(entry.path.isEmpty() ? QStringLiteral("an empty path") : entry.path);
            return false;
        }
        seen.insert(entry.path.toLower());
        sum += entry.size;
        out.append(entry);
    }
    if (out.isEmpty()) {
        *error = QStringLiteral("The list of files is empty.");
        return false;
    }
    *entries = out;
    *total = sum;
    *published = root.value(QStringLiteral("published")).toString();
    return true;
}

void RecData::manifestArrived(QNetworkReply *reply)
{
    reply->deleteLater();
    if (m_reply != reply)
        return;                        // cancelled; already said
    m_reply = nullptr;

    if (reply->property("tooLarge").toBool()) {
        fail(QStringLiteral("%1 is far larger than a list of files, so it was not used.")
                 .arg(reply->url().toDisplayString()));
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        fail(describe(reply, QStringLiteral("The list of files")));
        return;
    }

    const QByteArray bytes = reply->readAll();
    QVector<Entry> entries;
    qint64 total = 0;
    QString published;
    QString error;
    if (!parseManifest(bytes, &entries, &total, &published, &error)) {
        fail(error);
        return;
    }
    m_entries = entries;
    m_totalBytes = total;
    m_published = published;

    // Asked before anything is written, so a full disk is a sentence up front
    // rather than a write error forty files in.
    qint64 present = 0;
    for (const Entry &entry : std::as_const(m_entries)) {
        const QFileInfo info(destination(entry));
        if (info.isFile() && info.size() == entry.size)
            present += entry.size;
    }
    const QStorageInfo storage(folderPath());
    const qint64 needed = total - present;
    if (storage.isValid() && storage.bytesAvailable() >= 0 && storage.bytesAvailable() < needed + kSpareBytes) {
        fail(QStringLiteral("This needs %1 of free space, and the disk holding %2 has %3.")
                 .arg(sizeOf(needed + kSpareBytes), displayFolder(), sizeOf(storage.bytesAvailable())));
        return;
    }

    // Kept first, so a download stopped part-way still knows what the whole
    // is when the app next starts. Nothing trusts it for more than that: a
    // file counts as here only at its listed size, and it only ever reaches
    // its name through the hash check below.
    QSaveFile list(QDir(folderPath()).filePath(kManifestFile));
    if (!list.open(QIODevice::WriteOnly) || list.write(bytes) != bytes.size() || !list.commit()) {
        fail(QStringLiteral("Could not write to %1: %2").arg(displayFolder(), list.errorString()));
        return;
    }
    next();
}

QString RecData::stepText(const QString &verb) const
{
    const Entry &entry = m_entries.at(m_index);
    const QString group = entry.path.section(QLatin1Char('/'), 0, 0);
    int of = 0;
    int at = 0;
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries.at(i).path.section(QLatin1Char('/'), 0, 0) != group)
            continue;
        ++of;
        if (i <= m_index)
            ++at;
    }
    const QString what = group == QLatin1String("EmbeddingData") ? QStringLiteral("the catalogue")
                                                                  : QStringLiteral("the country charts");
    return QStringLiteral("%1 %2, %3 of %4").arg(verb, what).arg(at).arg(of);
}

void RecData::next()
{
    ++m_index;
    m_currentBytes = 0;
    if (m_index >= m_entries.size()) {
        complete();
        return;
    }

    const Entry &entry = m_entries.at(m_index);
    const QString target = destination(entry);
    // A .part from a run that was stopped is never continued: it ends
    // wherever the run did, and fetching one file again is cheaper than
    // reasoning about how much of it can be trusted.
    QFile::remove(target + QStringLiteral(".part"));

    const QFileInfo info(target);
    if (info.isFile() && info.size() == entry.size) {
        startVerify();
        return;
    }
    if (info.exists() && !QFile::remove(target)) {
        fail(QStringLiteral("%1 is the wrong size and could not be replaced.")
                 .arg(QDir::toNativeSeparators(target)));
        return;
    }
    startFetch();
}

// A file already here at the right size is hashed before it is kept: the
// size says it was finished, not that it is intact. Read a slice per turn of
// the event loop, like a download, so the check never holds the interface.
void RecData::startVerify()
{
    const Entry &entry = m_entries.at(m_index);
    m_file.setFileName(destination(entry));
    if (!m_file.open(QIODevice::ReadOnly)) {
        startFetch();                  // unreadable: fetched again, and replaced
        return;
    }
    m_hash.reset();
    m_currentBytes = 0;
    setStatus(stepText(QStringLiteral("Checking")));
    const int generation = m_generation;
    QTimer::singleShot(0, this, [this, generation]() { verifyStep(generation); });
}

void RecData::verifyStep(int generation)
{
    if (generation != m_generation || !m_busy)
        return;                        // cancelled, or a newer download
    const QByteArray chunk = m_file.read(kSlice);
    if (!chunk.isEmpty()) {
        m_hash.addData(chunk);
        m_currentBytes += chunk.size();
        noteProgress();
        if (generation != m_generation)
            return;                    // cancelled from a progress handler
        if (!m_file.atEnd()) {
            QTimer::singleShot(0, this, [this, generation]() { verifyStep(generation); });
            return;
        }
    }
    m_file.close();

    const Entry &entry = m_entries.at(m_index);
    if (m_currentBytes == entry.size && m_hash.result().toHex() == entry.sha256) {
        ++m_kept;
        m_completedBytes += entry.size;
        m_currentBytes = 0;
        next();
        return;
    }

    // Here at the right size with the wrong contents: damaged since, or never
    // this file. Refused, and fetched afresh in its place.
    qWarning("Monolist: recommendation data: %s does not match its checksum; fetching it again",
             qPrintable(entry.path));
    ++m_refused;
    m_currentBytes = 0;
    if (!QFile::remove(destination(entry))) {
        fail(QStringLiteral("%1 is damaged and could not be replaced.")
                 .arg(QDir::toNativeSeparators(destination(entry))));
        return;
    }
    startFetch();
}

void RecData::startFetch()
{
    const Entry &entry = m_entries.at(m_index);
    const QString part = destination(entry) + QStringLiteral(".part");
    QDir().mkpath(QFileInfo(part).absolutePath());
    m_file.setFileName(part);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail(QStringLiteral("Could not write to %1: %2")
                 .arg(QDir::toNativeSeparators(part), m_file.errorString()));
        return;
    }
    m_hash.reset();
    m_currentBytes = 0;
    setStatus(stepText(QStringLiteral("Downloading")));

    QNetworkReply *reply = m_network->get(makeRequest(urlFor(entry.path)));
    // Held to a few slices: past that the reply stops reading from the
    // network until pump() catches up, instead of buffering the whole file.
    reply->setReadBufferSize(kSlice * 4);
    m_reply = reply;
    connect(reply, &QNetworkReply::readyRead, this, &RecData::pump);
    connect(reply, &QNetworkReply::finished, this, &RecData::pump);
}

// Takes one slice from the reply: into the .part, and into the hash, so the
// file is checked as it arrives rather than read a second time afterwards.
// Called for each readyRead and for finished; while more is waiting it
// queues itself for the next turn of the event loop rather than looping, and
// once nothing is waiting and the reply is finished, the file is judged.
void RecData::pump()
{
    QNetworkReply *reply = m_reply;
    if (!reply || !m_busy)
        return;

    const QByteArray chunk = reply->read(kSlice);
    // An error page — GitHub's 404 — is read and dropped, never written: it
    // is not the file, and finished says what went wrong.
    if (!chunk.isEmpty() && reply->error() == QNetworkReply::NoError && httpStatus(reply) < 400) {
        const Entry &entry = m_entries.at(m_index);
        m_currentBytes += chunk.size();
        if (m_currentBytes > entry.size) {
            ++m_refused;
            fail(QStringLiteral("%1 came out larger than the list of files says, so it was refused. "
                                "Downloading again fetches it afresh.").arg(entry.path));
            return;
        }
        if (m_file.write(chunk) != chunk.size()) {
            fail(QStringLiteral("Could not write %1: %2").arg(entry.path, m_file.errorString()));
            return;
        }
        m_hash.addData(chunk);
        noteProgress();
        if (m_reply != reply)
            return;                    // cancelled from a progress handler
    }

    if (reply->bytesAvailable() > 0) {
        if (!m_pumpQueued) {
            m_pumpQueued = true;
            QTimer::singleShot(0, this, [this]() {
                m_pumpQueued = false;
                pump();
            });
        }
        return;
    }
    if (reply->isFinished())
        finishFetch();
}

void RecData::finishFetch()
{
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    disconnect(reply, nullptr, this, nullptr);
    reply->deleteLater();
    m_file.close();

    const Entry &entry = m_entries.at(m_index);
    const QString target = destination(entry);
    const QString part = target + QStringLiteral(".part");

    if (reply->error() != QNetworkReply::NoError || httpStatus(reply) >= 400) {
        QFile::remove(part);
        fail(describe(reply, entry.path));
        return;
    }
    // Both checked on what arrived, never on what the server said it would
    // send: a short read, or a file changed in transit, is refused here.
    if (m_currentBytes != entry.size) {
        ++m_refused;
        QFile::remove(part);
        fail(QStringLiteral("%1 arrived as %2 bytes where the list of files says %3, so it was refused. "
                            "Downloading again fetches it afresh.")
                 .arg(entry.path).arg(m_currentBytes).arg(entry.size));
        return;
    }
    if (m_hash.result().toHex() != entry.sha256) {
        ++m_refused;
        QFile::remove(part);
        fail(QStringLiteral("%1 did not match its checksum, so it was refused. Downloading again "
                            "fetches it afresh; if this keeps happening, the copy online is damaged.")
                 .arg(entry.path));
        return;
    }
    if (QFile::exists(target) && !QFile::remove(target)) {
        QFile::remove(part);
        fail(QStringLiteral("Could not replace %1, which may be in use.").arg(QDir::toNativeSeparators(target)));
        return;
    }
    if (!QFile::rename(part, target)) {
        QFile::remove(part);
        fail(QStringLiteral("Could not put %1 in place.").arg(QDir::toNativeSeparators(target)));
        return;
    }
    ++m_fetched;
    m_completedBytes += entry.size;
    m_currentBytes = 0;
    next();
}

void RecData::complete()
{
    m_busy = false;
    refreshDiskState();
    if (!m_installed) {
        // Every file passed, so one went missing while the rest arrived.
        m_failed = true;
        setStatus(QStringLiteral("Some files disappeared from %1 while the rest arrived. Downloading "
                                 "again fetches them.").arg(displayFolder()));
        Q_EMIT finished(false);
        return;
    }
    qInfo("Monolist: recommendation data %s is here: %d files kept, %d downloaded, %d refused",
          qPrintable(tag()), m_kept, m_fetched, m_refused);
    setStatus(QStringLiteral("Downloaded and checked: %1 files, %2.")
                  .arg(m_entries.size()).arg(sizeOf(m_totalBytes)));
    use();
    Q_EMIT finished(true);
}

void RecData::fail(const QString &message)
{
    stopTransfers();
    m_busy = false;
    m_failed = true;
    refreshDiskState(/*tidy=*/true);
    qWarning("Monolist: recommendation data: %s", qPrintable(message));
    setStatus(message);
    Q_EMIT finished(false);
}

void RecData::cancel()
{
    if (!m_busy)
        return;
    stopTransfers();
    m_busy = false;
    m_failed = false;
    refreshDiskState(/*tidy=*/true);
    setStatus(partial() ? QStringLiteral("Stopped. What is here is kept, and downloading again carries on from it.")
                        : QStringLiteral("Stopped."));
    Q_EMIT finished(false);
}

void RecData::stopTransfers()
{
    ++m_generation;                    // stale verify slices stop at once
    if (QNetworkReply *reply = m_reply) {
        m_reply = nullptr;
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
    if (m_file.isOpen()) {
        const bool writing = m_file.openMode().testFlag(QIODevice::WriteOnly);
        const QString name = m_file.fileName();
        m_file.close();
        // A .part is never continued, so none is left behind.
        if (writing)
            QFile::remove(name);
    }
    m_currentBytes = 0;
}

// ------------------------------------------------------------------- remove

void RecData::remove()
{
    if (m_busy || m_removing)
        return;
    m_removing = true;
    m_failed = false;
    m_removeFailed = false;
    setRemovePending(true);
    setStatus(QStringLiteral("Removing…"));
    removeWhenReleased();
}

// The catalogue is memory-mapped and the charts are open SQLite files:
// Windows will not delete either while they are held. So the recommender is
// pointed away first, and the files go once its worker has let go: when no
// setting points into the folder and no load is still queued behind a build,
// since until that load runs the worker holds whatever it read before. Asked
// again after each load, in case another pointed back in meanwhile.
void RecData::removeWhenReleased()
{
    if (m_recommender) {
        const bool released = m_recommender->release(folderPath());
        if (released || m_recommender->loadPending()) {
            connect(m_recommender, &Recommender::dataLoaded, this, &RecData::removeWhenReleased,
                    Qt::SingleShotConnection);
            return;
        }
    }
    removeFiles();
}

void RecData::removeFiles()
{
    const QString folder = folderPath();
    const QFileInfo manifest(QDir(folder).filePath(kManifestFile));
    // The list of files goes last, and only once everything else has: what
    // could not be deleted (held open by another program) then still reads
    // as a download part-way, with Remove, rather than as nothing at all.
    QStringList files;
    QDirIterator found(folder, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (found.hasNext())
        files << found.next();
    bool removed = true;
    for (const QString &path : std::as_const(files)) {
        if (QFileInfo(path) == manifest)
            continue;
        QFile file(path);
        if (!file.remove()) {
            // A read-only file, as removeRecursively deals with it.
            file.setPermissions(file.permissions() | QFileDevice::WriteOwner);
            if (!file.remove())
                removed = false;
        }
    }
    // Only the version folder this made, and its parent if that is now empty
    // (rmdir leaves a folder with anything else in it alone).
    if (removed)
        removed = QDir(folder).removeRecursively();
    QDir().rmdir(QFileInfo(folder).absolutePath());
    // Left set when some of it stayed, so the next launch finishes the job
    // rather than finding the files and putting them back to use.
    if (removed)
        setRemovePending(false);
    m_removing = false;
    refreshDiskState();
    m_failed = !removed;
    m_removeFailed = !removed;
    setStatus(removed ? QStringLiteral("Removed.")
                      : QStringLiteral("Some of it could not be removed; what is left is in %1.")
                            .arg(displayFolder()));
}

void RecData::use()
{
    // Not while a Remove waits to delete it.
    if (!m_installed || !m_recommender || m_removing)
        return;
    // The graph is left to the "beside the catalogue" rule: the two were
    // built as a pair, and GraphData sits next to EmbeddingData here.
    m_recommender->useDirectories(catalogueDirectory(), QString());
    Q_EMIT changed();
}

// -------------------------------------------------------------------- state

void RecData::refreshDiskState(bool tidy)
{
    m_installed = false;
    m_bytesOnDisk = 0;
    const QDir dir(folderPath());
    if (!dir.exists()) {
        if (!m_busy)
            m_published.clear();
        return;
    }

    QFile list(dir.filePath(kManifestFile));
    QVector<Entry> entries;
    qint64 total = 0;
    QString published;
    QString error;
    if (list.open(QIODevice::ReadOnly) && list.size() <= kManifestLimit
        && parseManifest(list.readAll(), &entries, &total, &published, &error)) {
        bool whole = true;
        for (const Entry &entry : std::as_const(entries)) {
            const QFileInfo info(dir.filePath(entry.path));
            if (info.isFile() && info.size() == entry.size)
                m_bytesOnDisk += entry.size;
            else
                whole = false;
        }
        m_installed = whole;
        m_totalBytes = total;
        m_published = published;
    }

    if (tidy && !m_installed && m_bytesOnDisk == 0) {
        list.close();
        QDir(dir.path()).removeRecursively();
        QDir().rmdir(QFileInfo(dir.path()).absolutePath());
    }
}

void RecData::setStatus(const QString &text)
{
    m_status = text;
    m_sinceNotify.restart();
    Q_EMIT changed();
}

void RecData::noteProgress()
{
    if (m_sinceNotify.isValid() && m_sinceNotify.elapsed() < 100)
        return;
    m_sinceNotify.restart();
    Q_EMIT changed();
}

// `what` opens the sentence: "The list of files", or a file's path.
QString RecData::describe(QNetworkReply *reply, const QString &what)
{
    const int status = httpStatus(reply);
    const QString where = reply->url().toDisplayString();
    if (status == 404 || reply->error() == QNetworkReply::ContentNotFoundError) {
        return QStringLiteral("%1 is not at %2. The data may not have been published there yet; "
                              "try again later.").arg(what, where);
    }
    // Cancel never gets here (it disconnects first), so a cancelled reply is
    // the transfer timeout giving up on a silent connection.
    if (reply->error() == QNetworkReply::OperationCanceledError
        || reply->error() == QNetworkReply::TimeoutError) {
        return QStringLiteral("%1 stopped arriving: the connection went quiet. Downloading again "
                              "carries on from the last whole file.").arg(what);
    }
    if (status >= 400)
        return QStringLiteral("%1 was refused by the server (HTTP %2).").arg(what).arg(status);
    return QStringLiteral("%1 could not be downloaded: %2").arg(what, reply->errorString());
}
