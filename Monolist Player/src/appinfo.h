#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>

class QNetworkAccessManager;
class QNetworkReply;
class QProcess;

// What this copy of the app is, and how it is kept current.
//
// Three separate questions live here because the user asked them as three:
// which build am I running, is there a newer one, and are the open-source
// tools it plays through up to date. The last is its own thing on purpose —
// what usually goes stale in a player like this is not the player, it is
// yt-dlp, and a version of yt-dlp from six months ago is the difference
// between a track playing and a track failing.
//
// Exposed to QML as the "About" singleton.
class AppInfo : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(QString buildNumber READ buildNumber CONSTANT)
    Q_PROPERTY(QString commit READ commit CONSTANT)
    Q_PROPERTY(QString buildDate READ buildDate CONSTANT)
    Q_PROPERTY(bool modified READ modified CONSTANT)
    Q_PROPERTY(QString qtVersion READ qtVersion CONSTANT)
    // "0.1 (build 29)" — the one line to quote in a bug report.
    Q_PROPERTY(QString fullVersion READ fullVersion CONSTANT)

    // The open-source tools this plays through, each {name, version, role}.
    // Empty until refreshComponents() has run: finding a version means
    // starting a process, which is not something to do while the app launches.
    Q_PROPERTY(QVariantList components READ components NOTIFY componentsChanged)
    Q_PROPERTY(bool componentsKnown READ componentsKnown NOTIFY componentsChanged)

    Q_PROPERTY(int updateState READ updateState NOTIFY updateChanged)
    Q_PROPERTY(QString updateMessage READ updateMessage NOTIFY updateChanged)
    Q_PROPERTY(QString updateUrl READ updateUrl NOTIFY updateChanged)
    // The two the interface actually asks about, named rather than compared
    // against an enum value that reads as a magic number in QML.
    Q_PROPERTY(bool checking READ checking NOTIFY updateChanged)
    Q_PROPERTY(bool updateAvailable READ updateAvailable NOTIFY updateChanged)

    Q_PROPERTY(int toolsState READ toolsState NOTIFY toolsChanged)
    Q_PROPERTY(QString toolsMessage READ toolsMessage NOTIFY toolsChanged)
    Q_PROPERTY(bool toolsBusy READ toolsBusy NOTIFY toolsChanged)
    Q_PROPERTY(bool canUpdateTools READ canUpdateTools CONSTANT)

public:
    // Idle covers "not asked yet"; the rest are what an answer can be.
    enum State {
        Idle = 0,
        Working,        // a request or a process is out
        UpToDate,
        Available,      // something newer exists
        NotConfigured,  // no release feed has been set, so there is nothing to ask
        Failed
    };
    Q_ENUM(State)

    explicit AppInfo(QObject *parent = nullptr);

    QString version() const;
    QString buildNumber() const;
    QString commit() const;
    QString buildDate() const;
    bool modified() const;
    QString qtVersion() const;
    QString fullVersion() const;

    QVariantList components() const { return m_components; }
    bool componentsKnown() const { return m_componentsKnown; }

    int updateState() const { return m_updateState; }
    QString updateMessage() const { return m_updateMessage; }
    QString updateUrl() const { return m_updateUrl; }
    bool checking() const { return m_updateState == Working; }
    bool updateAvailable() const { return m_updateState == Available; }

    int toolsState() const { return m_toolsState; }
    QString toolsMessage() const { return m_toolsMessage; }
    bool toolsBusy() const { return m_toolsState == Working; }
    bool canUpdateTools() const;

    // Where a release feed lives, if one has been set. Stored as the setting
    // `update.feed`; a GitHub releases API URL is the shape expected.
    static QString updateFeed();
    static void setUpdateFeed(const QString &url);

public Q_SLOTS:
    // Asks the versions of the bundled tools, one process each. Cheap enough
    // to do when the Settings page opens, too expensive to do at start-up.
    void refreshComponents();
    // Is there a newer Monolist? Answers NotConfigured when no feed is set,
    // which is the honest answer rather than a spinner that never resolves.
    void checkForUpdate();
    // The "advanced" update: brings the bundled open-source tools up to date
    // by running the same setup script that installed them.
    void updateTools();
    void openUpdatePage();
    // For a bug report: the version line, the build, and every component.
    QString report() const;
    // The same, on the clipboard, because nobody retypes a build number
    // correctly and a wrong one sends whoever reads it the wrong way.
    void copyReport();

Q_SIGNALS:
    void componentsChanged();
    void updateChanged();
    void toolsChanged();

private:
    void setUpdate(int state, const QString &message, const QString &url = {});
    void setTools(int state, const QString &message);
    // The version a tool prints, or empty when it cannot be run at all.
    static QString toolVersion(const QString &program, const QStringList &arguments);
    static QString setupScriptPath();

    QNetworkAccessManager *m_network = nullptr;
    QPointer<QNetworkReply> m_updateReply;
    QPointer<QProcess> m_toolsProcess;

    QVariantList m_components;
    bool m_componentsKnown = false;

    int m_updateState = Idle;
    QString m_updateMessage;
    QString m_updateUrl;

    int m_toolsState = Idle;
    QString m_toolsMessage;
};
