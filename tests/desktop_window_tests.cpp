#include "../src/desktop_window_state.hpp"
#include <QGuiApplication>
#include <QProcess>
#include <QResizeEvent>
#include <QScreen>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QWindow>

class ProbeWindow final : public QWindow {
public:
    QSize deliveredSize;
protected:
    void resizeEvent(QResizeEvent* event) override { deliveredSize = event->size(); }
};

class DesktopWindowTests : public QObject {
    Q_OBJECT
private slots:
    void remembersNormalSizeAcrossRestart() {
        QTemporaryDir directory;
        const auto path = directory.filePath("audio.ini");
        const auto run = [&](const QString& size) {
            QProcess process;
            process.start(QCoreApplication::applicationFilePath(), {"--window-probe", path, size});
            if (!process.waitForFinished(10000) || process.exitCode() != 0) return QByteArray{};
            return process.readAllStandardOutput().trimmed();
        };
        QCOMPARE(run("730x510"), QByteArray("730x510"));
        QCOMPARE(run("restore"), QByteArray("730x510"));
        for (const auto& action : {"maximized", "fullscreen", "minimized"}) {
            QCOMPARE(run(action), QByteArray("730x510"));
            QCOMPARE(run("restore"), QByteArray("730x510"));
        }
    }
    void rejectsInvalidSizeAndPreservesOtherSettings() {
        QTemporaryDir directory;
        const auto path = directory.filePath("audio.ini");
        for (const auto& value : {QVariant(), QVariant("garbage"), QVariant(QSize(-1, 600)), QVariant(QSize(600, 0))}) {
            {
                QSettings settings(path, QSettings::IniFormat);
                if (value.isValid()) settings.setValue("desktop/normalSize", value);
                else settings.remove("desktop/normalSize");
                settings.setValue("input/device", "unchanged");
            }
            QWindow window;
            window.resize(620, 420);
            DesktopWindowState state(window, path);
            QCOMPARE(window.size(), QSize(620, 420));
            QVERIFY(state.save());
            QCOMPARE(QSettings(path, QSettings::IniFormat).value("input/device").toString(), QString("unchanged"));
        }
    }
    void oversizedRestoreFitsRealScreen() {
        QTemporaryDir directory;
        const auto path = directory.filePath("audio.ini");
        {
            QSettings settings(path, QSettings::IniFormat);
            settings.setValue("desktop/normalSize", QSize(30000, 30000));
        }
        QWindow window;
        window.setMinimumSize(QSize(360, 360));
        DesktopWindowState state(window, path);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTRY_VERIFY(window.screen()->availableGeometry().contains(window.frameGeometry()));
        QVERIFY(state.save());
        QCOMPARE(QSettings(path, QSettings::IniFormat).value("desktop/normalSize").toSize(), window.size());
    }
    void fitsFrameInsideSmallWorkArea() {
        const QRect workArea(100, 200, 640, 480);
        const auto geometry = DesktopWindowState::boundedGeometry(QRect(-900, -500, 1400, 1000), workArea, QMargins(8, 30, 8, 8));
        QCOMPARE(geometry.size(), QSize(624, 442));
        QVERIFY(workArea.contains(geometry.marginsAdded(QMargins(8, 30, 8, 8))));
        QCOMPARE(DesktopWindowState::boundedGeometry(QRect(150, 250, 320, 240), workArea, {}), QRect(150, 250, 320, 240));
    }
};

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    if (app.arguments().contains("--window-probe")) {
        ProbeWindow window;
        window.resize(620, 420);
        DesktopWindowState state(window, app.arguments().at(2));
        QObject::connect(&app, &QCoreApplication::aboutToQuit, &state, [&] { state.save(); });
        window.show();
        if (!QTest::qWaitForWindowExposed(&window)) return 1;
        QCoreApplication::processEvents();
        const auto action = app.arguments().at(3);
        if (action == "maximized") window.showMaximized();
        else if (action == "fullscreen") window.showFullScreen();
        else if (action == "minimized") window.showMinimized();
        else if (action != "restore") {
            window.resize(730, 510);
            if (!QTest::qWaitFor([&] { return window.deliveredSize == QSize(730, 510); })) return 1;
        }
        const auto expectedState = action == "maximized" ? Qt::WindowMaximized
            : action == "fullscreen" ? Qt::WindowFullScreen
            : action == "minimized" ? Qt::WindowMinimized : Qt::WindowNoState;
        if (!QTest::qWaitFor([&] { return window.windowState() == expectedState; })) return 1;
        const auto size = QSettings(app.arguments().at(2), QSettings::IniFormat).value("desktop/normalSize").toSize();
        const auto normal = window.windowState() != Qt::WindowNoState ? size : window.size();
        fprintf(stdout, "%dx%d\n", normal.width(), normal.height());
        QTimer::singleShot(0, &app, &QCoreApplication::quit);
        return app.exec();
    }
    DesktopWindowTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "desktop_window_tests.moc"
