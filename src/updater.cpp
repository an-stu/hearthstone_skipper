#include "updater.h"

#include "logger.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>
#include <QVersionNumber>

#ifndef APP_VERSION
#define APP_VERSION "v0.0.0"
#endif

namespace {

constexpr auto kGitHubReleasesUrl = "https://api.github.com/repos/an-stu/hearthstone_skipper/releases?per_page=1";
constexpr const char *kArchiveSuffix = "-macos-arm64.zip";

QVersionNumber versionFromTag(QString tag) {
    if (tag.startsWith('v') || tag.startsWith('V')) {
        tag.remove(0, 1);
    }
    return QVersionNumber::fromString(tag);
}

QString normalizedVersionString(const QString &tag) {
    if (tag.startsWith('v') || tag.startsWith('V')) {
        return tag.mid(1);
    }
    return tag;
}

} // namespace

Updater::Updater(QObject *parent) : QObject(parent), _network(new QNetworkAccessManager(this)), _logger(spdlog::get("skipper")) {
}

void Updater::checkForUpdates() {
    if (_busy) {
        SPDLOG_LOGGER_WARN(_logger, "updater_check skipped: already busy");
        emit checkFinished(false, {}, tr("正在检查更新，请稍候"));
        return;
    }
    _busy = true;
    const QString releasesUrl =
        qEnvironmentVariable("SKIPPER_UPDATE_API_URL", QLatin1String(kGitHubReleasesUrl));
    SPDLOG_LOGGER_INFO(_logger, "updater_check_begin url={}", releasesUrl.toStdString());

    QNetworkRequest request{QUrl(releasesUrl)};
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("User-Agent", QByteArray("skipper-updater/") + APP_VERSION);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply *reply = _network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] { handleReleaseResponse(reply); });
}

void Updater::handleReleaseResponse(QNetworkReply *reply) {
    reply->deleteLater();
    const QByteArray body = reply->readAll();
    if (reply->error() != QNetworkReply::NoError) {
        SPDLOG_LOGGER_ERROR(_logger, "updater_check_failed error={} code={}", reply->errorString().toStdString(),
                            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt());
        _busy = false;
        emit checkFinished(false, {}, reply->errorString());
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray() || document.array().isEmpty()) {
        SPDLOG_LOGGER_ERROR(_logger, "updater_check_invalid_response error={} body={}", parseError.errorString().toStdString(),
                            body.toStdString());
        _busy = false;
        emit checkFinished(false, {}, tr("GitHub 返回了无法解析的数据"));
        return;
    }

    const QJsonObject release = document.array().first().toObject();
    const QString tag = release.value("tag_name").toString();
    const QVersionNumber latestVersion = versionFromTag(tag);
    const QVersionNumber currentVersion = versionFromTag(QString::fromLatin1(APP_VERSION));
    if (latestVersion.isNull() || currentVersion.isNull() || latestVersion <= currentVersion) {
        SPDLOG_LOGGER_INFO(_logger, "updater_check_none current={} latest={}", APP_VERSION, tag.toStdString());
        _busy = false;
        emit checkFinished(false, tag, {});
        return;
    }

    QString archiveUrl;
    QString checksumUrl;
    const QJsonArray assets = release.value("assets").toArray();
    for (const QJsonValue &assetValue : assets) {
        const QJsonObject asset = assetValue.toObject();
        const QString name = asset.value("name").toString();
        const QString downloadUrl = asset.value("browser_download_url").toString();
        if (name == QStringLiteral("SHA256SUMS.txt")) {
            checksumUrl = downloadUrl;
        } else if (name.endsWith(QLatin1String(kArchiveSuffix))) {
            archiveUrl = downloadUrl;
        }
    }

    if (archiveUrl.isEmpty()) {
        SPDLOG_LOGGER_ERROR(_logger, "updater_check_missing_archive tag={}", tag.toStdString());
        _busy = false;
        emit checkFinished(false, tag, tr("Release 中缺少 macOS 安装包"));
        return;
    }
    if (checksumUrl.isEmpty()) {
        SPDLOG_LOGGER_ERROR(_logger, "updater_check_missing_checksum tag={}", tag.toStdString());
        _busy = false;
        emit checkFinished(false, tag, tr("Release 中缺少 SHA256 校验文件"));
        return;
    }

    _latestVersion = tag;
    _archiveUrl = QUrl(archiveUrl);
    _checksumUrl = QUrl(checksumUrl);
    SPDLOG_LOGGER_INFO(_logger, "updater_check_available latest={} archive={} checksum={}", tag.toStdString(),
                       _archiveUrl.toString().toStdString(), _checksumUrl.toString().toStdString());
    _busy = false;
    emit checkFinished(true, tag, {});
}

void Updater::installUpdate() {
    if (_latestVersion.isEmpty() || _archiveUrl.isEmpty() || _checksumUrl.isEmpty()) {
        SPDLOG_LOGGER_WARN(_logger, "updater_install_rejected: no update info");
        emit installFinished(false, tr("没有可用的更新信息，请先检查更新"));
        return;
    }
    if (_busy) {
        SPDLOG_LOGGER_WARN(_logger, "updater_install_rejected: already busy");
        emit installFinished(false, tr("更新正在进行中"));
        return;
    }
    _busy = true;
    _workDirectory = updateWorkDirectory();
    if (!QDir().mkpath(_workDirectory)) {
        finishInstall(false, tr("无法创建更新缓存目录"));
        return;
    }
    SPDLOG_LOGGER_INFO(_logger, "updater_install_begin version={} work={}", _latestVersion.toStdString(),
                       _workDirectory.toStdString());
    downloadChecksum();
}

void Updater::downloadChecksum() {
    QNetworkRequest request{_checksumUrl};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setRawHeader("User-Agent", QByteArray("skipper-updater/") + APP_VERSION);
    QNetworkReply *reply = _network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] { handleChecksumResponse(reply); });
}

void Updater::handleChecksumResponse(QNetworkReply *reply) {
    reply->deleteLater();
    const QByteArray body = reply->readAll();
    if (reply->error() != QNetworkReply::NoError) {
        SPDLOG_LOGGER_ERROR(_logger, "updater_checksum_failed error={}", reply->errorString().toStdString());
        finishInstall(false, tr("下载 SHA256 校验文件失败：%1").arg(reply->errorString()));
        return;
    }

    const QString expectedFile = archiveFileName();
    for (const QByteArray &line : body.split('\n')) {
        const QList<QByteArray> parts = line.trimmed().split(' ');
        if (parts.size() >= 2 && QFileInfo(QString::fromLatin1(parts.last())).fileName() == expectedFile) {
            _expectedSha256 = QString::fromLatin1(parts.first()).toLower();
            break;
        }
    }
    if (_expectedSha256.size() != 64) {
        SPDLOG_LOGGER_ERROR(_logger, "updater_checksum_missing_entry file={} body={}", expectedFile.toStdString(),
                            body.toStdString());
        finishInstall(false, tr("校验文件中找不到 %1 的 SHA-256 值").arg(expectedFile));
        return;
    }

    SPDLOG_LOGGER_INFO(_logger, "updater_checksum_ok expected={}", _expectedSha256.toStdString());
    downloadArchive();
}

void Updater::downloadArchive() {
    QNetworkRequest request{_archiveUrl};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setRawHeader("User-Agent", QByteArray("skipper-updater/") + APP_VERSION);

    _archivePath = QDir(_workDirectory).filePath(archiveFileName());
    auto *archiveFile = new QFile(_archivePath, this);
    if (!archiveFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        SPDLOG_LOGGER_ERROR(_logger, "updater_archive_open_failed path={} error={}", _archivePath.toStdString(),
                            archiveFile->errorString().toStdString());
        archiveFile->deleteLater();
        finishInstall(false, tr("无法写入更新文件"));
        return;
    }

    QNetworkReply *reply = _network->get(request);
    connect(reply, &QNetworkReply::readyRead, this, [archiveFile, reply] {
        if (archiveFile->isOpen()) {
            archiveFile->write(reply->readAll());
        }
    });
    connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
        static qint64 lastLogged = 0;
        if (received - lastLogged > 5 * 1024 * 1024 || (total > 0 && received >= total)) {
            lastLogged = received;
            SPDLOG_LOGGER_INFO(_logger, "updater_download_progress received={} total={}", received, total);
        }
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, archiveFile, reply] { handleArchiveResponse(reply, archiveFile); });
}

void Updater::handleArchiveResponse(QNetworkReply *reply, QFile *archiveFile) {
    if (archiveFile->isOpen()) {
        archiveFile->write(reply->readAll());
        archiveFile->close();
    }
    archiveFile->deleteLater();
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        SPDLOG_LOGGER_ERROR(_logger, "updater_archive_failed error={}", reply->errorString().toStdString());
        finishInstall(false, tr("下载更新包失败：%1").arg(reply->errorString()));
        return;
    }

    const QString actualSha256 = sha256OfFile(_archivePath);
    if (actualSha256.compare(_expectedSha256, Qt::CaseInsensitive) != 0) {
        SPDLOG_LOGGER_ERROR(_logger, "updater_sha256_mismatch expected={} actual={}", _expectedSha256.toStdString(),
                            actualSha256.toStdString());
        finishInstall(false, tr("更新包 SHA-256 校验失败，已取消安装"));
        return;
    }

    SPDLOG_LOGGER_INFO(_logger, "updater_sha256_ok path={}", _archivePath.toStdString());
    extractAndInstall();
}

void Updater::extractAndInstall() {
    const QString extractedDirectory = QDir(_workDirectory).filePath("extracted");
    QDir(extractedDirectory).removeRecursively();
    if (!QDir().mkpath(extractedDirectory)) {
        finishInstall(false, tr("无法创建解压目录"));
        return;
    }

    QString output;
    if (!runCommand("/usr/bin/ditto", {"-x", "-k", _archivePath, extractedDirectory}, &output)) {
        SPDLOG_LOGGER_ERROR(_logger, "updater_extract_failed output={}", output.toStdString());
        finishInstall(false, tr("解压更新包失败"));
        return;
    }

    const QString extractedBundle = QDir(extractedDirectory).filePath("skipper.app");
    if (!QFileInfo::exists(extractedBundle)) {
        finishInstall(false, tr("更新包中缺少 skipper.app"));
        return;
    }

    if (!runCommand("/usr/bin/codesign", {"--verify", "--deep", "--strict", extractedBundle}, &output)) {
        SPDLOG_LOGGER_ERROR(_logger, "updater_codesign_failed output={}", output.toStdString());
        finishInstall(false, tr("更新包签名校验失败"));
        return;
    }

    const QString installedBundle = currentBundlePath();
    if (QFileInfo(installedBundle).absolutePath() != QStringLiteral("/Applications")) {
        SPDLOG_LOGGER_WARN(_logger, "updater_refuse_non_applications bundle={}", installedBundle.toStdString());
        finishInstall(false, tr("当前应用未安装在 /Applications，无法自动更新"));
        return;
    }

    const QString backupBundle = QDir(_workDirectory).filePath("skipper.app.old");
    QFileInfo(backupBundle).dir().remove(backupBundle);
    if (!runCommand("/bin/mv", {installedBundle, backupBundle}, &output)) {
        SPDLOG_LOGGER_ERROR(_logger, "updater_move_old_failed output={}", output.toStdString());
        finishInstall(false, tr("无法移除旧版本应用"));
        return;
    }

    if (!runCommand("/usr/bin/ditto", {extractedBundle, installedBundle}, &output)) {
        SPDLOG_LOGGER_ERROR(_logger, "updater_install_failed output={}", output.toStdString());
        runCommand("/bin/mv", {backupBundle, installedBundle}, &output);
        finishInstall(false, tr("安装新版本失败，已回滚"));
        return;
    }

    if (!runCommand("/usr/bin/open", {"-n", installedBundle}, &output)) {
        SPDLOG_LOGGER_ERROR(_logger, "updater_launch_failed output={}", output.toStdString());
        runCommand("/bin/mv", {backupBundle, installedBundle}, &output);
        finishInstall(false, tr("启动新版本失败，已回滚"));
        return;
    }

    SPDLOG_LOGGER_INFO(_logger, "updater_install_success version={} installed={}", _latestVersion.toStdString(),
                       installedBundle.toStdString());
    _logger->flush();
    emit installFinished(true, tr("已更新到 %1，正在重启").arg(_latestVersion));
    QTimer::singleShot(800, qApp, &QCoreApplication::quit);
}

void Updater::finishInstall(bool success, const QString &message) {
    _busy = false;
    SPDLOG_LOGGER_INFO(_logger, "updater_install_finish success={} message={}", success, message.toStdString());
    emit installFinished(success, message);
}

QString Updater::currentBundlePath() const {
    QDir directory{QCoreApplication::applicationDirPath()};
    directory.cdUp(); // Contents/MacOS -> Contents
    directory.cdUp(); // Contents -> skipper.app
    return directory.absolutePath();
}

QString Updater::updateWorkDirectory() const {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    return QDir(base).filePath(QStringLiteral("updates/%1").arg(normalizedVersionString(_latestVersion)));
}

QString Updater::archiveFileName() const {
    return _archiveUrl.fileName();
}

bool Updater::runCommand(const QString &program, const QStringList &arguments, QString *output, int timeoutMs) {
    QProcess process;
    process.start(program, arguments);
    if (!process.waitForStarted(5000)) {
        if (output) {
            *output = QStringLiteral("%1 无法启动").arg(program);
        }
        return false;
    }
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(3000);
        if (output) {
            *output = QStringLiteral("%1 执行超时").arg(program);
        }
        return false;
    }
    if (output) {
        *output = QString::fromLocal8Bit(process.readAllStandardOutput()) + QString::fromLocal8Bit(process.readAllStandardError());
    }
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

QString Updater::sha256OfFile(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    constexpr qint64 kChunkSize = 1024 * 1024;
    while (!file.atEnd()) {
        hash.addData(file.read(kChunkSize));
    }
    return QString::fromLatin1(hash.result().toHex());
}

QString Updater::latestVersion() const {
    return _latestVersion;
}

bool Updater::canInstallInPlace() const {
    return QFileInfo(currentBundlePath()).absolutePath() == QStringLiteral("/Applications");
}
