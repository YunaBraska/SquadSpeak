#include "chat_content.hpp"
#include "voice_session.hpp"
#include <QBuffer>
#include <QImage>
#include <QTemporaryDir>
#include <QFile>
#include <QFileInfo>
#ifndef Q_OS_IOS
#include <QProcess>
#include <QProcessEnvironment>
#endif
#ifdef Q_OS_IOS
#include <QFuture>
#include <QSemaphore>
#include <QThreadPool>
#include <QtConcurrentRun>
#endif
#include <QTimer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QSignalSpy>
#include <QCryptographicHash>
#include <QTextDocument>
#include <QTextCursor>
#include <QTextBlock>
#include <QTextList>
#include <QTextTable>
#include <QFont>
#include <QFontInfo>
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
#ifdef Q_OS_IOS
        QSKIP("The isolated image-worker subprocess is unavailable on iOS.");
#else
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
#endif
    }
#ifdef Q_OS_IOS
    void inProcessDecoderSurvivesOwnerDestructionAndReuse() {
        QImage image(32, 24, QImage::Format_RGBA8888);
        image.fill(QColor(30, 90, 160, 128));
        QByteArray source;
        QBuffer buffer(&source);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(image.save(&buffer, "PNG"));

        QObject owner;
        QByteArray prepared;
        QString error;
        bool completed = false;
        QVERIFY(ChatContent::prepare(source, &owner, [&](QByteArray bytes, QString failure) {
            prepared = std::move(bytes);
            error = std::move(failure);
            completed = true;
        }));
        QTRY_VERIFY_WITH_TIMEOUT(completed, 10000);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(ChatContent::sanitizedImageSize(prepared), image.size());

        auto* shortLived = new QObject;
        bool calledAfterOwnerDestruction = false;
        QVERIFY(ChatContent::prepare(source, shortLived, [&](QByteArray, QString) {
            calledAfterOwnerDestruction = true;
        }));
        delete shortLived;
        QVERIFY(QThreadPool::globalInstance()->waitForDone(10000));
        QCoreApplication::processEvents();
        QVERIFY(!calledAfterOwnerDestruction);

        prepared.clear();
        error.clear();
        completed = false;
        QVERIFY(ChatContent::prepare(source, &owner, [&](QByteArray bytes, QString failure) {
            prepared = std::move(bytes);
            error = std::move(failure);
            completed = true;
        }));
        QTRY_VERIFY_WITH_TIMEOUT(completed, 10000);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(ChatContent::sanitizedImageSize(prepared), image.size());

        auto* pool = QThreadPool::globalInstance();
        const auto previousMax = pool->maxThreadCount();
        pool->setMaxThreadCount(1);
        QSemaphore entered;
        QSemaphore release;
        auto blocker = QtConcurrent::run([&] {
            entered.release();
            release.acquire();
        });
        bool released = false;
        const auto restore = qScopeGuard([&] {
            if (!released)
                release.release();
            blocker.waitForFinished();
            pool->setMaxThreadCount(previousMax);
        });
        QVERIFY(entered.tryAcquire(1, 10000));
        QObject queuedOwner;
        int queuedCompletions = 0;
        QString queuedError;
        QVERIFY(ChatContent::prepare(source, &queuedOwner, [&](QByteArray bytes, QString failure) {
            QVERIFY(bytes.isEmpty());
            queuedError = std::move(failure);
            ++queuedCompletions;
        }));
        bool rejectedCompleted = false;
        QString rejectedError;
        QVERIFY(!ChatContent::prepare(source, &queuedOwner, [&](QByteArray, QString failure) {
            rejectedCompleted = true;
            rejectedError = std::move(failure);
        }));
        QVERIFY(rejectedCompleted);
        QVERIFY(!rejectedError.isEmpty());
        QCOMPARE(queuedCompletions, 0);
        QTRY_COMPARE_WITH_TIMEOUT(queuedCompletions, 1, 25000);
        QCOMPARE(queuedError, ChatContent::tr("Image processing took too long."));
        QVERIFY(!ChatContent::prepare(source, &queuedOwner, [](QByteArray bytes, QString failure) {
            QVERIFY(bytes.isEmpty());
            QVERIFY(!failure.isEmpty());
        }));
        release.release();
        released = true;
        blocker.waitForFinished();
        QVERIFY(QThreadPool::globalInstance()->waitForDone(10000));
        QCoreApplication::processEvents();
        QCOMPARE(queuedCompletions, 1);
        completed = false;
        QVERIFY(ChatContent::prepare(source, &owner, [&](QByteArray bytes, QString failure) {
            prepared = std::move(bytes);
            error = std::move(failure);
            completed = true;
        }));
        QTRY_VERIFY_WITH_TIMEOUT(completed, 10000);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(ChatContent::sanitizedImageSize(prepared), image.size());
    }
#endif
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
    void markdownCodeAndQuotesAreVisuallyDistinctWithoutChangingCopiedText_data() {
        QTest::addColumn<QColor>("accent");
        QTest::addColumn<QColor>("background");
        QTest::newRow("dark") << QColor("#b69ae0") << QColor("#19171f");
        QTest::newRow("light") << QColor("#7958b5") << QColor("#d9d3e0");
    }
    void markdownCodeAndQuotesAreVisuallyDistinctWithoutChangingCopiedText() {
        QFETCH(QColor, accent);
        QFETCH(QColor, background);
        ChatContent content;
        QTextDocument rendered;
        QFont font; font.setPixelSize(14);
        const auto html = content.format("Ordinary text and `inline_code`.\n\n> Quoted words\n\n```\ncode_line\n  indentation\n```\n\n**Bold** and *italic*.", {}, accent, background, font);
        rendered.setHtml(html);
        const auto normal = rendered.find("Ordinary").charFormat();
        const auto code = rendered.find("inline_code").charFormat();
        QVERIFY2(code.background().style() != Qt::NoBrush, qPrintable(html));
        QCOMPARE(code.background().color(), background);
        QVERIFY(QFontInfo(code.font()).fixedPitch());
        QCOMPARE(normal.background().style(), Qt::NoBrush);
        QCOMPARE(normal.font().pixelSize(), 14);
        const auto blockCode = rendered.find("code_line");
        QVERIFY(QFontInfo(blockCode.charFormat().font()).fixedPitch());
        QVERIFY(blockCode.blockFormat().background().style() != Qt::NoBrush);
        QCOMPARE(blockCode.blockFormat().background().color(), background);
        const auto quote = rendered.find("Quoted words");
        QVERIFY(quote.charFormat().fontItalic());
        QVERIFY(quote.blockFormat().leftMargin() > 0);
        QCOMPARE(quote.blockFormat().background().style(), Qt::NoBrush);
        QCOMPARE(quote.charFormat().foreground().color(), accent);
        QVERIFY(rendered.toPlainText().contains("code_line\n  indentation"));
        QVERIFY(rendered.find("Bold").charFormat().fontWeight() >= QFont::Bold);
        QVERIFY(rendered.find("italic").charFormat().fontItalic());
        QVERIFY(!normal.isAnchor() && !code.isAnchor() && !quote.charFormat().isAnchor());
    }
    void markdownStructuresSurviveImageEmbedding_data() {
        QTest::addColumn<bool>("embed");
        QTest::newRow("text") << false;
        QTest::newRow("with attachment") << true;
    }
    void markdownStructuresSurviveImageEmbedding() {
        QFETCH(bool, embed);
        ChatContent content;
        QString markdown = "# Heading\n\n## Subheading\n\n"
            "3. Number three\n4. Number four\n   - Nested bullet\n\n"
            "> Quote\n>\n> > Nested quote with **strong**\n\n"
            "| Name | Count |\n| :--- | ---: |\n| Mira | 12 |\n| Kai | 7 |\n\n"
            "Escaped \\*stars\\* and ``code `tick` ``; ~~removed~~.\n\n"
            "```cpp\n    https://example.org/code\n```\n\n"
            "![portrait](https://example.org/image.png)";
        if (embed) markdown = ChatContent::embedImage(markdown, "image fixture");
        QTextDocument rendered;
        rendered.setHtml(content.format(markdown));
        QCOMPARE(rendered.find("Heading").blockFormat().headingLevel(), 1);
        QCOMPARE(rendered.find("Subheading").blockFormat().headingLevel(), 2);
        const auto number = rendered.find("Number three");
        QVERIFY(number.currentList());
        QCOMPARE(number.currentList()->format().style(), QTextListFormat::ListDecimal);
        QCOMPARE(number.currentList()->format().start(), 3);
        QCOMPARE(number.currentList()->count(), 2);
        const auto bullet = rendered.find("Nested bullet");
        QVERIFY(bullet.currentList());
        QVERIFY2(bullet.currentList()->format().indent() > number.currentList()->format().indent(), qPrintable(markdown));
        QVERIFY(rendered.find("Nested quote").blockFormat().leftMargin() > rendered.find("Quote").blockFormat().leftMargin());
        QVERIFY(rendered.find("strong").charFormat().fontWeight() >= QFont::Bold);
        const auto* table = rendered.find("Mira").currentTable();
        QVERIFY(table);
        QCOMPARE(table->rows(), 3);
        QCOMPARE(table->columns(), 2);
        QVERIFY(table->cellAt(1, 1).firstCursorPosition().blockFormat().alignment().testFlag(Qt::AlignRight));
        QCOMPARE(table->cellAt(0, 0).firstCursorPosition().block().text(), "Name");
        QCOMPARE(table->cellAt(0, 1).firstCursorPosition().block().text(), "Count");
        QVERIFY(rendered.toPlainText().contains("Escaped *stars* and code `tick`"));
        QVERIFY(rendered.find("removed").charFormat().fontStrikeOut());
        QVERIFY(!rendered.find("https://example.org/code").charFormat().isAnchor());
        QVERIFY(rendered.toPlainText().contains("    https://example.org/code"));
    }
    void imageEmbeddingChangesOnlyImageSyntax_data() {
        QTest::addColumn<QString>("markdown");
        QTest::addColumn<QString>("expected");
        QTest::newRow("inline") << "Before ![photo](https://example.org/p.png) **after**." << "Before ![photo](%1) **after**.";
        QTest::newRow("code and escaped image")
            << "`![photo](https://example.org/p.png)` \\![photo](https://example.org/p.png)\n\n![photo](https://example.org/p.png)"
            << "`![photo](https://example.org/p.png)` \\![photo](https://example.org/p.png)\n\n![photo](%1)";
        QTest::newRow("shared reference") << "![photo][ref] [Open][ref]\n\n[ref]: https://example.org/p.png"
            << "![photo](%1) [Open][ref]\n\n[ref]: https://example.org/p.png";
        QTest::newRow("shortcut") << "![photo]\n\n[photo]: https://example.org/p.png"
            << "![photo](%1)\n\n[photo]: https://example.org/p.png";
        QTest::newRow("collapsed") << "![photo][]\n\n[photo]: https://example.org/p.png"
            << "![photo](%1)\n\n[photo]: https://example.org/p.png";
        QTest::newRow("nested target") << "![photo](https://example.org/p(one).png)" << "![photo](%1)";
        QTest::newRow("angle target and title") << "![photo](<https://example.org/p).png> \"A (caption)\")" << "![photo](%1 \"A (caption)\")";
        QTest::newRow("repeated image") << "![one](https://example.org/p.png) and ![two](https://example.org/p.png)"
            << "![one](%1) and ![two](%1)";
        QTest::newRow("nested label") << "![a [label]](https://example.org/p.png)" << "![a [label]](%1)";
        QTest::newRow("fenced example") << "```md\n![example](https://example.org/p.png)\n```\n\n![photo](https://example.org/p.png)"
            << "```md\n![example](https://example.org/p.png)\n```\n\n![photo](%1)";
        QTest::newRow("linked image") << "[![photo](https://example.org/p.png)](https://example.org/open)"
            << "[![photo](%1)](https://example.org/open)";
        QTest::newRow("unresolved multiline reference") << "![photo]\n[ref]\n\n[ref]: https://example.org/p.png"
            << "![photo]\n[ref]\n\n[ref]: https://example.org/p.png\n\n![Image](%1)";
        QTest::newRow("escaped label") << "![a \\] label](https://example.org/p.png)" << "![a \\] label](%1)";
        const auto code = "```md\n" + QString("![example](https://example.org/p.png)\n").repeated(300) + "```\n\n";
        QTest::newRow("large code example") << code + "![photo](https://example.org/p.png)" << code + "![photo](%1)";
    }
    void imageEmbeddingChangesOnlyImageSyntax() {
        QFETCH(QString, markdown);
        QFETCH(QString, expected);
        const QByteArray bytes("image fixture");
        const auto reference = "attachment:" + QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        const auto result = ChatContent::embedImage(markdown, bytes);
        QCOMPARE(result, expected.arg(reference));
        QCOMPARE(ChatContent::embedImage(result, bytes), result);
    }
    void formatKeepsRepeatedImagesAndIgnoresCodeImages() {
        ChatContent content;
        const QByteArray bytes("image fixture");
        const auto hash = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        QImage image(8, 8, QImage::Format_RGBA8888);
        image.fill(Qt::green);
        QByteArray png;
        QBuffer buffer(&png);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(image.save(&buffer, "PNG"));
        const auto html = content.format(
            "`![code](attachment:" + hash + ")`\n\n![one](attachment:" + hash + ") ![two](attachment:" + hash + ")",
            {{"hash", hash}, {"source", "data:image/png;base64," + QString::fromLatin1(png.toBase64())},
             {"width", image.width()}, {"height", image.height()}});
        QTextDocument rendered;
        rendered.setHtml(html);
        int imageCount = 0;
        for (auto block = rendered.begin(); block.isValid(); block = block.next())
            for (auto it = block.begin(); !it.atEnd(); ++it)
                if (it.fragment().isValid() && it.fragment().charFormat().isImageFormat()) ++imageCount;
        QCOMPARE(imageCount, 2);
        QVERIFY(rendered.toPlainText().contains("![code](attachment:"));
    }
    void markdownLinkBoundaries_data() {
        QTest::addColumn<QString>("markdown");
        QTest::addColumn<QString>("label");
        QTest::addColumn<QString>("target");
        QTest::newRow("raw punctuation") << "See https://example.org/a?q=1#part." << "https://example.org/a?q=1#part" << "https://example.org/a?q=1#part";
        QTest::newRow("inline code") << "`https://example.org/a?q=1#part`" << "https://example.org/a?q=1#part" << "";
        QTest::newRow("fenced code") << "```\nhttps://example.org/a?q=1#part\n```" << "https://example.org/a?q=1#part" << "";
        QTest::newRow("reference") << "[Named][site]\n\n[site]: https://example.org/a?q=1#part" << "Named" << "https://example.org/a?q=1#part";
        QTest::newRow("unsafe quote") << "> [Blocked](javascript:alert)" << "Blocked" << "";
        QTest::newRow("unsafe code label") << "[`Blocked`](file:///secret)" << "Blocked" << "";
        QTest::newRow("html") << "<a href=\"https://example.org\">Literal</a>" << "Literal" << "";
        QTest::newRow("unfinished") << "[unfinished](https://example.org" << "unfinished" << "";
    }
    void markdownLinkBoundaries() {
        QFETCH(QString, markdown);
        QFETCH(QString, label);
        QFETCH(QString, target);
        ChatContent content;
        QTextDocument rendered;
        rendered.setHtml(content.format(markdown));
        const auto cursor = rendered.find(label);
        QVERIFY(!cursor.isNull());
        QCOMPARE(cursor.charFormat().isAnchor(), !target.isEmpty());
        QCOMPARE(cursor.charFormat().anchorHref(), target);
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
