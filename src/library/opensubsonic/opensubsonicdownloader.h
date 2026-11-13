#pragma once

#include <QFile>
#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QString>

#include <memory>

#include "library/opensubsonic/opensubsonictypes.h"

class QNetworkReply;

namespace mixxx {
namespace opensubsonic {

class SubsonicClient;

/// Downloads songs sequentially into the volatile audio buffer.
///
/// Mixxx' audio engine only accepts local files, so every track that should
/// be dragged onto a deck has to be materialised as a file first. The buffer
/// is located on a RAM backed filesystem and is released again as soon as the
/// track is removed from all players, so no audio data is stored persistently.
/// The downloader keeps a small queue and never runs more than one transfer at
/// a time.
class OpenSubsonicDownloader : public QObject {
    Q_OBJECT

  public:
    explicit OpenSubsonicDownloader(
            const SubsonicClient* pClient,
            QObject* parent = nullptr);
    ~OpenSubsonicDownloader() override;

    /// Queues a single song. Duplicates are ignored.
    void enqueue(const Song& song);
    /// Queues all songs that are not available locally yet.
    void enqueueAll(const QList<Song>& songs);
    /// Aborts the current transfer and drops all pending songs.
    void cancelAll();

    /// Whether the song is already buffered locally.
    static bool isCached(const Song& song);

  signals:
    void songDownloaded(const QString& songId, const QString& localPath);
    void downloadFailed(const QString& songId, const QString& errorMessage);
    void queueEmpty();

  private:
    void startNextDownload();
    void finishCurrentDownload(bool success, const QString& errorMessage);
    void resetCurrentDownload();

    const SubsonicClient* const m_pClient;

    QNetworkAccessManager m_network;
    QQueue<Song> m_queue;
    QSet<QString> m_scheduled;

    QNetworkReply* m_pReply;
    std::unique_ptr<QFile> m_pPartialFile;
    Song m_currentSong;
    QString m_currentPartPath;
};

} // namespace opensubsonic
} // namespace mixxx
