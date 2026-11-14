#pragma once

#include <QEvent>
#include <QScreen>
#include <QSettings>
#include <QTimer>
#include <QWindow>
#include <algorithm>

// The profile lock in main owns serialization. Resize events only update memory.
class DesktopWindowState final : public QObject {
public:
    DesktopWindowState(QWindow& window, const QString& settingsFile)
        : window_(window), settingsFile_(settingsFile), minimum_(window.minimumSize()) {
        QSettings settings(settingsFile_, QSettings::IniFormat);
        settings.setFallbacksEnabled(false);
        const auto stored = settings.value("desktop/normalSize").toSize();
        auto initial = window_.geometry();
        if (stored.isValid() && !stored.isEmpty()) initial.setSize(stored);
        window_.create();
        if (const auto* screen = window_.screen()) initial = boundedGeometry(initial,
            screen->availableGeometry(), window_.frameMargins());
        window_.setMinimumSize(minimum_.boundedTo(initial.size()));
        window_.setGeometry(initial);
        settle_.setSingleShot(true);
        connect(&settle_, &QTimer::timeout, this, [this] {
            if (window_.windowState() != Qt::WindowNoState) return;
            if (const auto* screen = window_.screen(); screen && needsClamp_) {
                const auto available = screen->availableGeometry();
                const auto bounded = boundedGeometry(window_.geometry(), available, window_.frameMargins());
                const auto frame = window_.frameMargins();
                const auto clientLimit = (available.size() - QSize(frame.left() + frame.right(),
                    frame.top() + frame.bottom())).expandedTo(QSize(1, 1));
                window_.setMinimumSize(minimum_.boundedTo(clientLimit));
                if (window_.geometry() != bounded) window_.setGeometry(bounded);
                needsClamp_ = false;
            }
            normalSize_ = window_.size();
        });
        const auto observeScreen = [this](QScreen* screen) {
            disconnect(workAreaConnection_);
            if (screen) workAreaConnection_ = connect(screen, &QScreen::availableGeometryChanged,
                this, [this] { needsClamp_ = true; settle_.start(0); });
            needsClamp_ = true;
            settle_.start(0);
        };
        connect(&window_, &QWindow::screenChanged, this, observeScreen);
        observeScreen(window_.screen());
        window_.installEventFilter(this);
        normalSize_ = window_.size();
    }

    // Flush once before shutdown while the profile lock is still held.
    bool save() const {
        const auto size = window_.windowState() == Qt::WindowNoState ? window_.size() : normalSize_;
        if (!size.isValid() || size.isEmpty()) return true;
        QSettings settings(settingsFile_, QSettings::IniFormat);
        settings.setFallbacksEnabled(false);
        if (settings.value("desktop/normalSize").toSize() == size) return true;
        settings.setValue("desktop/normalSize", size);
        settings.sync();
        return settings.status() == QSettings::NoError;
    }

    static QRect boundedGeometry(const QRect& geometry, const QRect& available, const QMargins& frame) {
        if (available.isEmpty()) return geometry;
        const auto width = std::min(geometry.width(), std::max(1, available.width() - frame.left() - frame.right()));
        const auto height = std::min(geometry.height(), std::max(1, available.height() - frame.top() - frame.bottom()));
        const auto x = std::clamp(geometry.x(), available.left() + frame.left(),
            std::max(available.left() + frame.left(), available.right() - frame.right() - width + 1));
        const auto y = std::clamp(geometry.y(), available.top() + frame.top(),
            std::max(available.top() + frame.top(), available.bottom() - frame.bottom() - height + 1));
        return {x, y, width, height};
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == &window_ && event->type() == QEvent::Expose && frame_ != window_.frameMargins()) {
            frame_ = window_.frameMargins();
            needsClamp_ = true;
        }
        if (watched == &window_ && (event->type() == QEvent::Resize
                || event->type() == QEvent::Expose || event->type() == QEvent::WindowStateChange))
            settle_.start(0);
        return QObject::eventFilter(watched, event);
    }

private:
    QWindow& window_;
    const QString settingsFile_;
    const QSize minimum_;
    QSize normalSize_;
    QMargins frame_;
    bool needsClamp_ = true;
    QTimer settle_;
    QMetaObject::Connection workAreaConnection_;
};
