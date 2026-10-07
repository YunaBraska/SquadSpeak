#include "chat_content.hpp"

#include <QBuffer>
#include <QDesktopServices>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QClipboard>
#include <QMimeData>
#include <QFile>
#include <QProcess>
#include <QTimer>
#include <QSharedPointer>
#include <QImageReader>
#include <QImageWriter>
#include <QPainter>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QFontDatabase>
#include <QFontInfo>
#include <QTextImageFormat>
#include <QCryptographicHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSet>
#include <QtEndian>
#include <algorithm>
#include <stdexcept>

namespace {
class DisplayDocument final : public QTextDocument {
    QVariant loadResource(int, const QUrl&) override { return {}; }
};
}

bool ChatContent::allowedLink(const QUrl& link) {
    return link.isValid() && (link.scheme() == "https" || link.scheme() == "http")
        && !link.host().isEmpty() && link.userInfo().isEmpty();
}

bool ChatContent::openLink(const QString& link) const {
    const QUrl url(link, QUrl::StrictMode);
    return allowedLink(url) && QDesktopServices::openUrl(url);
}

QString ChatContent::format(const QString& markdown, const QVariantMap& attachment, const QColor& linkColor,
                            const QColor& codeBackground, const QFont& font) const {
    DisplayDocument document;
    document.setDefaultFont(font);
    document.setMarkdown(markdown, QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub) | QTextDocument::MarkdownNoHTML);
    auto codeFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    codeFont.setStyleHint(QFont::Monospace);
    codeFont.setFixedPitch(true);
    const auto codeFamily = QFontInfo(codeFont).family();
    const auto codeBlock = [](const QTextBlock& block) {
        const auto format = block.blockFormat();
        return format.hasProperty(QTextFormat::BlockCodeFence) || format.hasProperty(QTextFormat::BlockCodeLanguage)
            || format.nonBreakableLines();
    };
    struct Change { int start; int length; QTextCharFormat format; QString text; bool replace; };
    QList<Change> changes;
    for (auto block = document.begin(); block.isValid(); block = block.next()) {
        const bool fenced = codeBlock(block);
        const int quote = block.blockFormat().intProperty(QTextFormat::BlockQuoteLevel);
        if (fenced || quote > 0) {
            auto format = block.blockFormat();
            format.setLeftMargin(fenced ? 8 : 12 * quote);
            format.setRightMargin(8);
            if (fenced) {
                format.setBackground(codeBackground);
                format.setTopMargin(codeBlock(block.previous()) ? 0 : 8);
                format.setBottomMargin(codeBlock(block.next()) ? 0 : 8);
            }
            QTextCursor(block).setBlockFormat(format);
        }
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (!fragment.isValid()) continue;
            auto format = fragment.charFormat();
            if (format.isImageFormat()) {
                const auto image = format.toImageFormat();
                const auto link = image.name();
                const auto source = attachment.value("source").toString();
                if (link == "attachment:" + attachment.value("hash").toString()
                    && source.startsWith("data:image/png;base64,") && attachment.value("width").toInt() > 0
                    && attachment.value("height").toInt() > 0) {
                    auto embedded = image;
                    embedded.setName(source);
                    const auto width = std::clamp(attachment.value("displayWidth", 300).toDouble(), 1.0, 800.0);
                    const auto scale = std::min({1.0, width / attachment.value("width").toInt(), 240.0 / attachment.value("height").toInt()});
                    embedded.setWidth(attachment.value("width").toInt() * scale);
                    embedded.setHeight(attachment.value("height").toInt() * scale);
                    changes.append({fragment.position(), fragment.length(), embedded, {}, false});
                    continue;
                }
                QTextCharFormat replacement;
                if (allowedLink(QUrl(link))) { replacement.setAnchor(true); replacement.setAnchorHref(link); replacement.setForeground(linkColor); }
                auto description = image.property(QTextFormat::ImageAltText).toString();
                if (description.isEmpty()) description = tr("Image");
                changes.append({fragment.position(), fragment.length(), replacement, description, true});
                continue;
            }
            const bool code = fenced || format.fontFixedPitch() || format.fontFamilies().toStringList().contains("monospace");
            const bool anchor = format.isAnchor();
            if (anchor) {
                if (allowedLink(QUrl(format.anchorHref()))) format.setForeground(linkColor);
                else { format.setAnchor(false); format.setAnchorHref({}); format.clearForeground(); }
            }
            if (code) {
                format.setFontFamilies({codeFamily});
                format.setFontFixedPitch(true);
                format.setBackground(codeBackground);
            } else if (quote > 0) {
                format.setFontItalic(true);
                format.setForeground(linkColor);
            }
            if (code || quote > 0 || anchor)
                changes.append({fragment.position(), fragment.length(), format, {}, false});
        }
    }
    // Reverse order keeps positions stable while replacing image objects.
    for (auto it = changes.crbegin(); it != changes.crend(); ++it) {
        QTextCursor cursor(&document);
        cursor.setPosition(it->start); cursor.setPosition(it->start + it->length, QTextCursor::KeepAnchor);
        if (it->replace) cursor.insertText(it->text, it->format); else cursor.setCharFormat(it->format);
    }
    return document.toHtml();
}

QString ChatContent::embedImage(const QString& markdown, const QByteArray& image) {
    const auto reference = "attachment:" + QString::fromLatin1(QCryptographicHash::hash(image, QCryptographicHash::Sha256).toHex());
    DisplayDocument document;
    document.setMarkdown(markdown, QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub) | QTextDocument::MarkdownNoHTML);
    QList<QPair<int, QTextImageFormat>> images;
    QSet<QString> sources;
    for (auto block = document.begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (!fragment.charFormat().isImageFormat()) continue;
            auto format = fragment.charFormat().toImageFormat();
            sources.insert(format.name()); format.setName(reference);
            images.append({fragment.position(), format});
        }
    if (sources.size() > 1) throw std::runtime_error(tr("Each message can contain one image. Send additional images separately.").toStdString());
    if (images.isEmpty()) return markdown + (markdown.isEmpty() ? "" : "\n\n") + "![Image](" + reference + ")";
    if (*sources.begin() == reference) return markdown;
    auto result = markdown;
    qsizetype search = 0;
    for (const auto& [position, format] : images) {
        QTextCursor current(&document);
        current.setPosition(position); current.setPosition(position + 1, QTextCursor::KeepAnchor);
        if (current.charFormat().toImageFormat().name() == reference) continue;
        auto title = format.stringProperty(QTextFormat::ImageTitle);
        title.replace("\\", "\\\\").replace("\"", "\\\"");
        const auto expected = document.toHtml();
        const auto expectedText = document.toPlainText();
        bool replaced = false;
        // Locate candidate image syntax, then let Qt prove that replacing it
        // changes only this image. Code, links and reference definitions stay intact.
        const auto closing = [&result](qsizetype start, QChar open, QChar close) {
            int depth = 0;
            QChar quote;
            for (auto i = start; i < result.size(); ++i) {
                const auto c = result[i];
                if (c == '\\') { ++i; continue; }
                if (!quote.isNull()) { if (c == quote) quote = {}; continue; }
                if (open == '(' && (c == '<' || ((c == '\'' || c == '"') && i > start && result[i - 1].isSpace()))) {
                    quote = c == '<' ? QChar('>') : c; continue;
                }
                if (c == open) ++depth;
                else if (c == close && --depth == 0) return i;
            }
            return qsizetype(-1);
        };
        for (auto start = result.indexOf("![", search); start >= 0 && !replaced; start = result.indexOf("![", start + 2)) {
            const auto label = closing(start + 1, '[', ']');
            if (label < 0) continue;
            const auto replacement = result.mid(start, label - start + 1) + '(' + reference
                + (title.isEmpty() ? QString{} : " \"" + title + '"') + ')';
            QList<qsizetype> ends{label};
            auto next = label + 1;
            while (next < result.size() && result[next].isSpace()) ++next;
            if (next < result.size() && (result[next] == '(' || result[next] == '[')) {
                const auto end = closing(next, result[next], result[next] == '(' ? ')' : ']');
                if (end >= 0) ends.append(end);
            }
            for (const auto end : ends) {
                auto candidate = result;
                candidate.replace(start, end - start + 1, replacement);
                DisplayDocument probe;
                probe.setMarkdown(candidate, QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub) | QTextDocument::MarkdownNoHTML);
                if (probe.toPlainText() != expectedText) continue;
                QTextCursor cursor(&probe);
                cursor.setPosition(position); cursor.setPosition(position + 1, QTextCursor::KeepAnchor);
                if (!cursor.charFormat().isImageFormat() || cursor.charFormat().toImageFormat().name() != reference) continue;
                for (const auto& image : images) {
                    const auto imagePosition = image.first;
                    current.setPosition(imagePosition); current.setPosition(imagePosition + 1, QTextCursor::KeepAnchor);
                    cursor.setPosition(imagePosition); cursor.setPosition(imagePosition + 1, QTextCursor::KeepAnchor);
                    if (cursor.charFormat().isImageFormat() && cursor.charFormat().toImageFormat().name() == reference)
                        cursor.setCharFormat(current.charFormat());
                }
                if (probe.toHtml() != expected) continue;
                result = std::move(candidate);
                document.setMarkdown(result, QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub) | QTextDocument::MarkdownNoHTML);
                search = start + replacement.size();
                replaced = true;
                break;
            }
        }
        if (!replaced) throw std::runtime_error(tr("Image could not be sanitized.").toStdString());
    }
    return result;
}

bool ChatContent::prepareMessage(const QString& markdown) {
    if (busy_) return false;
    if (markdown.toUtf8().size() > 16384) { error_ = tr("Message is too long (maximum 16 KiB)."); emit changed(); return false; }
    DisplayDocument document;
    document.setMarkdown(markdown, QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub) | QTextDocument::MarkdownNoHTML);
    QSet<QString> images;
    for (auto block = document.begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it)
            if (it.fragment().charFormat().isImageFormat()) images.insert(it.fragment().charFormat().toImageFormat().name());
    if (images.size() > 1) { error_ = tr("Each message can contain one image. Send additional images separately."); emit changed(); return false; }
    const auto finish = [this, markdown](QByteArray png, QString error) {
        busy_ = false; error_ = std::move(error);
        if (error_.isEmpty()) {
            prepared_ = std::move(png);
            QString text;
            try { text = prepared_.isEmpty() ? markdown : embedImage(markdown, prepared_); }
            catch (const std::exception& error) { error_ = QString::fromUtf8(error.what()); emit changed(); return; }
            if (text.toUtf8().size() > 16384) error_ = tr("Message with image reference is too long (maximum 16 KiB).");
            else { emit changed(); emit messagePrepared(text, prepared_); return; }
        }
        emit changed();
    };
    if (images.isEmpty()) { preparedUrl_.clear(); finish(prepared_, {}); return true; }
    const auto link = *images.begin();
    const auto internal = "attachment:" + QString::fromLatin1(QCryptographicHash::hash(prepared_, QCryptographicHash::Sha256).toHex());
    if (!prepared_.isEmpty() && (link == preparedUrl_ || link == internal)) { finish(prepared_, {}); return true; }
    if (!prepared_.isEmpty() && preparedUrl_.isEmpty()) {
        error_ = tr("The message already contains a selected image. Remove the image URL or attachment."); emit changed(); return false;
    }
    const QUrl url(link, QUrl::StrictMode);
    if (!allowedLink(url)) { error_ = tr("Image URL must use HTTP or HTTPS. Select local images."); emit changed(); return false; }
    busy_ = true; prepared_.clear(); error_.clear(); preparedUrl_ = link; emit changed();
    auto* manager = new QNetworkAccessManager(this);
    QNetworkRequest request(url);
    request.setMaximumRedirectsAllowed(5);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setTransferTimeout(20000);
    auto* reply = manager->get(request);
    connect(reply, &QNetworkReply::redirected, this, [reply](const QUrl& target) { if (!allowedLink(target)) reply->abort(); });
    reply->setReadBufferSize(65536);
    auto bytes = QSharedPointer<QByteArray>::create();
    const auto read = [reply, bytes] {
        if (!reply->isReadable()) return;
        bytes->append(reply->read(maximumSourceBytes + 1 - bytes->size()));
        if (bytes->size() > maximumSourceBytes) reply->abort();
    };
    connect(reply, &QNetworkReply::readyRead, this, read);
    connect(reply, &QNetworkReply::metaDataChanged, this, [reply] {
        if (reply->header(QNetworkRequest::ContentLengthHeader).toLongLong() > maximumSourceBytes) reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, manager, bytes, read, finish] {
        read();
        const auto error = reply->error();
        manager->deleteLater();
        if (error != QNetworkReply::NoError || bytes->size() > maximumSourceBytes) {
            finish({}, tr("Image could not be loaded. Check the URL, connection, and 25 MiB size limit.")); return;
        }
        if (bytes->isEmpty()) { finish({}, tr("The image source is empty.")); return; }
        finish(*bytes, {});
    });
    QTimer::singleShot(20000, reply, [reply] { if (!reply->isFinished()) reply->abort(); });
    return true;
}

QByteArray ChatContent::sanitizeImage(const QByteArray& source) {
    if (source.isEmpty() || source.size() > maximumSourceBytes) throw std::runtime_error(tr("Image file is empty or larger than 25 MiB.").toStdString());
    QBuffer input; input.setData(source); input.open(QIODevice::ReadOnly);
    QByteArray format;
    if (source.startsWith(QByteArray::fromHex("89504e470d0a1a0a"))) format = "png";
    else if (source.startsWith(QByteArray::fromHex("ffd8ff"))) format = "jpeg";
    else if (source.startsWith("GIF87a") || source.startsWith("GIF89a")) format = "gif";
    else if (source.startsWith("BM")) format = "bmp";
    else if (source.startsWith("RIFF") && source.mid(8, 4) == "WEBP") format = "webp";
    else throw std::runtime_error(tr("Supported formats are PNG, JPEG, WebP, GIF, and BMP.").toStdString());
    QImageReader reader(&input, format);
    reader.setAutoDetectImageFormat(false);
    const auto size = reader.size();
    if (!size.isValid() || size.isEmpty())
        throw std::runtime_error(tr("Image could not be decoded safely.").toStdString());
    if (size.width() > 8192 || size.height() > 8192 || qint64(size.width()) * size.height() > maximumPixels)
        throw std::runtime_error(tr("Image is too large: maximum 8192 pixels per side and 24 megapixels.").toStdString());
    reader.setAutoTransform(true);
    const auto decoded = reader.read();
    if (decoded.isNull() || qint64(decoded.width()) * decoded.height() > maximumPixels)
        throw std::runtime_error(tr("Image could not be decoded safely.").toStdString());
    QImage pixels(decoded.size(), QImage::Format_ARGB32_Premultiplied);
    if (pixels.isNull()) throw std::runtime_error(tr("Not enough memory for the image.").toStdString());
    pixels.fill(Qt::transparent);
    QPainter painter(&pixels); painter.drawImage(0, 0, decoded); painter.end();
    QByteArray png; QBuffer output(&png); output.open(QIODevice::WriteOnly);
    QImageWriter writer(&output, "png");
    if (!writer.write(pixels) || png.size() > maximumImageBytes)
        throw std::runtime_error(tr("Sanitized image is larger than 8 MiB or could not be created.").toStdString());
    return png;
}

QSize ChatContent::sanitizedImageSize(const QByteArray& png) {
    const auto invalid = [] { throw std::invalid_argument(tr("Invalid sanitized PNG.").toStdString()); };
    if (png.size() < 45 || png.size() > maximumImageBytes
        || !png.startsWith(QByteArray::fromHex("89504e470d0a1a0a"))) invalid();
    QSize size;
    bool data = false;
    for (qsizetype offset = 8; offset + 12 <= png.size();) {
        const auto length = qFromBigEndian<quint32>(png.constData() + offset);
        if (length > quint64(png.size() - offset - 12)) invalid();
        const auto type = png.mid(offset + 4, 4);
        if (offset == 8) {
            if (type != "IHDR" || length != 13) invalid();
            const auto w = qFromBigEndian<quint32>(png.constData() + offset + 8);
            const auto h = qFromBigEndian<quint32>(png.constData() + offset + 12);
            if (w < 1 || h < 1 || w > 8192 || h > 8192 || quint64(w) * h > maximumPixels
                || png[offset + 16] != 8 || png[offset + 17] != 6
                || png[offset + 18] != 0 || png[offset + 19] != 0 || png[offset + 20] != 0) invalid();
            size = QSize(int(w), int(h));
        } else if (type == "IDAT") data = true;
        else if (type == "IEND") {
            if (!data || length != 0 || offset + 12 != png.size()) invalid();
            return size;
        } else if (type != "pHYs" || length != 9 || data) invalid();
        offset += qsizetype(length) + 12;
    }
    throw std::invalid_argument(tr("Invalid sanitized PNG.").toStdString());
}

bool ChatContent::prepare(const QByteArray& source, QObject* context, std::function<void(QByteArray, QString)> completion) {
    if (source.isEmpty() || source.size() > maximumSourceBytes) {
        completion({}, tr("Image file is empty or larger than 25 MiB.")); return false;
    }
    auto* process = new QProcess(context);
    struct State { QByteArray output; QByteArray diagnostic; bool done = false, timedOut = false; };
    const auto state = QSharedPointer<State>::create();
    const auto finish = [process, state, completion](QByteArray result, QString error) {
        if (state->done) return;
        state->done = true; process->deleteLater(); completion(std::move(result), std::move(error));
    };
    QObject::connect(process, &QProcess::readyReadStandardOutput, context, [process, state] {
        state->output.append(process->readAllStandardOutput());
        if (state->output.size() > maximumImageBytes) { state->output.clear(); process->kill(); }
    });
    const auto readDiagnostic = [process, state] {
        state->diagnostic = (state->diagnostic + process->readAllStandardError().right(2048)).right(2048);
    };
    QObject::connect(process, &QProcess::readyReadStandardError, context, readDiagnostic);
    QObject::connect(process, &QProcess::errorOccurred, context, [finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) finish({}, tr("Image decoder could not be started."));
    });
    QObject::connect(process, &QProcess::finished, context, [process, state, finish, readDiagnostic](int code, QProcess::ExitStatus status) {
        state->output.append(process->readAllStandardOutput());
        readDiagnostic();
        if (state->timedOut) finish({}, tr("Image processing took too long."));
        else if (status == QProcess::NormalExit && code == 0 && !state->output.isEmpty() && state->output.size() <= maximumImageBytes)
            finish(state->output, {});
        else {
            // The worker's final line is the owned error; earlier lines may be decoder diagnostics.
            const auto diagnostic = state->diagnostic.trimmed();
            const auto message = diagnostic.mid(diagnostic.lastIndexOf('\n') + 1);
            finish({}, message.isEmpty() ? tr("Image could not be sanitized.") : tr(message.constData()));
        }
    });
    QObject::connect(process, &QProcess::started, context, [process, source] { process->write(source); process->closeWriteChannel(); });
    QTimer::singleShot(20000, process, [process, state] {
        if (process->state() != QProcess::NotRunning) { state->timedOut = true; process->kill(); }
    });
#ifdef Q_OS_WIN
    const auto helper = QStringLiteral("/squad_image_worker.exe");
#else
    const auto helper = QStringLiteral("/squad_image_worker");
#endif
    process->start(QCoreApplication::applicationDirPath() + helper, {});
    return true;
}

bool ChatContent::prepareDraft(const QByteArray& source) {
    if (busy_) return false;
    prepared_.clear(); preparedUrl_.clear(); error_.clear();
    if (source.isEmpty() || source.size() > maximumSourceBytes) {
        error_ = tr("Image files must contain between 1 byte and 25 MiB."); emit changed(); return false;
    }
    prepared_ = source; emit changed(); return true;
}

bool ChatContent::prepareFile(const QUrl& path) {
    if (busy_) return false;
    QFile file(path.toLocalFile());
    if (!path.isLocalFile() || !file.open(QIODevice::ReadOnly) || file.size() > maximumSourceBytes) {
        error_ = tr("Image file cannot be read or is larger than 25 MiB."); emit changed(); return false;
    }
    return prepareDraft(file.read(maximumSourceBytes + 1));
}

bool ChatContent::pasteImage() {
    if (busy_) return false;
    const auto* clipboard = QGuiApplication::clipboard()->mimeData();
    if (clipboard) {
        for (const auto* format : {"image/png", "image/jpeg", "image/webp", "image/gif", "image/bmp"})
            if (clipboard->hasFormat(format)) return prepareDraft(clipboard->data(format));
    }
    // Some platforms expose only an already rendered native bitmap.
    const auto image = QGuiApplication::clipboard()->image();
    if (image.isNull() || qint64(image.width()) * image.height() > maximumPixels) {
        error_ = tr("No suitable image in the clipboard."); emit changed(); return false;
    }
    QByteArray encoded; QBuffer output(&encoded); output.open(QIODevice::WriteOnly);
    if (!image.save(&output, "PNG")) { error_ = tr("Clipboard image could not be read."); emit changed(); return false; }
    return prepareDraft(encoded);
}

void ChatContent::clearImage() { if (!busy_) { prepared_.clear(); preparedUrl_.clear(); error_.clear(); emit changed(); } }
