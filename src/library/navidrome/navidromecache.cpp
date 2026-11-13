#include "library/navidrome/navidromecache.h"

#include <QDir>
#include <QRegularExpression>
#include <QStandardPaths>

namespace mixxx {
namespace navidrome {

namespace {

QString sanitizeFileComponent(const QString& value) {
    QString sanitized = value;
    static const QRegularExpression kInvalidCharacters(
            QStringLiteral("[^A-Za-z0-9._-]"));
    sanitized.replace(kInvalidCharacters, QStringLiteral("_"));
    return sanitized;
}

} // namespace

QString cacheRootPath() {
    QString basePath = QStandardPaths::writableLocation(
            QStandardPaths::CacheLocation);
    if (basePath.isEmpty()) {
        basePath = QDir::homePath() + QStringLiteral("/.cache/mixxx");
    }
    return basePath + QStringLiteral("/navidrome");
}

QString cachedSongFilePath(const Song& song) {
    QString suffix = sanitizeFileComponent(song.suffix.toLower());
    if (suffix.isEmpty()) {
        // Mixxx requires a known file name suffix to select a decoder.
        suffix = QStringLiteral("bin");
    }
    QString id = sanitizeFileComponent(song.id);
    if (id.isEmpty()) {
        id = QStringLiteral("unknown");
    }
    return cacheRootPath() + QLatin1Char('/') + id + QLatin1Char('.') + suffix;
}

} // namespace navidrome
} // namespace mixxx
