// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// PushFPS signed updates: the OpenSSL-made fixture signature verified with Windows CNG, the signed
// checksum list, the install record, and the whole install flow against a local HTTP server
// (download, signature, SHA-256, unpack with tar, installer start), including tampered files.
#include "veyra/ui/UpdateChecker.h"
#include "veyra/ui/UpdateVerify.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>
#include <map>
#include <memory>

using namespace veyra::ui;

namespace {
QByteArray fixture(const char* name) {
    QFile f(QStringLiteral(VEYRA_UPDATE_TEST_DATA "/") + QString::fromLatin1(name));
    if (!f.open(QIODevice::ReadOnly)) qFatal("missing fixture %s", name);
    return f.readAll();
}
// Serves fixed paths over HTTP/1.1 (one request per connection).
class Server : public QTcpServer {
public:
    std::map<QByteArray, QByteArray> files;
    Server() {
        if (!listen(QHostAddress::LocalHost)) qFatal("Cannot listen on localhost");
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto* socket = nextPendingConnection();
                auto header = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket, header] {
                    *header += socket->readAll();
                    if (!header->contains("\r\n\r\n")) return;
                    socket->disconnect(socket, &QTcpSocket::readyRead, nullptr, nullptr);
                    const QByteArray path = header->split(' ').value(1);
                    const auto it = files.find(path);
                    const QByteArray body = it == files.end() ? QByteArray("missing") : it->second;
                    socket->write(QByteArray(it == files.end() ? "HTTP/1.1 404 No\r\n" : "HTTP/1.1 200 OK\r\n") + "Content-Length: " +
                                  QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            }
        });
    }
    QByteArray base() const { return "http://127.0.0.1:" + QByteArray::number(serverPort()); }
};
} // namespace

class UpdateVerifyTests : public QObject {
    Q_OBJECT
private slots:
    void opensslSignatureVerifies() {
        QString why;
        QVERIFY2(verifyUpdateSignature(fixture("test-only-public.pem"), fixture("SHA256SUMS.txt"), fixture("SHA256SUMS.txt.sig"), &why), qPrintable(why));
    }
    void tamperingIsRefused() {
        QByteArray sums = fixture("SHA256SUMS.txt");
        sums[0] = sums[0] == 'a' ? 'b' : 'a';
        QString why;
        QVERIFY(!verifyUpdateSignature(fixture("test-only-public.pem"), sums, fixture("SHA256SUMS.txt.sig"), &why));
        QCOMPARE(why, QStringLiteral("signature-mismatch"));
        QByteArray sig = fixture("SHA256SUMS.txt.sig");
        sig[10] = char(sig[10] ^ 0x40);
        QVERIFY(!verifyUpdateSignature(fixture("test-only-public.pem"), fixture("SHA256SUMS.txt"), sig, &why));
        QVERIFY(!verifyUpdateSignature(QByteArray(), fixture("SHA256SUMS.txt"), fixture("SHA256SUMS.txt.sig"), &why));
        QCOMPARE(why, QStringLiteral("no-public-key"));
        QVERIFY(!verifyUpdateSignature("-----BEGIN PUBLIC KEY-----\nAAAA\n-----END PUBLIC KEY-----\n", fixture("SHA256SUMS.txt"), fixture("SHA256SUMS.txt.sig"), &why));
        QVERIFY(!verifyUpdateSignature(fixture("test-only-public.pem"), fixture("SHA256SUMS.txt"), QByteArray(5000, 'x'), &why));
    }
    void checksumList() {
        const QByteArray sums = fixture("SHA256SUMS.txt");
        const auto pkg = updatePackageIn(sums);
        QVERIFY(pkg.has_value());
        QCOMPARE(pkg->fileName, QStringLiteral("PushFPS-9.9.9-win64.zip"));
        QCOMPARE(pkg->version, QVersionNumber(9, 9, 9));
        QCOMPARE(pkg->sha256, QByteArray("9cf0393b38dfb998d3fdf131983522a2cb938ada3c6ab2b33eff807182e6805c"));
        QCOMPARE(updateSumFor(sums, QStringLiteral("PushFPS-9.9.9-OBS-Plugin.zip")).value(), QByteArray(64, '0'));
        QVERIFY(!updateSumFor(sums, QStringLiteral("other.zip")).has_value());
        QVERIFY(!updatePackageIn(sums + sums).has_value());   // two packages: refused
        QVERIFY(!updatePackageIn("deadbeef  PushFPS-1.0.0-win64.zip\n").has_value());
        QVERIFY(!updatePackageIn(QByteArray(64, 'a') + "  PushFPS-1.0.0-beta-win64.zip\n").has_value());
        QVERIFY(updatePackageIn(QByteArray(64, 'A') + " *PushFPS-1.2.3-win64.zip\r\n").has_value());
    }
    void installRecord() {
        auto rec = parseInstalledVersionFile("PushFPS 3.2.0\r\nRuntime: Veyra-2.1.6-NVIDIA-win64-portable.zip from https://x\r\nRuntime SHA-256: ab\r\n");
        QVERIFY(rec.has_value());
        QCOMPARE(rec->gpu, QStringLiteral("NVIDIA"));
        QVERIFY(rec->extras);
        rec = parseInstalledVersionFile("PushFPS 3.2.0\nRuntime: Veyra-2.1.6-AMD-win64-portable.zip from x\nNot installed: runtime\\amd-nr (...)\n");
        QCOMPARE(rec->gpu, QStringLiteral("AMD"));
        QVERIFY(!rec->extras);
        // The neural runtime's own line is not an "extras left out" line: -ExperimentalExtras Yes stays Yes.
        rec = parseInstalledVersionFile("PushFPS 3.2.2\r\nRuntime: Veyra-2.1.6-NVIDIA-win64-portable.zip from x\r\n"
                                        "Neural runtime: not installed (runtime\\experimental\\nr-original, NVIDIA's neural-rendering runtime, experimental; optional)\r\n");
        QVERIFY(rec.has_value());
        QVERIFY(rec->extras);
        rec = parseInstalledVersionFile("PushFPS 3.2.2\nRuntime: Veyra-2.1.6-NVIDIA-win64-portable.zip from x\nNeural runtime: kept (runtime\\experimental\\nr-original, installed earlier by your choice)\n");
        QVERIFY(rec->extras);
        QVERIFY(!parseInstalledVersionFile("hello").has_value());
        QCOMPARE(parseInstalledVersionFile("PushFPS 3.1.4\nRuntime: Veyra-2.1.9-NVIDIA-win64-portable.zip from x\n")->version, QVersionNumber(3, 1, 4));
    }
    // 3.1.3 and 3.1.4 report "2.1.9" (Veyra's version) as their own: the installer's record decides, so an
    // installed release is not offered again.
    void installedRecordStopsTheUpdateLoop() {
        Server server;
        server.files["/latest"] = QJsonDocument(QJsonObject{
            {"tag_name", "v3.1.4"}, {"draft", false}, {"prerelease", false},
            {"html_url", "https://github.com/DosukaSOL/PushFPS/releases/tag/v3.1.4"}, {"assets", QJsonArray{}}}).toJson();
        QTemporaryDir install;
        QFile rec(install.filePath("PushFPS-VERSION.txt"));
        QVERIFY(rec.open(QIODevice::WriteOnly));
        rec.write("PushFPS 3.1.4\r\nRuntime: Veyra-2.1.9-NVIDIA-win64-portable.zip from x\r\n");
        rec.close();
        UpdateChecker checker(QStringLiteral("2.1.9"), nullptr, QUrl(QString::fromLatin1(server.base() + "/latest")));
        checker.setAllowHttpForTests(true);
        checker.setInstallDirForTests(install.path());
        QSignalSpy checked(&checker, &UpdateChecker::completed);
        checker.check(true);
        QVERIFY(checked.wait(5000));
        QVERIFY2(checked.first().first().toString() != QStringLiteral("available"), qPrintable(checked.first().first().toString()));
    }

    // The whole flow against a local server: signed list, zip, unpack with tar, installer launch.
    void installFlow_data() {
        QTest::addColumn<bool>("tamperZip");
        QTest::addColumn<bool>("tamperSig");
        QTest::addColumn<QString>("result");
        QTest::newRow("verified") << false << false << QStringLiteral("started");
        QTest::newRow("zip-changed") << true << false << QStringLiteral("sha256-mismatch");
        QTest::newRow("signature-changed") << false << true << QStringLiteral("signature-mismatch");
    }
    void installFlow() {
        QFETCH(bool, tamperZip); QFETCH(bool, tamperSig); QFETCH(QString, result);
        Server server;
        const QByteArray dl = "/DosukaSOL/PushFPS/releases/download/v9.9.9/";
        auto asset = [&](const char* name) { return QJsonObject{{"name", name}, {"browser_download_url", QString::fromLatin1(server.base() + dl + name)}}; };
        server.files["/latest"] = QJsonDocument(QJsonObject{
            {"tag_name", "v9.9.9"}, {"draft", false}, {"prerelease", false},
            {"html_url", "https://github.com/DosukaSOL/PushFPS/releases/tag/v9.9.9"},
            {"assets", QJsonArray{asset("SHA256SUMS.txt"), asset("SHA256SUMS.txt.sig"), asset("PushFPS-9.9.9-win64.zip"),
                                  QJsonObject{{"name", "PushFPS-9.9.9-win64.zip"}, {"browser_download_url", "https://evil.example/PushFPS-9.9.9-win64.zip"}}}}}).toJson();
        server.files[dl + "SHA256SUMS.txt"] = fixture("SHA256SUMS.txt");
        QByteArray sig = fixture("SHA256SUMS.txt.sig");
        if (tamperSig) sig[3] = char(sig[3] ^ 1);
        server.files[dl + "SHA256SUMS.txt.sig"] = sig;
        QByteArray zip = fixture("PushFPS-9.9.9-win64.zip");
        if (tamperZip) zip.append('x');
        server.files[dl + "PushFPS-9.9.9-win64.zip"] = zip;

        QTemporaryDir install;
        QFile rec(install.filePath("PushFPS-VERSION.txt"));
        QVERIFY(rec.open(QIODevice::WriteOnly));
        rec.write("PushFPS 3.2.0\r\nRuntime: Veyra-2.1.6-NVIDIA-win64-portable.zip from x\r\nNot installed: x\r\n");
        rec.close();

        UpdateChecker checker(QStringLiteral("3.2.0"), nullptr, QUrl(QString::fromLatin1(server.base() + "/latest")));
        checker.setAllowHttpForTests(true);
        checker.setUpdateKeyForTests(fixture("test-only-public.pem"));
        checker.setInstallDirForTests(install.path());
        QString program; QStringList args;
        checker.setLaunchInstallerForTests([&](const QString& p, const QStringList& a) { program = p; args = a; return true; });
        QSignalSpy checked(&checker, &UpdateChecker::completed), finished(&checker, &UpdateChecker::installFinished), quit(&checker, &UpdateChecker::quitForUpdate);
        checker.check(true);
        QVERIFY(checked.wait(5000));
        QCOMPARE(checked.first().first().toString(), QStringLiteral("available"));
        QVERIFY2(checker.canInstall(), "signed assets and an install record");
        checker.installUpdate();
        QVERIFY(finished.wait(20000));
        QCOMPARE(finished.first().first().toString(), result);
        if (result == QStringLiteral("started")) {
            QCOMPARE(quit.count(), 1);
            QCOMPARE(program, QStringLiteral("powershell.exe"));
            QVERIFY(args.contains(QStringLiteral("-WaitForPid")));
            QCOMPARE(args.at(args.indexOf(QStringLiteral("-Gpu")) + 1), QStringLiteral("NVIDIA"));
            QCOMPARE(args.at(args.indexOf(QStringLiteral("-ExperimentalExtras")) + 1), QStringLiteral("No"));
            QCOMPARE(QDir::fromNativeSeparators(args.at(args.indexOf(QStringLiteral("-InstallDir")) + 1)), install.path());
            QVERIFY(QFile::exists(QDir::fromNativeSeparators(args.at(args.indexOf(QStringLiteral("-File")) + 1))));
        } else {
            QCOMPARE(quit.count(), 0);
            QVERIFY(program.isEmpty());
            QCOMPARE(checker.installState(), QStringLiteral("failed"));
        }
    }
    void unsignedReleaseOnlyOpensThePage() {
        UpdateChecker checker(QStringLiteral("3.2.0"));
        checker.setUpdateKeyForTests(QByteArray());
        QVERIFY(!checker.canInstall());
    }
};

QTEST_MAIN(UpdateVerifyTests)
#include "UpdateVerifyTests.moc"
