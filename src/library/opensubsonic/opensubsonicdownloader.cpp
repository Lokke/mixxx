#include "library/opensubsonic/opensubsonicdownloader.h"

#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>

#include "library/opensubsonic/opensubsoniccache.h"
#include "library/opensubsonic/subsonicclient.h"
#include "util/logger.h"

namespace mixxx {
namespace opensubsonic {

namespace {

const Logger kLogger("OpenSubsonicDownloader");

constexpr int kTransferTimeoutMillis = 60000;

} // namespace

OpenSubsonicDownloader::OpenSubsonicDownloader(
        const SubsonicClient* pClient,
        QObject* parent)
        : QObject(parent),
          m_pClient(pClient),
          m_pReply(nullptr) {
}

OpenSubsonicDownloader::~OpenSubsonicDownloader() {
    cancelAll();
}

bool OpenSubsonicDownloader::isCached(const Song& song) {
    const QFileInfo fileInfo(cachedSongFilePath(song));
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        return false;
    }
    if (song.size > 0 && fileInfo.size() != song.size) {
        // Incomplete or outdated download, fetch it again.
        return false;
    }
    return true;
}

void OpenSubsonicDownloader::enqueue(const Song& song) {
    if (song.id.isEmpty() || isCached(song) || m_scheduled.contains(song.id)) {
        return;
    }
    m_scheduled.insert(song.id);
    m_queue.enqueue(song);
    startNextDownload();
}

void OpenSubsonicDownloader::enqueueAll(const QList<Song>& songs) {
    for (const Song& song : songs) {
        if (song.id.isEmpty() || isCached(song) || m_scheduled.contains(song.id)) {
            continue;
        }
        m_scheduled.insert(song.id);
        m_queue.enqueue(song);
    }
    startNextDownload();
}

void OpenSubsonicDownloader::cancelAll() {
    m_queue.clear();
    m_scheduled.clear();
    if (m_pReply) {
        // Disconnect first so that finishCurrentDownload() is not triggered
        // by the abort() of our own destructor.
        m_pReply->disconnect(this);
        m_pReply->abort();
        m_pReply->deleteLater();
        m_pReply = nullptr;
    }
    resetCurrentDownload();
}

void OpenSubsonicDownloader::startNextDownload() {
    if (m_pReply || m_queue.isEmpty() || !m_pClient) {
        return;
    }

    while (!m_queue.isEmpty()) {
        m_currentSong = m_queue.dequeue();
        if (!isCached(m_currentSong)) {
            break;
        }
        m_scheduled.remove(m_currentSong.id);
        m_currentSong = Song();
    }
    if (m_currentSong.id.isEmpty()) {
        emit queueEmpty();
        return;
    }

    const QString localPath = cachedSongFilePath(m_currentSong);
    if (!ensureSessionCacheDirectory()) {
        finishCurrentDownload(false,
                tr("Could not create buffer directory '%1'")
                        .arg(QFileInfo(localPath).absolutePath()));
        return;
    }

    m_currentPartPath = localPath + QStringLiteral(".part");
    m_pPartialFile = std::make_unique<QFile>(m_currentPartPath);
    if (!m_pPartialFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        const QString errorMessage = m_pPartialFile->errorString();
        finishCurrentDownload(false, errorMessage);
        return;
    }

    QNetworkRequest request(m_pClient->downloadUrl(m_currentSong));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
            QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(kTransferTimeoutMillis);

    kLogger.debug() << "Downloading" << m_currentSong.artist
                    << "-" << m_currentSong.title;

    m_pReply = m_network.get(request);
    connect(m_pReply,
            &QNetworkReply::readyRead,
            this,
            [this]() {
                if (m_pReply && m_pPartialFile) {
                    m_pPartialFile->write(m_pReply->readAll());
                }
            });
    connect(m_pReply,
            &QNetworkReply::finished,
            this,
            [this]() {
                const bool success =
                        m_pReply && m_pReply->error() == QNetworkReply::NoError;
                const QString errorMessage =
                        (m_pReply && !success) ? m_pReply->errorString()
                                               : QString();
                if (success && m_pReply && m_pPartialFile) {
                    // Flush whatever is left in the reply buffer.
                    m_pPartialFile->write(m_pReply->readAll());
                }
                finishCurrentDownload(success, errorMessage);
            });
}

void OpenSubsonicDownloader::finishCurrentDownload(
        bool success,
        const QString& errorMessage) {
    const QString songId = m_currentSong.id;
    const QString localPath = cachedSongFilePath(m_currentSong);

    resetCurrentDownload();

    if (success && QFile::exists(localPath)) {
        // Remove a possibly existing outdated file before replacing it.
        QFile::remove(localPath);
    }
    if (success && !m_currentPartPath.isEmpty()) {
        success = QFile::rename(m_currentPartPath, localPath);
    }
    if (!success) {
        if (!m_currentPartPath.isEmpty()) {
            QFile::remove(m_currentPartPath);
        }
    }
    m_currentPartPath.clear();

    if (success) {
        kLogger.debug() << "Download finished for" << songId;
        emit songDownloaded(songId, localPath);
    } else {
        // Allow a later retry.
        m_scheduled.remove(songId);
        kLogger.warning() << "Download failed for" << songId << errorMessage;
        emit downloadFailed(songId, errorMessage);
    }

    m_currentSong = Song();
    if (m_queue.isEmpty()) {
        emit queueEmpty();
    } else {
        // Queued to avoid deep recursion when many downloads fail in a row.
        QMetaObject::invokeMethod(
                this, &OpenSubsonicDownloader::startNextDownload, Qt::QueuedConnection);
    }
}

void OpenSubsonicDownloader::resetCurrentDownload() {
    if (m_pPartialFile) {
        m_pPartialFile->close();
        m_pPartialFile.reset();
    }
    if (m_pReply) {
        m_pReply->disconnect(this);
        m_pReply->deleteLater();
        m_pReply = nullptr;
    }
}

} // namespace opensubsonic
} // namespace mixxx

#include "moc_opensubsonicdownloader.cpp"
