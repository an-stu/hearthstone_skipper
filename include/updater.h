#pragma once

#include "logger.h"

#include <QNetworkAccessManager>
#include <QObject>
#include <QUrl>

#include <memory>

class QFile;
class QNetworkReply;

class Updater final : public QObject {
    Q_OBJECT

  public:
    explicit Updater(QObject *parent = nullptr);

    void checkForUpdates();
    void installUpdate();

    [[nodiscard]] QString latestVersion() const;
    [[nodiscard]] bool canInstallInPlace() const;

  signals:
    void checkFinished(bool updateAvailable, const QString &version, const QString &errorMessage);
    void installFinished(bool success, const QString &message);

  private:
    void handleReleaseResponse(QNetworkReply *reply);
    void downloadChecksum();
    void handleChecksumResponse(QNetworkReply *reply);
    void downloadArchive();
    void handleArchiveResponse(QNetworkReply *reply, QFile *archiveFile);
    void extractAndInstall();
    void finishInstall(bool success, const QString &message);

    [[nodiscard]] QString currentBundlePath() const;
    [[nodiscard]] QString updateWorkDirectory() const;
    [[nodiscard]] QString archiveFileName() const;
    static bool runCommand(const QString &program, const QStringList &arguments, QString *output = nullptr,
                           int timeoutMs = 60000);
    static QString sha256OfFile(const QString &path);

  private:
    QNetworkAccessManager *_network = nullptr;
    QString _latestVersion;
    QUrl _archiveUrl;
    QUrl _checksumUrl;
    QString _expectedSha256;
    QString _workDirectory;
    QString _archivePath;
    bool _busy = false;
    std::shared_ptr<spdlog::logger> _logger;
};
