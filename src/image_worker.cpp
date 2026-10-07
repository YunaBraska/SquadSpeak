#include "chat_content.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QImageReader>

#include <cstdio>
#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

namespace {
constexpr qsizetype chunkSize = 64 * 1024;

int fail(const char* message) {
    std::fputs(message, stderr);
    std::fputc('\n', stderr);
    return 1;
}
}

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    QImageReader::setAllocationLimit(128);

#ifdef Q_OS_WIN
    if (_setmode(_fileno(stdin), _O_BINARY) == -1)
        return fail(QT_TRANSLATE_NOOP("ChatContent", "The image source could not be read."));
    if (_setmode(_fileno(stdout), _O_BINARY) == -1)
        return fail(QT_TRANSLATE_NOOP("ChatContent", "The sanitized image could not be written."));
#endif
    QFile input;
    if (!input.open(stdin, QIODevice::ReadOnly)) return fail(QT_TRANSLATE_NOOP("ChatContent", "The image source could not be read."));
    QByteArray source;
    while (!input.atEnd()) {
        const auto chunk = input.read(chunkSize);
        if (chunk.isEmpty()) {
            if (input.error() != QFileDevice::NoError) return fail(QT_TRANSLATE_NOOP("ChatContent", "Error while reading the image source."));
            break;
        }
        if (source.size() > ChatContent::maximumSourceBytes - chunk.size())
            return fail(QT_TRANSLATE_NOOP("ChatContent", "The image source is larger than 25 MiB."));
        source += chunk;
    }

    try {
        const auto png = ChatContent::sanitizeImage(source);
        QFile output;
        if (!output.open(stdout, QIODevice::WriteOnly)) return fail(QT_TRANSLATE_NOOP("ChatContent", "The sanitized image could not be written."));
        if (output.write(png) != png.size() || !output.flush()) return fail(QT_TRANSLATE_NOOP("ChatContent", "The sanitized image could not be written completely."));
        return 0;
    } catch (const std::exception& error) {
        return fail(error.what());
    }
}
