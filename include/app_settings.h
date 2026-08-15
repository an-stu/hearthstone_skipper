#pragma once

#include "clash_config.h"

#include <QDateTime>
#include <QSettings>
#include <QString>
#include <optional>

class AppSettings {
public:
    AppSettings();
    [[nodiscard]] std::optional<ClashConfig> clash_config() const;
    void clash_config_set(const ClashConfig &value);
    [[nodiscard]] bool float_button_enabled() const;
    void float_button_enabled_set(bool enabled);
    [[nodiscard]] bool auto_update_enabled() const;
    void auto_update_enabled_set(bool enabled);
    [[nodiscard]] QDateTime last_auto_update_check() const;
    void last_auto_update_check_set(const QDateTime &value);

    static AppSettings &instance();

private:
    QSettings _settings;
};