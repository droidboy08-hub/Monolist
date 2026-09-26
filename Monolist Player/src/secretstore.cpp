#include "secretstore.h"

#include <QRegularExpression>
#include <QStringList>
#include <QtGlobal>

bool SecretStore::validName(const QString &name)
{
    static const QRegularExpression shape(QStringLiteral("^[a-z0-9][a-z0-9._-]{0,63}$"));
    if (!shape.match(name).hasMatch())
        return false;
    // Windows opens the device, not a file, for these, whatever follows the
    // first dot: "con.session" would be the console.
    static const QStringList devices = {
        QStringLiteral("con"), QStringLiteral("prn"), QStringLiteral("aux"), QStringLiteral("nul"),
        QStringLiteral("com1"), QStringLiteral("com2"), QStringLiteral("com3"), QStringLiteral("com4"),
        QStringLiteral("com5"), QStringLiteral("com6"), QStringLiteral("com7"), QStringLiteral("com8"),
        QStringLiteral("com9"), QStringLiteral("lpt1"), QStringLiteral("lpt2"), QStringLiteral("lpt3"),
        QStringLiteral("lpt4"), QStringLiteral("lpt5"), QStringLiteral("lpt6"), QStringLiteral("lpt7"),
        QStringLiteral("lpt8"), QStringLiteral("lpt9")
    };
    return !devices.contains(name.section(QLatin1Char('.'), 0, 0));
}

QString SecretStore::statusText(Status status)
{
    switch (status) {
    case Status::Ok:          return QStringLiteral("ok");
    case Status::NotFound:    return QStringLiteral("not found");
    case Status::Corrupt:     return QStringLiteral("corrupt");
    case Status::Unavailable: return QStringLiteral("unavailable");
    case Status::Failed:      return QStringLiteral("failed");
    }
    return QStringLiteral("unknown");
}

namespace {
SecretStore::Status refuseName(const QString &name, QString *error)
{
    if (error)
        *error = QStringLiteral("\"%1\" is not a valid secret name").arg(name);
    return SecretStore::Status::Failed;
}
}

SecretStore::Status SecretStore::write(const QString &name, const QByteArray &secret, QString *error)
{
    if (error)
        error->clear();
    if (!validName(name))
        return refuseName(name, error);
    return platformWrite(name, secret, error);
}

SecretStore::Status SecretStore::read(const QString &name, QByteArray *secret, QString *error)
{
    if (error)
        error->clear();
    QByteArray value;
    const Status status = validName(name) ? platformRead(name, &value, error) : refuseName(name, error);
    if (secret)
        *secret = status == Status::Ok ? value : QByteArray();
    return status;
}

SecretStore::Status SecretStore::remove(const QString &name, QString *error)
{
    if (error)
        error->clear();
    if (!validName(name))
        return refuseName(name, error);
    return platformRemove(name, error);
}

// — no store on this platform —
//
// Linux gets libsecret later. Until a platform has a real store, nothing is
// kept at all: a secret in a plain file beside the database would be exactly
// the thing this class exists to prevent.
#if !defined(Q_OS_WIN) && !defined(MONOLIST_HAVE_KEYCHAIN)

bool SecretStore::available()
{
    return false;
}

QString SecretStore::backendName()
{
    return QString();
}

QString SecretStore::unavailableReason()
{
    return QStringLiteral("Keeping a sign-in safely is not available on this platform yet.");
}

QString SecretStore::folderPath()
{
    return QString();
}

SecretStore::Status SecretStore::platformWrite(const QString &, const QByteArray &, QString *error)
{
    if (error)
        *error = unavailableReason();
    return Status::Unavailable;
}

SecretStore::Status SecretStore::platformRead(const QString &, QByteArray *, QString *error)
{
    if (error)
        *error = unavailableReason();
    return Status::Unavailable;
}

SecretStore::Status SecretStore::platformRemove(const QString &, QString *error)
{
    if (error)
        *error = unavailableReason();
    return Status::Unavailable;
}

#endif
