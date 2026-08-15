#pragma once



#include <QSystemTrayIcon>
#include <QMenu>
#include "skipper.h"
#include "qcurl.h"
#include "float_button.h"

class SettingDialog;
class Updater;

class App : public QObject
{
    Q_OBJECT

public:
    explicit App(QObject* parent = nullptr);

    ~App() override;

private slots:
    void onFunction1();

    void onFunction2();

    void onFunction3();

    void onFunction4();

private:
    void createActions();

    void createTrayIcon();

    void scheduleAutoUpdateCheck();

public:
    static Skipper* skipper;
    static App* instance() { return _instance; }
    void setFloatButtonEnabled(bool enabled);
    void setAutoUpdateEnabled(bool enabled);
private:

    QSystemTrayIcon* trayIcon = nullptr;
    QMenu* trayIconMenu{};
    static App* _instance;

    QAction* function1Action{};
    QAction* function2Action{};
    QAction* function3Action{};
    QAction* function4Action{};
    QAction* quitAction{};
    SettingDialog* settingDialog = nullptr;
    FloatButton* floatButton = nullptr;
    Updater* updater = nullptr;

    ConfigAwareQEasy* qeasy = nullptr;
    bool manualUpdateCheck = false;
};

