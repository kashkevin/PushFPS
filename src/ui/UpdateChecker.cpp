#include "veyra/ui/UpdateChecker.h"
#include "veyra/ui/UpdateVerify.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QProcess>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QVersionNumber>
#include <QDebug>

namespace veyra::ui {
namespace {
constexpr qsizetype maxBody = 128 * 1024;
QVersionNumber versionNumber(const QString& text, bool local) {
    static const QRegularExpression stable(QStringLiteral("^v?([0-9]+\\.[0-9]+\\.[0-9]+)$"));
    static const QRegularExpression installed(QStringLiteral("^v?([0-9]+\\.[0-9]+\\.[0-9]+)(?:\\.0)?(?:-[A-Za-z0-9.-]+)?$"));
    const auto match = (local ? installed : stable).match(text);
    return match.hasMatch() ? QVersionNumber::fromString(match.captured(1)) : QVersionNumber{};
}
}
UpdateChecker::UpdateChecker(QString currentVersion, QObject* parent, QUrl endpoint, int timeoutMs)
    : QObject(parent), currentVersion_(std::move(currentVersion)), endpoint_(std::move(endpoint)), timeoutMs_(timeoutMs) {
    publicKeyPem_ = QByteArray(veyra::brand::kUpdatePublicKeyPem);
    installDir_ = QCoreApplication::instance() ? QCoreApplication::applicationDirPath() : QString{};
    launch_ = [](const QString& program, const QStringList& args) { return QProcess::startDetached(program, args); };
    deadline_.setSingleShot(true);
    connect(&deadline_, &QTimer::timeout, this, [this] {
        if (reply_) { failure_ = QStringLiteral("timeout"); reply_->abort(); }
    });
}
// The installed version: the build's own version, or the installer's record (PushFPS-VERSION.txt) when
// that names a newer one. 3.1.3 and 3.1.4 were built with the Veyra version (2.1.9) as their own, so
// without the record they kept offering the update they had just installed.
QVersionNumber UpdateChecker::localVersion() const {
    QVersionNumber local = versionNumber(currentVersion_, true);
    if (!installDir_.isEmpty()) {
        QFile f(QDir(installDir_).filePath(QStringLiteral("PushFPS-VERSION.txt")));
        if (f.open(QIODevice::ReadOnly))
            if (const auto record = parseInstalledVersionFile(f.read(64 * 1024)); record && QVersionNumber::compare(record->version, local) > 0)
                local = record->version;
    }
    return local;
}
UpdateChecker::~UpdateChecker() {
    deadline_.stop();
    if (reply_) {
        reply_->disconnect(this);
        reply_->abort();
    }
    if (download_) {
        download_->disconnect(this);
        download_->abort();
    }
}
// Slipstream: true, with one log line, when this copy must not use the updater (UpdatePolicy.h).
bool UpdateChecker::refusedByPolicy() const {
    if (updatesAllowed(policy_)) return false;
    const auto reason = updatePolicyReason(policy_);
    qInfo().noquote() << "[update-check]" << QStringLiteral("disabled: ") + QString::fromLatin1(reason.data(), qsizetype(reason.size()));
    return true;
}
void UpdateChecker::startAutomaticCheck() {
    if (automaticScheduled_) return;
    if (refusedByPolicy()) { automaticScheduled_ = true; return; }
    automaticScheduled_ = true;
    QTimer::singleShot(2500, this, [this] {
        // A manual check made during startup already fulfils this run's check.
        if (!checking() && !manual_) check(false);
    });
}
void UpdateChecker::check(bool manual) {
    if (refusedByPolicy()) { emit completed(QStringLiteral("disabled")); return; }
    if (checking()) { manual_ = manual_ || manual; return; }
    manual_ = manual;
    body_.clear(); failure_.clear();
    QNetworkRequest request(endpoint_);
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("User-Agent", "PushFPS-Update-Check");
    request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setMaximumRedirectsAllowed(3);
    request.setTransferTimeout(timeoutMs_);
    reply_ = network_.get(request);
    qInfo().noquote() << "[update-check-start]" << (manual ? "manual" : "automatic");
    reply_->setReadBufferSize(maxBody + 1);
    connect(reply_, &QIODevice::readyRead, this, [this] {
        body_ += reply_->read(maxBody + 1 - body_.size());
        if (body_.size() > maxBody) { failure_ = QStringLiteral("response-too-large"); reply_->abort(); }
    });
    connect(reply_, &QNetworkReply::finished, this, &UpdateChecker::finish);
    deadline_.start(timeoutMs_);
    emit checkingChanged();
}
void UpdateChecker::finish() {
    deadline_.stop();
    auto* reply = reply_;
    if (reply->isOpen()) body_ += reply->read(maxBody + 1 - body_.size());
    if (body_.size() > maxBody) failure_ = QStringLiteral("response-too-large");
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (failure_.isEmpty() && (reply->error() != QNetworkReply::NoError || status != 200))
        failure_ = QStringLiteral("http-%1-network-%2").arg(status).arg(int(reply->error()));
    reply_ = nullptr;
    reply->deleteLater();
    QString result, versions;
    if (failure_.isEmpty()) {
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(body_, &error);
        const auto obj = document.object();
        const auto tag = obj.value(QStringLiteral("tag_name")).toString();
        const auto remote = versionNumber(tag, false), local = localVersion();
        // Both versions in the log: "an update is offered although I am on the latest" can then be
        // answered from a user's log file.
        versions = QStringLiteral(" installed=%1 build=%2 latest=%3").arg(local.toString(), currentVersion_, remote.toString());
        const QUrl url(obj.value(QStringLiteral("html_url")).toString());
        const bool validUrl = url.scheme() == QStringLiteral("https") && url.host() == QStringLiteral("github.com")
            && url.userInfo().isEmpty() && url.port(-1) == -1 && !url.hasQuery() && !url.hasFragment()
            && url.path() == QString::fromLatin1(veyra::brand::kReleaseTagPathPrefix) + tag;
        if (error.error != QJsonParseError::NoError || !document.isObject()
            || !obj.value(QStringLiteral("draft")).isBool() || obj.value(QStringLiteral("draft")).toBool()
            || !obj.value(QStringLiteral("prerelease")).isBool() || obj.value(QStringLiteral("prerelease")).toBool()
            || local.segmentCount() != 3 || remote.segmentCount() != 3 || !validUrl) {
            failure_ = QStringLiteral("invalid-release");
        } else if (QVersionNumber::compare(remote, local) > 0) {
            latestVersion_ = remote.toString(); releaseUrl_ = url; tag_ = tag;
            // Signed-update assets (checked again after download against the signed list).
            sumsUrl_.clear(); sigUrl_.clear(); zipUrl_.clear(); zipName_.clear();
            const QString wantZip = QStringLiteral("PushFPS-%1-win64.zip").arg(latestVersion_);
            for (const auto& v : obj.value(QStringLiteral("assets")).toArray()) {
                const auto a = v.toObject();
                const QString name = a.value(QStringLiteral("name")).toString();
                const QUrl link(a.value(QStringLiteral("browser_download_url")).toString());
                if (!assetUrlOk(link) || link.fileName() != name) continue;
                if (name == QStringLiteral("SHA256SUMS.txt")) sumsUrl_ = link;
                else if (name == QStringLiteral("SHA256SUMS.txt.sig")) sigUrl_ = link;
                else if (name == wantZip) { zipUrl_ = link; zipName_ = name; }
            }
            emit releaseChanged();
            emit updateAvailable(latestVersion_);
            result = QStringLiteral("available");
        } else {
            latestVersion_.clear(); releaseUrl_.clear(); sumsUrl_.clear(); sigUrl_.clear(); zipUrl_.clear(); emit releaseChanged();
            if (manual_) emit notice(tr("当前已是最新版本"), false);
            result = QStringLiteral("current");
        }
    }
    if (!failure_.isEmpty()) {
        if (manual_) emit notice(tr("检查更新失败，请检查网络后重试"), true);
        result = failure_;
    }
    body_.clear();
    qInfo().noquote() << "[update-check]" << result + versions;
    emit checkingChanged();
    emit completed(result);
}
void UpdateChecker::openRelease() {
    if (!releaseUrl_.isEmpty()) QDesktopServices::openUrl(releaseUrl_);
}

// ---- PushFPS signed updates -------------------------------------------------------------------
namespace {
constexpr qint64 kMaxSums = 256 * 1024, kMaxSig = 4096, kMaxZip = qint64(2) * 1024 * 1024 * 1024;
}
bool UpdateChecker::assetUrlOk(const QUrl& url) const {
    if (!url.isValid() || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()) return false;
    const QString path = url.path();
    if (!path.startsWith(QString::fromLatin1(veyra::brand::kReleaseDownloadPathPrefix) + tag_ + QLatin1Char('/'))) return false;
    if (allowHttp_) return url.scheme() == QStringLiteral("http") || url.scheme() == QStringLiteral("https");
    return url.scheme() == QStringLiteral("https") && url.host() == QStringLiteral("github.com") && url.port(-1) == -1;
}
bool UpdateChecker::canInstall() const {
    if (!updatesAllowed(policy_)) return false;
    if (publicKeyPem_.isEmpty() || sumsUrl_.isEmpty() || sigUrl_.isEmpty() || zipUrl_.isEmpty() || installDir_.isEmpty()) return false;
    QFile f(QDir(installDir_).filePath(QStringLiteral("PushFPS-VERSION.txt")));
    if (!f.open(QIODevice::ReadOnly)) return false;   // a portable or development copy: the release page instead
    return parseInstalledVersionFile(f.read(64 * 1024)).has_value();
}
void UpdateChecker::setInstall(const QString& state, double progress) {
    installState_ = state; installProgress_ = progress;
    emit installChanged();
}
void UpdateChecker::failInstall(const QString& reason) {
    if (download_) { download_->disconnect(this); download_->abort(); download_->deleteLater(); download_ = nullptr; }
    qInfo().noquote() << "[update-install] failed" << reason;
    setInstall(QStringLiteral("failed"));
    emit notice(reason == QStringLiteral("signature-mismatch") || reason == QStringLiteral("sha256-mismatch")
                    ? tr("更新文件没有通过签名或校验，已停止安装，当前版本未改动")
                    : tr("更新没有完成，当前版本未改动；可以在发布页手动下载"), true);
    emit installFinished(reason);
}
void UpdateChecker::download(const QUrl& url, qint64 limit, std::function<void(QByteArray)> done, QString file) {
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "PushFPS-Update");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setMaximumRedirectsAllowed(5);
    request.setTransferTimeout(60000);
    auto out = std::make_shared<QFile>(file);
    if (!file.isEmpty() && !out->open(QIODevice::WriteOnly | QIODevice::Truncate)) { failInstall(QStringLiteral("cannot-write")); return; }
    auto hash = std::make_shared<QCryptographicHash>(QCryptographicHash::Sha256);
    auto body = std::make_shared<QByteArray>();
    auto total = std::make_shared<qint64>(0);
    download_ = network_.get(request);
    auto* reply = download_;
    connect(reply, &QIODevice::readyRead, this, [=, this] {
        const QByteArray chunk = reply->readAll();
        *total += chunk.size();
        if (*total > limit) { failInstall(QStringLiteral("too-large")); return; }
        if (file.isEmpty()) *body += chunk;
        else { hash->addData(chunk); if (out->write(chunk) != chunk.size()) failInstall(QStringLiteral("cannot-write")); }
    });
    if (!file.isEmpty())
        connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64 got, qint64 all) {
            if (all > 0) setInstall(QStringLiteral("downloading"), double(got) / double(all));
        });
    connect(reply, &QNetworkReply::finished, this, [=, this] {
        if (download_ != reply) return;   // failed and replaced already
        download_ = nullptr;
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || status != 200) { failInstall(QStringLiteral("http-%1").arg(status)); return; }
        const QByteArray rest = reply->readAll();
        *total += rest.size();
        if (*total > limit) { failInstall(QStringLiteral("too-large")); return; }
        if (file.isEmpty()) { *body += rest; done(*body); }
        else { hash->addData(rest); out->write(rest); out->close(); done(hash->result().toHex()); }
    });
}
void UpdateChecker::installUpdate() {
    if (refusedByPolicy()) return;
    if (installState_ == QStringLiteral("downloading") || installState_ == QStringLiteral("verifying") || installState_ == QStringLiteral("installing")) return;
    if (!canInstall()) { openRelease(); return; }
    qInfo().noquote() << "[update-install] start" << latestVersion_;
    setInstall(QStringLiteral("verifying"));
    download(sumsUrl_, kMaxSums, [this](QByteArray sums) {
        sums_ = std::move(sums);
        download(sigUrl_, kMaxSig, [this](QByteArray signature) {
            QString why;
            if (!verifyUpdateSignature(publicKeyPem_, sums_, signature, &why)) { failInstall(why); return; }
            const auto package = updatePackageIn(sums_);
            if (!package || package->fileName != zipName_ || package->version != QVersionNumber::fromString(latestVersion_) ||
                QVersionNumber::compare(package->version, localVersion()) <= 0) {
                failInstall(QStringLiteral("package-mismatch")); return;
            }
            expectedZipSha_ = package->sha256;
            verifyAndDownloadZip();
        });
    });
}
void UpdateChecker::verifyAndDownloadZip() {
    stageDir_ = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).filePath(QStringLiteral("PushFPS-update-") + latestVersion_);
    QDir(stageDir_).removeRecursively();
    if (!QDir().mkpath(stageDir_)) { failInstall(QStringLiteral("cannot-write")); return; }
    const QString zip = QDir(stageDir_).filePath(zipName_);
    setInstall(QStringLiteral("downloading"), 0);
    download(zipUrl_, kMaxZip, [this, zip](QByteArray sha) {
        if (sha != expectedZipSha_) { QFile::remove(zip); failInstall(QStringLiteral("sha256-mismatch")); return; }
        qInfo().noquote() << "[update-install] verified" << zipName_ << "sha256" << sha;
        unpackAndLaunch();
    }, zip);
}
void UpdateChecker::unpackAndLaunch() {
    setInstall(QStringLiteral("installing"), 1);
    const QString zip = QDir(stageDir_).filePath(zipName_);
    const QString files = QDir(stageDir_).filePath(QStringLiteral("files"));
    QDir().mkpath(files);
    auto* tar = new QProcess(this);
    const QString tarExe = QDir(qEnvironmentVariable("SystemRoot", QStringLiteral("C:/Windows"))).filePath(QStringLiteral("System32/tar.exe"));
    connect(tar, &QProcess::finished, this, [this, tar, files](int code, QProcess::ExitStatus status) {
        tar->deleteLater();
        const QString script = QDir(files).filePath(QStringLiteral("Install-PushFPS.ps1"));
        if (status != QProcess::NormalExit || code != 0 || !QFileInfo::exists(script)) { failInstall(QStringLiteral("unpack")); return; }
        QFile f(QDir(installDir_).filePath(QStringLiteral("PushFPS-VERSION.txt")));
        const auto installed = f.open(QIODevice::ReadOnly) ? parseInstalledVersionFile(f.read(64 * 1024)) : std::nullopt;
        if (!installed) { failInstall(QStringLiteral("not-installed")); return; }
        const QStringList args{QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"), QStringLiteral("-File"),
                               QDir::toNativeSeparators(script), QStringLiteral("-Gpu"), installed->gpu, QStringLiteral("-InstallDir"),
                               QDir::toNativeSeparators(installDir_), QStringLiteral("-NoShortcuts"), QStringLiteral("-ExperimentalExtras"),
                               installed->extras ? QStringLiteral("Yes") : QStringLiteral("No"), QStringLiteral("-WaitForPid"),
                               QString::number(QCoreApplication::applicationPid())};
        if (!launch_(QStringLiteral("powershell.exe"), args)) { failInstall(QStringLiteral("launch")); return; }
        qInfo().noquote() << "[update-install] installer started for" << latestVersion_;
        emit installFinished(QStringLiteral("started"));
        emit quitForUpdate();
    });
    tar->start(QFileInfo::exists(tarExe) ? tarExe : QStringLiteral("tar"), {QStringLiteral("-xf"), QDir::toNativeSeparators(zip), QStringLiteral("-C"), QDir::toNativeSeparators(files)});
}
}
