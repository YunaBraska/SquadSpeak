#pragma once

#include <QObject>

// The native updater owns download verification, replacement and relaunch.
class AppUpdates final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
public:
    explicit AppUpdates(bool enabled, QObject* parent = nullptr);
    ~AppUpdates() override;
    [[nodiscard]] bool available() const { return controller_ != nullptr; }
    Q_INVOKABLE bool check();
private:
    void* controller_ = nullptr;
};
