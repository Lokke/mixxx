#include "library/opensubsonic/opensubsoniccache.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>

#if defined(Q_OS_UNIX)
#include <cerrno>
#include <csignal>
#include <sys/types.h>
#elif defined(Q_OS_WIN)
#include <windows.h>
#endif

namespace mixxx {
namespace opensubsonic {

namespace {

const QString kSessionCacheDirectoryPrefix = QStringLiteral("mixxx-opensubsonic-");

/// Base directory for the volatile audio buffer. RAM backed locations are
/// preferred so that no audio data ends up on persistent storage.
QString sessionCacheBasePath() {
    // Per user runtime directory, a tmpfs that is wiped on logout.
    QString basePath =
            QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (basePath.isEmpty()) {
        const QString sharedMemoryPath = QStringLiteral("/dev/shm");
        const QFileInfo sharedMemoryInfo(sharedMemoryPath);
        if (sharedMemoryInfo.isDir() && sharedMemoryInfo.isWritable()) {
            basePath = sharedMemoryPath;
        }
    }
    if (basePath.isEmpty()) {
        // Last resort, may be backed by a regular disk.
        basePath = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    }
    if (basePath.isEmpty()) {
        basePath = QDir::tempPath();
    }
    return basePath;
}

bool isProcessAlive(qint64 processId) {
    if (processId <= 0) {
        return false;
    }
#if defined(Q_OS_UNIX)
    // Signal 0 only performs the existence and permission check.
    return ::kill(static_cast<pid_t>(processId), 0) == 0 || errno == EPERM;
#elif defined(Q_OS_WIN)
    const HANDLE hProcess =
            ::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(processId));
    if (hProcess == nullptr) {
        // Access denied means the process exists but cannot be inspected.
        return ::GetLastError() == ERROR_ACCESS_DENIED;
    }
    const DWORD waitResult = ::WaitForSingleObject(hProcess, 0);
    ::CloseHandle(hProcess);
    return waitResult == WAIT_TIMEOUT;
#else
    // Without a reliable way to probe a process assume it is alive so that
    // the buffer of a concurrent instance is never deleted.
    Q_UNUSED(processId);
    return true;
#endif
}

QString sanitizeFileComponent(const QString& value) {
    QString sanitized = value;
    static const QRegularExpression kInvalidCharacters(
            QStringLiteral("[^A-Za-z0-9._-]"));
    sanitized.replace(kInvalidCharacters, QStringLiteral("_"));
    return sanitized;
}

} // namespace

QString cacheRootPath() {
    return sessionCacheBasePath() + QLatin1Char('/') +
            kSessionCacheDirectoryPrefix +
            QString::number(QCoreApplication::applicationPid());
}

void prepareSessionCache() {
    const QString cacheRoot = cacheRootPath();
    const QDir baseDir(QFileInfo(cacheRoot).absolutePath());
    if (baseDir.exists()) {
        // Drop leftovers of runs that are no longer alive, e.g. after a crash.
        // The buffer of a concurrently running instance must be kept.
        const QStringList staleEntries = baseDir.entryList(
                QStringList{kSessionCacheDirectoryPrefix + QStringLiteral("*")},
                QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString& staleEntry : staleEntries) {
            bool isNumeric = false;
            const qint64 processId = staleEntry
                                             .mid(kSessionCacheDirectoryPrefix.size())
                                             .toLongLong(&isNumeric);
            if (isNumeric && isProcessAlive(processId)) {
                continue;
            }
            QDir(baseDir.filePath(staleEntry)).removeRecursively();
        }
    }
    ensureSessionCacheDirectory();
}

bool ensureSessionCacheDirectory() {
    const QString cacheRoot = cacheRootPath();
    if (!QDir().mkpath(cacheRoot)) {
        return false;
    }
    // The buffered audio data is private to the current user.
    return QFile::setPermissions(cacheRoot,
            QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                    QFileDevice::ExeOwner);
}

void purgeSessionCache() {
    QDir(cacheRootPath()).removeRecursively();
}

bool isSessionCacheFile(const QString& path) {
    if (path.isEmpty()) {
        return false;
    }
    const QString cacheRoot = QDir(cacheRootPath()).absolutePath();
    const QString absolutePath = QFileInfo(path).absoluteFilePath();
    return absolutePath.startsWith(cacheRoot + QLatin1Char('/'));
}

QString cachedSongFilePath(const Song& song) {
    QString suffix = sanitizeFileComponent(song.suffix.toLower());
    if (suffix.isEmpty()) {
        // A known file name suffix is required to select a decoder.
        suffix = QStringLiteral("bin");
    }
    QString id = sanitizeFileComponent(song.id);
    if (id.isEmpty()) {
        id = QStringLiteral("unknown");
    }
    return cacheRootPath() + QLatin1Char('/') + id + QLatin1Char('.') + suffix;
}

} // namespace opensubsonic
} // namespace mixxx
