#include "app.h"

#include "logger.h"
#include "setting_dialog.h"
#include "skipper.h"
#include "updater.h"

#include <QApplication>
#include <QMainWindow>
#include <QDesktopServices>
#include <QUrl>
#include <QCoreApplication>
#include <QSysInfo>
#include <QDate>
#include <QDateTime>
#include <QMessageBox>
#include <QTimer>

Skipper *App::skipper;
App *App::_instance = nullptr;


App::App(QObject *parent) : QObject(parent), trayIcon(nullptr) {
    _instance = this;
    initLogger();
    const auto startupLogger = spdlog::get("skipper");
    SPDLOG_LOGGER_INFO(startupLogger, "application_start version={} qt={} os={} arch={} pid={} log={}", APP_VERSION,
                       QT_VERSION_STR, QSysInfo::prettyProductName().toStdString(),
                       QSysInfo::currentCpuArchitecture().toStdString(), QCoreApplication::applicationPid(),
                       getLogFilePath().toStdString());
    if (std::optional clash_config = AppSettings::instance().clash_config(); !clash_config.has_value()) {
        qeasy = new ConfigAwareQEasy({}, this);
        auto *deducer = new ConfigDeducer(qeasy, this);
        skipper = new Skipper(qeasy, this);
        deducer->tryDeduce();
        connect(deducer, &ConfigDeducer::deduceFinished, this, [this](ConfigAwareQEasy *deduced) {
            if (deduced == nullptr) {
                return;
            }
            if (settingDialog) {
                settingDialog->setting_tab->setHitText("skipper 设置已自动推断");
            }
            AppSettings::instance().clash_config_set(deduced->config());
        });
    } else {
        qeasy = new ConfigAwareQEasy(clash_config.value(), this);
        skipper = new Skipper(qeasy, this);
    }
    settingDialog = new SettingDialog();
    updater = new Updater(this);

    createActions();
    createTrayIcon();
    if (AppSettings::instance().float_button_enabled()) {
        floatButton = new FloatButton();
    }
    assert(trayIcon != nullptr);
    trayIcon->show();
    settingDialog->show();

    connect(updater, &Updater::checkFinished, this, [this](bool updateAvailable, const QString &version, const QString &errorMessage) {
        const bool manual = manualUpdateCheck;
        manualUpdateCheck = false;
        const auto startupLogger = spdlog::get("skipper");
        if (!errorMessage.isEmpty()) {
            SPDLOG_LOGGER_ERROR(startupLogger, "update_check_failed error={}", errorMessage.toStdString());
            if (manual) {
                QMessageBox::warning(settingDialog, tr("检查更新"), tr("检查更新失败：%1").arg(errorMessage));
            }
            return;
        }
        if (updateAvailable) {
            if (manual) {
                const auto answer = QMessageBox::question(
                    settingDialog, tr("发现新版本"),
                    tr("发现新版本 %1，是否立即下载并安装？\n安装完成后 Skipper 会自动重启。").arg(version));
                if (answer == QMessageBox::Yes) {
                    updater->installUpdate();
                }
            } else if (updater->canInstallInPlace()) {
                trayIcon->showMessage(tr("Skipper 自动更新"), tr("发现新版本 %1，正在后台下载并安装").arg(version),
                                      QSystemTrayIcon::Information, 5000);
                updater->installUpdate();
            } else {
                SPDLOG_LOGGER_WARN(spdlog::get("skipper"), "update_skipped_non_applications_build version={}",
                                   version.toStdString());
            }
        } else if (manual) {
            QMessageBox::information(settingDialog, tr("检查更新"), tr("当前已是最新版本（%1）").arg(APP_VERSION));
        }
    });
    connect(updater, &Updater::installFinished, this, [this](bool success, const QString &message) {
        if (success) {
            trayIcon->showMessage(tr("Skipper 更新"), message, QSystemTrayIcon::Information, 5000);
        } else {
            QMessageBox::warning(settingDialog, tr("更新失败"), message);
        }
    });

    scheduleAutoUpdateCheck();
}

App::~App() {
    delete trayIcon;
    delete trayIconMenu;
    delete settingDialog;
}

void App::createActions() {
    function1Action = new QAction(tr("一键拔线"), this);
    connect(function1Action, &QAction::triggered, this, &App::onFunction1);

    function2Action = new QAction(tr("打开日志"), this);
    connect(function2Action, &QAction::triggered, this, &App::onFunction2);

    function3Action = new QAction(tr("打开设置"), this);
    connect(function3Action, &QAction::triggered, this, &App::onFunction3);

    function4Action = new QAction(tr("检查更新"), this);
    connect(function4Action, &QAction::triggered, this, &App::onFunction4);

    quitAction = new QAction(tr("退出"), this);
    connect(quitAction, &QAction::triggered, this, &QApplication::quit);
}

void App::createTrayIcon() {
    trayIconMenu = new QMenu();
    trayIconMenu->addAction(function1Action);
    trayIconMenu->addAction(function2Action);
    trayIconMenu->addAction(function3Action);
    trayIconMenu->addAction(function4Action);
    trayIconMenu->addSeparator();
    trayIconMenu->addAction(quitAction);

    trayIcon = new QSystemTrayIcon(this);

    trayIcon->setIcon(QIcon::fromTheme("dialog-information"));
    trayIcon->setContextMenu(trayIconMenu);
}

void App::onFunction1() {
    if (skipper) {
        skipper->skip();
    }
}

void App::onFunction2() {
    QDesktopServices::openUrl(QUrl::fromLocalFile(getLogFilePath()));
}

void App::onFunction3() {
    settingDialog->show();
}

void App::onFunction4() {
    manualUpdateCheck = true;
    updater->checkForUpdates();
}

void App::setAutoUpdateEnabled(bool enabled) {
    AppSettings::instance().auto_update_enabled_set(enabled);
}

void App::scheduleAutoUpdateCheck() {
    if (!AppSettings::instance().auto_update_enabled()) {
        return;
    }
    const QDateTime now = QDateTime::currentDateTime();
    const QDateTime lastCheck = AppSettings::instance().last_auto_update_check();
    if (lastCheck.isValid() && lastCheck.date() == now.date()) {
        return;
    }
    AppSettings::instance().last_auto_update_check_set(now);
    QTimer::singleShot(3000, this, [this] {
        if (AppSettings::instance().auto_update_enabled()) {
            updater->checkForUpdates();
        }
    });
}

void App::setFloatButtonEnabled(bool enabled) {
    AppSettings::instance().float_button_enabled_set(enabled);
    if (enabled && floatButton == nullptr) {
        floatButton = new FloatButton();
    } else if (!enabled && floatButton != nullptr) {
        delete floatButton;
        floatButton = nullptr;
    }
}
