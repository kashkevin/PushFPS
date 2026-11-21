#pragma once
#include <QVersionNumber>
#include <QObject>
#include <QByteArray>
#include <QUrl>
#include <QTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QStringList>
#include <functional>
#include "veyra/Brand.h"
#include "veyra/ui/UpdatePolicy.h"

namespace veyra::ui {
// UI-only, bounded asynchronous metadata request; never touches the engine.
class UpdateChecker final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool checking READ checking NOTIFY checkingChanged)
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY releaseChanged)
    Q_PROPERTY(QUrl releaseUrl READ releaseUrl NOTIFY releaseChanged)
    // PushFPS signed updates: one-click install when the release is signed (UpdateVerify.h).
    Q_PROPERTY(bool canInstall READ canInstall NOTIFY releaseChanged)
    Q_PROPERTY(QString installState READ installState NOTIFY installChanged)
    Q_PROPERTY(double installProgress READ installProgress NOTIFY installChanged)
public:
    explicit UpdateChecker(QString currentVersion, QObject* parent = nullptr,
        QUrl endpoint = QUrl(QString::fromLatin1(veyra::brand::kLatestReleaseApi)),
        int timeoutMs = 12000);
    ~UpdateChecker() override;
    bool checking() const { return reply_ != nullptr; }
    QString latestVersion() const { return latestVersion_; }
    QUrl releaseUrl() const { return releaseUrl_; }
    Q_INVOKABLE void startAutomaticCheck();
    Q_INVOKABLE void check(bool manual = true);
    Q_INVOKABLE void openRelease();
    bool canInstall() const;
    QString installState() const { return installState_; }
    double installProgress() const { return installProgress_; }
    // Downloads SHA256SUMS.txt and its signature, verifies them with Brand.h's public key, downloads
    // the zip, checks its SHA-256, unpacks it and starts its installer for this install folder; then
    // asks the app to quit (quitForUpdate). Any failure leaves the installed version untouched.
    Q_INVOKABLE void installUpdate();
    // Tests: a different public key, install folder and installer launcher.
    void setUpdateKeyForTests(QByteArray pem) { publicKeyPem_ = std::move(pem); }
    void setInstallDirForTests(QString dir) { installDir_ = std::move(dir); }
    void setLaunchInstallerForTests(std::function<bool(const QString&, const QStringList&)> launch) { launch_ = std::move(launch); }
    void setAllowHttpForTests(bool allow) { allowHttp_ = allow; }
    // Slipstream (UpdatePolicy.h): with any policy but Enabled this object never asks the release
    // server, never offers a release and never installs one. Enabled, the default, is every official
    // build. Set once, right after construction (QmlPlayerBridge).
    void setPolicy(UpdatePolicy policy) { policy_ = policy; }
    UpdatePolicy policy() const { return policy_; }
signals:
    void checkingChanged();
    void releaseChanged();
    void updateAvailable(const QString& version);
    void notice(const QString& message, bool isError);
    // Terminal diagnostic result (also used by deterministic/real HTTP tests).
    void completed(const QString& result);
    void installChanged();
    void quitForUpdate();
    void installFinished(const QString& result);   // "started" or the failure reason
private:
    QVersionNumber localVersion() const;
    bool refusedByPolicy() const;
    UpdatePolicy policy_ = UpdatePolicy::Enabled;
    void finish();
    void setInstall(const QString& state, double progress = 0);
    void failInstall(const QString& reason);
    void download(const QUrl& url, qint64 limit, std::function<void(QByteArray)> done, QString file = {});
    void verifyAndDownloadZip();
    void unpackAndLaunch();
    bool assetUrlOk(const QUrl& url) const;
    QUrl sumsUrl_, sigUrl_, zipUrl_;
    QString zipName_, installState_ = QStringLiteral("idle"), installDir_, stageDir_, tag_;
    double installProgress_ = 0;
    QByteArray sums_, publicKeyPem_, expectedZipSha_;
    QNetworkReply* download_ = nullptr;
    std::function<bool(const QString&, const QStringList&)> launch_;
    bool allowHttp_ = false;
    QNetworkAccessManager network_;
    QTimer deadline_;
    QNetworkReply* reply_ = nullptr;
    QString currentVersion_, latestVersion_;
    QUrl endpoint_, releaseUrl_;
    QByteArray body_;
    QString failure_;
    int timeoutMs_;
    bool automaticScheduled_ = false, manual_ = false;
};
}
