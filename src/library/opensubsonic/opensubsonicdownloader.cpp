#include "library/navidrome/navidromedownloader.h"

#include <QDir>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>

#include "library/navidrome/navidromecache.h"
#include "library/navidrome/subsonicclient.h"
#include "util/logger.h"

namespace mixxx {
namespace navidrome {

namespace {

const Logger kLogger("NavidromeDownloader");

constexpr int kTransferTimeoutMillis = 60000;

} // namespace

NavidromeDownloader::NavidromeDownloader(
        const SubsonicClient* pClient,
        QObject* parent)
        : QObject(parent),
          m_pClient(pClient),
          m_pReply(nullptr) {
}

NavidromeDownloader::~NavidromeDownloader() {
    cancelAll();
}

bool NavidromeDownloader::isCached(const Song& song) {
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

void NavidromeDownloader::enqueue(const Song& song) {
    if (song.id.isEmpty() || isCached(song) || m_scheduled.contains(song.id)) {
        return;
    }
    m_scheduled.insert(song.id);
    m_queue.enqueue(song);
    startNextDownload();
}

void NavidromeDownloader::enqueueAll(const QList<Song>& songs) {
    for (const Song& song : songs) {
        if (song.id.isEmpty() || isCached(song) || m_scheduled.contains(song.id)) {
            continue;
        }
        m_scheduled.insert(song.id);
        m_queue.enqueue(song);
    }
    startNextDownload();
}

void NavidromeDownloader::cancelAll() {
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

void NavidromeDownloader::startNextDownload() {
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
    const QFileInfo localFileInfo(localPath);
    if (!QDir().mkpath(localFileInfo.absolutePath())) {
        finishCurrentDownload(false,
                tr("Could not create cache directory '%1'")
                        .arg(localFileInfo.absolutePath()));
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

void NavidromeDownloader::finishCurrentDownload(
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
                this, &NavidromeDownloader::startNextDownload, Qt::QueuedConnection);
    }
}

void NavidromeDownloader::resetCurrentDownload() {
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

} // namespace navidrome
} // namespace mixxx

#include "moc_navidromedownloader.cpp"
