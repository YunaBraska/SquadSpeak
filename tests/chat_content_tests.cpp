#include "chat_content.hpp"
#include "voice_session.hpp"
#include <QBuffer>
#include <QImage>
#include <QTemporaryDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTimer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QSignalSpy>
#include <QCryptographicHash>
#include <QTextDocument>
#include <QTextCursor>
#include <QFont>
#include <QTest>
#include <QClipboard>
#include <QMimeData>
#include <QGuiApplication>
#include <QTranslator>
#include <QScopeGuard>
#include <array>
#include <utility>

class ChatContentTests final : public QObject {
    Q_OBJECT
private slots:
    void decoderTimeoutWaitsForProcessExit() {
        if (!qEnvironmentVariableIsSet("SQUAD_TEST_DECODER_TIMEOUT")) {
            QTemporaryDir folder;
            QVERIFY(folder.isValid());
#ifdef Q_OS_WIN
            const QString suffix = ".exe";
#else
            const QString suffix;
#endif
            const auto probe = folder.filePath("image-timeout-test" + suffix);
            QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), probe));
            QVERIFY(QFile::copy(probe, folder.filePath("squad_image_worker" + suffix)));
            QProcess child;
            auto environment = QProcessEnvironment::systemEnvironment();
            environment.insert("SQUAD_TEST_DECODER_TIMEOUT", "1");
            child.setProcessEnvironment(environment);
            child.start(probe, {"decoderTimeoutWaitsForProcessExit"});
            QVERIFY2(child.waitForStarted(), qPrintable(child.errorString()));
            QVERIFY2(child.waitForFinished(30000), qPrintable(child.errorString()));
            const auto output = child.readAllStandardOutput() + child.readAllStandardError();
            QCOMPARE(child.exitStatus(), QProcess::NormalExit);
            QVERIFY2(child.exitCode() == 0, output.constData());
            return;
        }
        QObject context;
        int completed = 0;
        bool completedWhileRunning = false;
        QByteArray result;
        QString failure;
        QVERIFY(ChatContent::prepare("deliberately stalled decoder", &context,
            [&](QByteArray bytes, QString error) {
                ++completed;
                result = std::move(bytes); failure = std::move(error);
                for (auto* process : context.findChildren<QProcess*>())
                    completedWhileRunning |= process->state() != QProcess::NotRunning;
            }));
        QTRY_COMPARE_WITH_TIMEOUT(completed, 1, 25000);
        QVERIFY2(!completedWhileRunning, "Decoder capacity must remain reserved until the process exits");
        QVERIFY(result.isEmpty());
        QCOMPARE(failure, "Image processing took too long.");
        QTest::qWait(100);
        QCOMPARE(completed, 1);
        QTRY_VERIFY(context.findChildren<QProcess*>().isEmpty());
    }
    void imageFailuresUseSelectedLanguage_data() {
        QTest::addColumn<QString>("language");
        for (const auto* language : {"en", "de", "ar"}) QTest::newRow(language) << QString(language);
    }
    void imageFailuresUseSelectedLanguage() {
        VoiceSession session;
        QFETCH(QString, language);
        QTranslator catalog;
        if (language != "en") {
            QVERIFY(catalog.load(":/i18n/squadspeak_" + language + ".qm"));
            QVERIFY(QCoreApplication::installTranslator(&catalog));
        }
        const auto restore = qScopeGuard([&] { QCoreApplication::removeTranslator(&catalog); });
        QImage image(16, 12, QImage::Format_RGBA8888); image.fill(Qt::green);
        QByteArray png; QBuffer output(&png);
        QVERIFY(output.open(QIODevice::WriteOnly)); QVERIFY(image.save(&output, "PNG"));
        QImage wide(8193, 1, QImage::Format_RGBA8888); wide.fill(Qt::green);
        QByteArray oversized; QBuffer oversizedOutput(&oversized);
        QVERIFY(oversizedOutput.open(QIODevice::WriteOnly)); QVERIFY(wide.save(&oversizedOutput, "PNG"));
        const std::array cases{
            std::pair{QByteArray{}, "Image file is empty or larger than 25 MiB."},
            std::pair{QByteArray("<svg/>"), "Supported formats are PNG, JPEG, WebP, GIF, and BMP."},
            std::pair{oversized, "Image is too large: maximum 8192 pixels per side and 24 megapixels."},
            std::pair{png.left(20), "Image could not be decoded safely."},
            std::pair{png.left(png.indexOf("IDAT") + 4), "Image could not be decoded safely."}};
        for (const auto& [source, message] : cases) {
            const auto expected = QCoreApplication::translate("ChatContent", message);
            if (language != "en") QVERIFY(expected != QString::fromUtf8(message));
            try {
                (void)ChatContent::sanitizeImage(source);
                QFAIL("Malformed image was accepted.");
            } catch (const std::runtime_error& error) {
                QCOMPARE(QString::fromUtf8(error.what()), expected);
            }
            bool completed = false;
            QByteArray result;
            QString diagnostic;
            QObject request;
            const bool started = ChatContent::prepare(source, &request, [&](QByteArray bytes, QString error) {
                result = bytes; diagnostic = error; completed = true;
            });
            QCOMPARE(started, !source.isEmpty());
            QTRY_VERIFY_WITH_TIMEOUT(completed, 5000);
            QVERIFY(result.isEmpty());
            QCOMPARE(diagnostic, expected);
        }
        try {
            (void)ChatContent::sanitizedImageSize("invalid");
            QFAIL("Malformed sanitized image was accepted.");
        } catch (const std::invalid_argument& error) {
            QCOMPARE(QString::fromUtf8(error.what()), QCoreApplication::translate("ChatContent", "Invalid sanitized PNG."));
        }
    }
    void markdownLinksFollowTheDisplayColorWithoutEnablingUnsafeLinks() {
        ChatContent content;
        const QColor accent("#b7d58a");
        QTextDocument rendered;
        rendered.setHtml(content.format("[Web](https://example.org) ![Foto](https://example.org/image.png) [Unsicher](file:///secret)", {}, accent));
        for (const auto& label : {"Web", "Foto"}) {
            const auto cursor = rendered.find(label);
            QVERIFY(!cursor.isNull());
            QVERIFY(cursor.charFormat().isAnchor());
            QCOMPARE(cursor.charFormat().foreground().color(), accent);
        }
        QVERIFY(!rendered.find("Unsicher").charFormat().isAnchor());
    }
    void markdownPreservesFormattingWithoutActiveContent() {
        ChatContent content;
        const auto html = content.format("# Titel\n\n**fett** und `code`\n\n| A | B |\n|---|---|\n| 1 | 2 |\n\nhttps://example.org/test\n\n"
            "![remote](https://example.org/track.png) ![local](file:///private/test.png)\n\n"
            "[unsafe](javascript:alert(1))\n\n<script>alert('bad')</script>\n\n<img src=\"https://example.org/hidden.png\">");
        QVERIFY(html.contains("<table")); QVERIFY(html.contains("href=\"https://example.org/test\""));
        QVERIFY(html.contains("href=\"https://example.org/track.png\""));
        QVERIFY(!html.contains("<img")); QVERIFY(!html.contains("<script"));
        QVERIFY(!html.contains("href=\"file:")); QVERIFY(!html.contains("href=\"javascript:"));
        QTextDocument rendered; rendered.setHtml(html);
        QVERIFY(rendered.toPlainText().contains("Titel")); QVERIFY(rendered.toPlainText().contains("<script>"));
        QVERIFY(!ChatContent::allowedLink(QUrl("https://user:password@example.org")));
        QVERIFY(!content.openLink("file:///private/test.png"));
    }
    void sanitizedRasterRetainsAlphaButNoTextMetadata() {
        QImage source(16, 12, QImage::Format_ARGB32);
        source.fill(QColor(10, 90, 130, 100)); source.setPixelColor(0, 0, QColor(255, 0, 0, 0));
        source.setText("Author", "Private name"); source.setText("Comment", "<script>payload</script>");
        QByteArray original; QBuffer buffer(&original); QVERIFY(buffer.open(QIODevice::WriteOnly)); QVERIFY(source.save(&buffer, "PNG"));
        QVERIFY(original.contains("Private name"));
        const auto clean = ChatContent::sanitizeImage(original);
        QVERIFY(!clean.contains("Private name")); QVERIFY(!clean.contains("payload"));
        const auto image = QImage::fromData(clean, "PNG");
        QCOMPARE(image.size(), source.size()); QVERIFY(image.textKeys().isEmpty());
        QCOMPARE(image.pixelColor(0, 0).alpha(), 0); QCOMPARE(image.pixelColor(1, 1).alpha(), 100);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, (void)ChatContent::sanitizeImage("<svg/>"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, (void)ChatContent::sanitizeImage(original.left(20)));
        QImage wide(8193, 1, QImage::Format_RGB32); wide.fill(Qt::white);
        QByteArray oversized; QBuffer output(&oversized); output.open(QIODevice::WriteOnly); QVERIFY(wide.save(&output, "PNG"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, (void)ChatContent::sanitizeImage(oversized));
    }
    void sourceStagingAndHostDecoderSupportFormatsAndOrientation_data() {
        QTest::addColumn<QByteArray>("format");
        QTest::addColumn<bool>("rotate");
        for (const auto& format : {"PNG", "JPEG", "WEBP", "BMP", "GIF"})
            QTest::newRow(format) << QByteArray(format) << false;
        QTest::newRow("JPEG EXIF orientation") << QByteArray("JPEG") << true;
    }
    void sourceStagingAndHostDecoderSupportFormatsAndOrientation() {
        QFETCH(QByteArray, format);
        QFETCH(bool, rotate);
        QTemporaryDir folder;
        QByteArray source;
        QSize expected(7, 11);
        if (format == "GIF") {
            source = QByteArray::fromBase64("R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7");
            expected = QSize(1, 1);
        } else {
            QImage pixels(expected, QImage::Format_ARGB32);
            pixels.fill(QColor(40, 90, 150, 128));
            pixels.setText("Author", "Private author");
            QBuffer buffer(&source); QVERIFY(buffer.open(QIODevice::WriteOnly));
            QVERIFY2(pixels.save(&buffer, format.constData()), format.constData());
        }
        if (rotate) {
            // EXIF orientation 6: apply a 90-degree clockwise rotation.
            const auto exif = QByteArray::fromHex("ffe100224578696600004d4d002a00000008000101120003000000010006000000000000");
            source.insert(2, exif);
            expected.transpose();
        }
        QFile input(folder.filePath("source"));
        QVERIFY(input.open(QIODevice::WriteOnly)); QCOMPARE(input.write(source), source.size()); input.close();
        ChatContent content;
        QVERIFY(content.prepareFile(QUrl::fromLocalFile(input.fileName())));
        QTRY_VERIFY(!content.busy());
        QVERIFY2(content.error().isEmpty(), qPrintable(content.error()));
        QCOMPARE(content.preparedImage(), source);
        QVERIFY(content.hasImage());
        QByteArray prepared; QString error; bool finished = false;
        QVERIFY(ChatContent::prepare(content.preparedImage(), this, [&](QByteArray png, QString failure) {
            prepared = png; error = failure; finished = true;
        }));
        QTRY_VERIFY(finished); QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(prepared.left(8), QByteArray::fromHex("89504e470d0a1a0a"));
        const auto clean = QImage::fromData(prepared, "PNG");
        QCOMPARE(ChatContent::sanitizedImageSize(prepared), expected);
        QCOMPARE(clean.size(), expected);
        QVERIFY(clean.textKeys().isEmpty());
        QVERIFY(!prepared.contains("Private author"));
        QVERIFY(!prepared.contains("Exif"));
        if (format == "PNG" || format == "WEBP") QCOMPARE(clean.pixelColor(0, 0).alpha(), 128);
        content.clearImage(); QVERIFY(content.preparedImage().isEmpty()); QVERIFY(!content.hasImage());
    }
    void encodedClipboardImagesRemainOpaqueUntilSentToTheHost() {
        auto* mime = new QMimeData;
        mime->setData("image/png", "invalid PNG source");
        QGuiApplication::clipboard()->setMimeData(mime);
        ChatContent content;
        QVERIFY(content.pasteImage());
        QVERIFY(!content.busy());
        QVERIFY(content.error().isEmpty()); QCOMPARE(content.preparedImage(), QByteArray("invalid PNG source"));
        QImage source(5, 7, QImage::Format_ARGB32);
        source.fill(QColor(70, 120, 160, 128)); source.setText("Author", "Clipboard identity");
        QByteArray bytes; QBuffer output(&bytes); QVERIFY(output.open(QIODevice::WriteOnly)); QVERIFY(source.save(&output, "PNG"));
        mime = new QMimeData; mime->setData("image/png", bytes); QGuiApplication::clipboard()->setMimeData(mime);
        QVERIFY(content.pasteImage()); QTRY_VERIFY(!content.busy()); QVERIFY(content.error().isEmpty());
        QCOMPARE(content.preparedImage(), bytes);
        QCOMPARE(QImage::fromData(content.preparedImage()).pixelColor(0, 0).alpha(), 128);
        QGuiApplication::clipboard()->clear();
    }

    void senderDoesNotDecodeASelectedFile() {
        QTemporaryDir folder;
        QFile input(folder.filePath("corrupt.png"));
        QVERIFY(input.open(QIODevice::WriteOnly)); input.write(QByteArray::fromHex("89504e470d0a1a0a")); input.close();
        ChatContent content;
        QVERIFY(content.prepareFile(QUrl::fromLocalFile(input.fileName())));
        QTRY_VERIFY(!content.busy());
        QVERIFY(content.error().isEmpty()); QCOMPARE(content.preparedImage(), QByteArray::fromHex("89504e470d0a1a0a"));
        content.clearImage();
        QVERIFY(!content.prepareFile(QUrl("https://example.org/image.png")));
        QVERIFY(!content.busy()); QVERIFY(content.preparedImage().isEmpty());
    }
    void sendingImportsWebImagesAndReplacesTheirAddress() {
        QImage pixels(8, 12, QImage::Format_ARGB32); pixels.fill(QColor(10, 80, 140, 128));
        pixels.setText("Author", "Private source author");
        QByteArray source; QBuffer buffer(&source); buffer.open(QIODevice::WriteOnly); QVERIFY(pixels.save(&buffer, "PNG"));
        QTcpServer server;
        int requests = 0;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            auto* socket = server.nextPendingConnection();
            const auto input = std::make_shared<QByteArray>();
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket, input] {
                input->append(socket->readAll());
                if (!input->contains("\r\n\r\n")) return;
                ++requests;
                const auto body = input->contains("/broken") ? QByteArray("<svg/>") : source;
                const auto size = input->contains("/large") ? ChatContent::maximumSourceBytes + 1 : body.size();
                socket->write("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: image/png\r\nContent-Length: "
                    + QByteArray::number(size) + "\r\n\r\n" + body);
                socket->disconnectFromHost();
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const auto base = "http://127.0.0.1:" + QString::number(server.serverPort());
        const auto markdown = "Vorher **fett**\n\n![Foto](" + base + "/image.png)\n\nNachher";
        ChatContent content;
        QVERIFY(!content.format(markdown).contains("<img"));
        QTest::qWait(50); QCOMPARE(requests, 0);
        QSignalSpy prepared(&content, &ChatContent::messagePrepared);
        QVERIFY(content.prepareMessage(markdown));
        QTRY_COMPARE(prepared.size(), 1);
        QVERIFY2(content.error().isEmpty(), qPrintable(content.error()));
        const auto message = prepared.first().first().toString();
        const auto png = prepared.first().at(1).toByteArray();
        const auto hash = QString::fromLatin1(QCryptographicHash::hash(png, QCryptographicHash::Sha256).toHex());
        QVERIFY(message.contains("attachment:" + hash)); QVERIFY(!message.contains(base));
        QTextDocument formatted;
        formatted.setMarkdown(message, QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub));
        QVERIFY(formatted.toPlainText().contains("Vorher fett"));
        QVERIFY(formatted.toPlainText().contains("Nachher"));
        const auto boldCursor = formatted.find("fett");
        QVERIFY(!boldCursor.isNull());
        QVERIFY(boldCursor.charFormat().fontWeight() >= QFont::Bold);
        const auto normalCursor = formatted.find("Vorher");
        QVERIFY(!normalCursor.isNull());
        QVERIFY(normalCursor.charFormat().fontWeight() < QFont::Bold);
        QCOMPARE(png, source);
        const auto display = content.format(message, {{"hash", hash}, {"source", "data:image/png;base64," + QString::fromLatin1(ChatContent::sanitizeImage(png).toBase64())}, {"width", 8}, {"height", 12}});
        QVERIFY(display.contains("<img")); QVERIFY(display.contains("src=\"data:image/png;base64,"));
        QCOMPARE(ChatContent::embedImage(message, png), message);
        QCOMPARE(requests, 1);
        QVERIFY(content.prepareMessage(markdown)); QCOMPARE(prepared.size(), 2); QCOMPARE(requests, 1);
        content.clearImage();
        QVERIFY(!content.prepareMessage("![a](" + base + "/one) ![b](" + base + "/two)"));
        QCOMPARE(requests, 1); QCOMPARE(prepared.size(), 2);
        QVERIFY(content.prepareMessage("![bad](" + base + "/broken)"));
        QTRY_VERIFY(!content.busy()); QVERIFY(content.error().isEmpty()); QCOMPARE(prepared.size(), 3);
        QCOMPARE(content.preparedImage(), QByteArray("<svg/>"));
        content.clearImage();
        QVERIFY(content.prepareMessage("![large](" + base + "/large)"));
        QTRY_VERIFY(!content.busy()); QVERIFY(!content.error().isEmpty()); QCOMPARE(prepared.size(), 3);
        const auto count = requests;
        const auto code = "`![example](" + base + "/image.png)`";
        QVERIFY(content.prepareMessage(code)); QCOMPARE(prepared.size(), 4);
        QCOMPARE(prepared.last().first().toString(), code); QCOMPARE(requests, count);
    }
};
int main(int argc, char** argv) {
    if (QFileInfo(QString::fromLocal8Bit(argv[0])).completeBaseName() == "squad_image_worker") {
        QCoreApplication app(argc, argv);
        QTimer::singleShot(30000, &app, &QCoreApplication::quit);
        return app.exec();
    }
    QGuiApplication app(argc, argv);
    ChatContentTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "chat_content_tests.moc"
