#include "secretstore.h"

#include <QtGlobal>

// The Mac store: the user's login Keychain, one generic password per secret.
// The whole file is Mac-only, and CMake adds it there with
// MONOLIST_HAVE_KEYCHAIN, which keeps secretstore.cpp's "unavailable" answers
// out of the build.
#ifdef MONOLIST_HAVE_KEYCHAIN

#include "appdatabase.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

namespace {

// Far above anything kept here (a cookie jar is tens of kilobytes), as on
// Windows.
constexpr qsizetype kMaxBytes = 4 * 1024 * 1024 - 1024;

// Every item belongs to one data folder, as the Windows store's files sit
// beside the database: the copy in daily use (Application Support) keeps the
// bundle id as its service, and any other folder (MONOLIST_DATA_DIR, which
// every self-test sets) has its path after it. A test run can then never
// read, replace or remove the secrets of the library in use.
QString service()
{
    const QString base = QStringLiteral("io.github.droidboy08-hub.monolist");
    const QString folder = QFileInfo(AppDatabase::databaseFilePath()).absolutePath();
    const QString usual = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (QDir::cleanPath(folder) == QDir::cleanPath(usual))
        return base;
    return base + QStringLiteral(":") + QDir::cleanPath(folder);
}

// A CFString made from a QString, released when it goes out of scope.
class CFText
{
public:
    explicit CFText(const QString &text) : m_ref(text.toCFString()) {}
    ~CFText() { if (m_ref) CFRelease(m_ref); }
    CFText(const CFText &) = delete;
    CFText &operator=(const CFText &) = delete;
    CFStringRef get() const { return m_ref; }
private:
    CFStringRef m_ref;
};

// A mutable dictionary naming one item: its class, service and account. Each
// call adds to it what it needs.
class Query
{
public:
    explicit Query(const QString &name)
        : m_dict(CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                           &kCFTypeDictionaryValueCallBacks)),
          m_service(service()), m_account(name)
    {
        CFDictionarySetValue(m_dict, kSecClass, kSecClassGenericPassword);
        CFDictionarySetValue(m_dict, kSecAttrService, m_service.get());
        CFDictionarySetValue(m_dict, kSecAttrAccount, m_account.get());
    }
    ~Query() { CFRelease(m_dict); }
    Query(const Query &) = delete;
    Query &operator=(const Query &) = delete;
    CFMutableDictionaryRef get() const { return m_dict; }
private:
    CFMutableDictionaryRef m_dict;
    CFText m_service;
    CFText m_account;
};

// The Keychain's own sentence for a status, and its number.
QString keychainError(OSStatus status)
{
    QString message;
    if (CFStringRef text = SecCopyErrorMessageString(status, nullptr)) {
        message = QString::fromCFString(text);
        CFRelease(text);
    }
    return QStringLiteral("%1 (Keychain error %2)")
        .arg(message.isEmpty() ? QStringLiteral("unknown error") : message)
        .arg(int(status));
}

void setError(QString *error, const QString &text)
{
    if (error)
        *error = text;
}

}

bool SecretStore::available()
{
    return true;
}

QString SecretStore::backendName()
{
    return QStringLiteral("macOS Keychain");
}

QString SecretStore::unavailableReason()
{
    return QString();
}

// The Keychain keeps them; there is no folder of ours.
QString SecretStore::folderPath()
{
    return QString();
}

SecretStore::Status SecretStore::platformWrite(const QString &name, const QByteArray &secret, QString *error)
{
    if (secret.size() > kMaxBytes) {
        setError(error, QStringLiteral("too large to keep (%1 bytes)").arg(secret.size()));
        return Status::Failed;
    }
    CFDataRef data = CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(secret.constData()),
                                  CFIndex(secret.size()));

    // Replaced in place when it is there, so the item keeps the access the
    // user gave it; added when it is not.
    Query match(name);
    CFMutableDictionaryRef changes = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                                                               &kCFTypeDictionaryKeyCallBacks,
                                                               &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(changes, kSecValueData, data);
    OSStatus status = SecItemUpdate(match.get(), changes);
    CFRelease(changes);

    if (status == errSecItemNotFound) {
        Query add(name);
        // What Keychain Access lists it as.
        const CFText label(QStringLiteral("Monolist: ") + name);
        CFDictionarySetValue(add.get(), kSecAttrLabel, label.get());
        CFDictionarySetValue(add.get(), kSecValueData, data);
        // The login keychain's default access: this app reads it without a
        // question; another app, or this one rebuilt with a new signature,
        // is asked about by macOS first.
        status = SecItemAdd(add.get(), nullptr);
    }
    CFRelease(data);

    if (status != errSecSuccess) {
        setError(error, QStringLiteral("could not save: %1").arg(keychainError(status)));
        return Status::Failed;
    }
    return Status::Ok;
}

SecretStore::Status SecretStore::platformRead(const QString &name, QByteArray *secret, QString *error)
{
    Query query(name);
    CFDictionarySetValue(query.get(), kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(query.get(), kSecMatchLimit, kSecMatchLimitOne);

    CFTypeRef result = nullptr;
    const OSStatus status = SecItemCopyMatching(query.get(), &result);
    if (status == errSecItemNotFound)
        return Status::NotFound;
    if (status != errSecSuccess) {
        // Refused, not damaged: the user said Deny to the Keychain's question,
        // or the keychain is locked. The item itself is untouched.
        setError(error, QStringLiteral("could not read: %1").arg(keychainError(status)));
        if (result)
            CFRelease(result);
        return Status::Failed;
    }
    if (!result || CFGetTypeID(result) != CFDataGetTypeID()) {
        if (result)
            CFRelease(result);
        setError(error, QStringLiteral("the Keychain item holds no data"));
        return Status::Corrupt;
    }
    const auto data = static_cast<CFDataRef>(result);
    *secret = QByteArray(reinterpret_cast<const char *>(CFDataGetBytePtr(data)), qsizetype(CFDataGetLength(data)));
    CFRelease(result);
    return Status::Ok;
}

SecretStore::Status SecretStore::platformRemove(const QString &name, QString *error)
{
    Query query(name);
    const OSStatus status = SecItemDelete(query.get());
    if (status == errSecSuccess || status == errSecItemNotFound)
        return Status::Ok;
    setError(error, QStringLiteral("could not delete: %1").arg(keychainError(status)));
    return Status::Failed;
}

#endif // MONOLIST_HAVE_KEYCHAIN
