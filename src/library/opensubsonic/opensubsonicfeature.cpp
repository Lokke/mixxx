#include "library/opensubsonic/opensubsonicfeature.h"

#include <QModelIndex>
#include <QUrlQuery>
#include <utility>

#include "library/library.h"
#include "library/opensubsonic/opensubsonictrackmodel.h"
#include "library/opensubsonic/subsonicclient.h"
#include "library/treeitem.h"
#include "mixer/playerinfo.h"
#include "preferences/configobject.h"
#include "track/bpm.h"
#include "track/track.h"
#include "util/fileaccess.h"
#include "util/fileinfo.h"
#include "util/logger.h"

namespace {

const mixxx::Logger kLogger("OpenSubsonicFeature");

const ConfigKey kServerUrlConfigKey("[OpenSubsonic]", "server_url");
const ConfigKey kUserNameConfigKey("[OpenSubsonic]", "username");
const ConfigKey kPasswordConfigKey("[OpenSubsonic]", "password");

const QString kArtistsNode = QStringLiteral("artists");
const QString kArtistNodePrefix = QStringLiteral("artist:");
const QString kAlbumNodePrefix = QStringLiteral("album:");
const QString kInfoNode = QStringLiteral("info");

QString readSetting(
        const UserSettingsPointer& pConfig,
        const ConfigKey& configKey,
        const char* pEnvironmentVariableName) {
    QString value = pConfig->getValueString(configKey);
    if (value.isEmpty()) {
        // Migration: the feature was called "Navidrome" before, so read the
        // values of the legacy config group once instead of silently losing
        // the credentials of existing installations.
        const ConfigKey legacyConfigKey(QStringLiteral("[Navidrome]"),
                configKey.item);
        value = pConfig->getValueString(legacyConfigKey);
    }
    if (value.isEmpty()) {
        // Convenience for development builds without a settings dialog.
        value = qEnvironmentVariable(pEnvironmentVariableName);
    }
    return value;
}

/// Extracts the song id from a stream URL of the server, i.e. the value of
/// the `id` query parameter. Returns an empty string if the URL has not been
/// created by SubsonicClient.
QString songIdFromStreamUrl(const QUrl& url) {
    if (!url.isValid()) {
        return QString();
    }
    return QUrlQuery(url).queryItemValue(QStringLiteral("id"));
}

/// Creates a temporary track that is played directly from the server, i.e.
/// without downloading it first. The audio pipeline opens the URL through the
/// FFmpeg sound source.
TrackPointer createStreamTrack(
        const mixxx::opensubsonic::Song& song, const QUrl& streamUrl) {
    if (!streamUrl.isValid() || streamUrl.isLocalFile()) {
        return {};
    }
    auto pTrack = Track::newTemporary(
            mixxx::FileAccess(mixxx::FileInfo::fromRemoteUrl(streamUrl)));
    pTrack->setTitle(song.title);
    pTrack->setArtist(song.artistForDisplay());
    pTrack->setAlbum(song.album);
    pTrack->setAlbumArtist(song.albumArtistForDisplay());
    const QString genre = song.genreForDisplay();
    if (!genre.isEmpty()) {
        pTrack->updateGenre(genre);
    }
    pTrack->setComment(song.comment);
    if (song.year > 0) {
        pTrack->setYear(QString::number(song.year));
    }
    if (song.trackNumber > 0) {
        pTrack->setTrackNumber(QString::number(song.trackNumber));
    }
    pTrack->setType(song.suffix.toLower());
    pTrack->setDuration(song.duration);
    pTrack->setBitrate(song.bitRate);
    if (mixxx::Bpm::isValidValue(song.bpm)) {
        pTrack->trySetBpm(song.bpm);
    }
    if (song.created.isValid()) {
        pTrack->setDateAdded(song.created);
    }
    return pTrack;
}

} // namespace

OpenSubsonicFeature::OpenSubsonicFeature(
        Library* pLibrary,
        UserSettingsPointer pConfig,
        mixxx::DbConnectionPoolPtr pDbConnectionPool)
        : LibraryFeature(pLibrary, pConfig, QStringLiteral("tracks")),
          m_pSidebarModel(make_parented<TreeItemModel>(this)),
          m_pClient(new mixxx::opensubsonic::SubsonicClient(this)),
          m_pTrackModel(new OpenSubsonicTrackModel(
                  this, pLibrary->trackCollectionManager(), m_pClient)),
          m_pDbConnectionPool(std::move(pDbConnectionPool)),
          m_pAnalyzerThread(AnalyzerThread::NullPointer()) {
    connect(m_pClient,
            &mixxx::opensubsonic::SubsonicClient::artistsFetched,
            this,
            &OpenSubsonicFeature::slotArtistsFetched);
    connect(m_pClient,
            &mixxx::opensubsonic::SubsonicClient::albumsFetched,
            this,
            &OpenSubsonicFeature::slotAlbumsFetched);
    connect(m_pClient,
            &mixxx::opensubsonic::SubsonicClient::songsFetched,
            this,
            &OpenSubsonicFeature::slotSongsFetched);
    connect(m_pClient,
            &mixxx::opensubsonic::SubsonicClient::requestFailed,
            this,
            &OpenSubsonicFeature::slotRequestFailed);

    // The Mixxx search box calls search() on the currently shown track model.
    // Forward that to a server side search and feed the reply back.
    connect(m_pTrackModel,
            &OpenSubsonicTrackModel::searchRequested,
            m_pClient,
            &mixxx::opensubsonic::SubsonicClient::searchSongs);
    connect(m_pClient,
            &mixxx::opensubsonic::SubsonicClient::searchResultsFetched,
            m_pTrackModel,
            &OpenSubsonicTrackModel::setSearchResults);
    connect(m_pClient,
            &mixxx::opensubsonic::SubsonicClient::searchResultsFetched,
            this,
            [this](const QString&, const QList<mixxx::opensubsonic::Song>& songs) {
                rememberSongs(songs);
            });

    // Ratings edited in the track table are stored on the server (setRating).
    connect(m_pTrackModel,
            &OpenSubsonicTrackModel::ratingChangeRequested,
            m_pClient,
            &mixxx::opensubsonic::SubsonicClient::setRating);

    // Remote stream URLs (dropped onto a deck, double clicked, ...) end up
    // here: they are turned into tracks that the players can open.
    connect(m_pLibrary,
            &Library::loadRemoteUrlToPlayer,
            this,
            &OpenSubsonicFeature::slotLoadRemoteUrlToPlayer);

    // Streamed tracks are not part of the library, so nothing would analyze
    // them. Hand every track that ends up in a player to the analyzer thread.
    connect(&PlayerInfo::instance(),
            &PlayerInfo::trackChanged,
            this,
            &OpenSubsonicFeature::slotPlayerTrackChanged);

    reloadSettings();
}

OpenSubsonicFeature::~OpenSubsonicFeature() {
    if (m_pAnalyzerThread) {
        // Stops the worker thread and deletes it once it has finished.
        m_pAnalyzerThread->stop();
        m_pAnalyzerThread = AnalyzerThread::NullPointer();
    }
}

void OpenSubsonicFeature::scheduleStreamTrackAnalysis(
        const TrackPointer& pTrack) {
    DEBUG_ASSERT(pTrack);
    if (!m_pDbConnectionPool) {
        // Without a database connection no waveform analysis is possible.
        return;
    }
    if (!m_pAnalyzerThread) {
        startAnalyzerThread();
    }
    const QString songId = songIdFromStreamUrl(pTrack->getFileInfo().toQUrl());
    if (!songId.isEmpty()) {
        const auto pCachedWaveform = m_waveformsBySongId.value(songId);
        if (pCachedWaveform) {
            // The waveform of this song has already been computed, no need to
            // stream and analyze the audio data again.
            pTrack->setWaveform(pCachedWaveform);
            const auto pCachedSummary =
                    m_waveformSummariesBySongId.value(songId);
            if (pCachedSummary) {
                pTrack->setWaveformSummary(pCachedSummary);
            }
            return;
        }
        // Remember the result as soon as it becomes available.
        connect(pTrack.get(),
                &Track::waveformUpdated,
                this,
                [this, pTrack, songId]() {
                    if (pTrack->getWaveform()) {
                        // Keep the memory footprint of the cache bounded.
                        constexpr int kMaxCachedWaveforms = 64;
                        if (m_waveformsBySongId.size() >= kMaxCachedWaveforms) {
                            m_waveformsBySongId.clear();
                            m_waveformSummariesBySongId.clear();
                        }
                        m_waveformsBySongId.insert(songId, pTrack->getWaveform());
                        m_waveformSummariesBySongId.insert(
                                songId, pTrack->getWaveformSummary());
                    }
                });
    }
    m_pendingAnalysisTracks.append(pTrack);
    submitPendingStreamTrackAnalyses();
}

void OpenSubsonicFeature::startAnalyzerThread() {
    DEBUG_ASSERT(!m_pAnalyzerThread);
    m_pAnalyzerThread = AnalyzerThread::createInstance(
            0,
            m_pDbConnectionPool,
            m_pConfig,
            static_cast<AnalyzerModeFlags>(
                    AnalyzerModeFlags::WithWaveform |
                    AnalyzerModeFlags::LowPriority));
    connect(m_pAnalyzerThread.get(),
            &AnalyzerThread::progress,
            this,
            [this](int threadId,
                    AnalyzerThreadState threadState,
                    TrackId trackId,
                    AnalyzerProgress trackProgress) {
                Q_UNUSED(threadId);
                Q_UNUSED(trackId);
                Q_UNUSED(trackProgress);
                if (threadState == AnalyzerThreadState::Idle) {
                    m_analyzerThreadIdle = true;
                    submitPendingStreamTrackAnalyses();
                } else if (threadState == AnalyzerThreadState::Busy) {
                    m_analyzerThreadIdle = false;
                }
            });
    m_pAnalyzerThread->start();
}

void OpenSubsonicFeature::submitPendingStreamTrackAnalyses() {
    if (!m_analyzerThreadIdle || m_pendingAnalysisTracks.isEmpty()) {
        return;
    }
    const TrackPointer pTrack = m_pendingAnalysisTracks.constFirst();
    if (m_pAnalyzerThread->submitNextTrack(AnalyzerTrack(pTrack))) {
        m_pendingAnalysisTracks.removeFirst();
        m_analyzerThreadIdle = false;
        kLogger.debug() << "Analyzing streamed track" << pTrack->getLocation();
    }
}

void OpenSubsonicFeature::reloadSettings() {
    const QString serverUrl =
            readSetting(m_pConfig, kServerUrlConfigKey, "OPENSUBSONIC_URL");
    const QString userName =
            readSetting(m_pConfig, kUserNameConfigKey, "OPENSUBSONIC_USER");
    const QString password =
            readSetting(m_pConfig, kPasswordConfigKey, "OPENSUBSONIC_PASSWORD");
    m_pClient->configure(serverUrl, userName, password);
    kLogger.info() << "OpenSubsonic server:" << serverUrl
                   << "configured:" << m_pClient->isConfigured();

    // Forget everything that belongs to the previous server/account.
    m_loadedNodes.clear();
    m_pendingRequests.clear();
    m_songsById.clear();
    m_selectedAlbumId.clear();
    m_pTrackModel->clearSearch();
    m_pTrackModel->clearSongs();

    auto pRootItem = TreeItem::newRoot(this);
    if (m_pClient->isConfigured()) {
        TreeItem* pArtistsItem = pRootItem->appendChild(
                tr("Artists"), kArtistsNode);
        // A placeholder makes the expander arrow appear even before the
        // (asynchronous) request has finished.
        pArtistsItem->appendChild(tr("Loading…"));
    } else {
        kLogger.warning()
                << "OpenSubsonic is not configured."
                << "Set the server url and the credentials in"
                << "Preferences > Library > OpenSubsonic"
                << "or use the OPENSUBSONIC_URL/OPENSUBSONIC_USER/OPENSUBSONIC_PASSWORD"
                << "environment variables.";
        pRootItem->appendChild(
                tr("Not configured"),
                kInfoNode);
    }
    m_pSidebarModel->setRootItem(std::move(pRootItem));
}

QVariant OpenSubsonicFeature::title() {
    return QVariant(tr("OpenSubsonic"));
}

void OpenSubsonicFeature::activate() {
    emit showTrackModel(m_pTrackModel);
}

void OpenSubsonicFeature::activateChild(const QModelIndex& index) {
    TreeItem* pItem = static_cast<TreeItem*>(index.internalPointer());
    if (!pItem) {
        return;
    }
    const QString nodeId = pItem->getData().toString();

    if (nodeId == kArtistsNode || nodeId.startsWith(kArtistNodePrefix)) {
        loadChildren(index);
        return;
    }

    if (nodeId.startsWith(kAlbumNodePrefix)) {
        const QString albumId = nodeId.mid(kAlbumNodePrefix.size());
        if (albumId.isEmpty()) {
            return;
        }
        m_selectedAlbumId = albumId;
        m_pendingRequests.insert(nodeId, QPersistentModelIndex(index));
        m_pClient->fetchAlbumSongs(albumId);
        return;
    }
}

void OpenSubsonicFeature::onLazyChildExpandation(const QModelIndex& index) {
    loadChildren(index);
}

void OpenSubsonicFeature::loadChildren(const QModelIndex& index) {
    TreeItem* pItem = static_cast<TreeItem*>(index.internalPointer());
    if (!pItem) {
        return;
    }
    const QString nodeId = pItem->getData().toString();
    if (nodeId.isEmpty() || m_loadedNodes.contains(nodeId)) {
        return;
    }

    if (nodeId == kArtistsNode) {
        m_loadedNodes.insert(nodeId);
        m_pendingRequests.insert(nodeId, QPersistentModelIndex(index));
        m_pClient->fetchArtists();
    } else if (nodeId.startsWith(kArtistNodePrefix)) {
        const QString artistId = nodeId.mid(kArtistNodePrefix.size());
        if (artistId.isEmpty()) {
            return;
        }
        m_loadedNodes.insert(nodeId);
        m_pendingRequests.insert(nodeId, QPersistentModelIndex(index));
        m_pClient->fetchArtistAlbums(artistId);
    }
}

void OpenSubsonicFeature::replaceChildren(
        const QModelIndex& parentIndex,
        std::vector<std::unique_ptr<TreeItem>>&& children) {
    if (!parentIndex.isValid()) {
        kLogger.warning() << "Dropping result, sidebar node has vanished";
        return;
    }
    TreeItem* pParentItem = static_cast<TreeItem*>(parentIndex.internalPointer());
    if (!pParentItem) {
        return;
    }
    if (pParentItem->childRows() > 0) {
        m_pSidebarModel->removeRows(0, pParentItem->childRows(), parentIndex);
    }
    if (!children.empty()) {
        m_pSidebarModel->insertTreeItemRows(std::move(children), 0, parentIndex);
    }
}

void OpenSubsonicFeature::showInfoItem(
        const QModelIndex& parentIndex,
        const QString& message) {
    std::vector<std::unique_ptr<TreeItem>> items;
    items.push_back(std::make_unique<TreeItem>(message, kInfoNode));
    replaceChildren(parentIndex, std::move(items));
}

void OpenSubsonicFeature::slotArtistsFetched(
        const QList<mixxx::opensubsonic::Artist>& artists) {
    const QPersistentModelIndex parentIndex =
            m_pendingRequests.take(kArtistsNode);
    if (!parentIndex.isValid()) {
        return;
    }

    std::vector<std::unique_ptr<TreeItem>> items;
    items.reserve(static_cast<size_t>(artists.size()));
    for (const mixxx::opensubsonic::Artist& artist : artists) {
        auto pItem = std::make_unique<TreeItem>(
                artist.name,
                QString(kArtistNodePrefix + artist.id));
        pItem->appendChild(tr("…"));
        items.push_back(std::move(pItem));
    }
    if (items.empty()) {
        items.push_back(
                std::make_unique<TreeItem>(tr("No artists found"), kInfoNode));
    }
    replaceChildren(parentIndex, std::move(items));
}

void OpenSubsonicFeature::slotAlbumsFetched(
        const QString& artistId,
        const QList<mixxx::opensubsonic::Album>& albums) {
    const QString context = kArtistNodePrefix + artistId;
    const QPersistentModelIndex parentIndex = m_pendingRequests.take(context);
    if (!parentIndex.isValid()) {
        return;
    }

    std::vector<std::unique_ptr<TreeItem>> items;
    items.reserve(static_cast<size_t>(albums.size()));
    for (const mixxx::opensubsonic::Album& album : albums) {
        QString label = album.name;
        if (album.year > 0) {
            label = tr("%1 (%2)").arg(album.name, QString::number(album.year));
        }
        items.push_back(std::make_unique<TreeItem>(
                label,
                QString(kAlbumNodePrefix + album.id)));
    }
    if (items.empty()) {
        items.push_back(
                std::make_unique<TreeItem>(tr("No albums found"), kInfoNode));
    }
    replaceChildren(parentIndex, std::move(items));
}

void OpenSubsonicFeature::slotSongsFetched(
        const QString& albumId,
        const QList<mixxx::opensubsonic::Song>& songs) {
    m_pendingRequests.remove(kAlbumNodePrefix + albumId);
    if (albumId != m_selectedAlbumId) {
        // The user already selected a different album in the meantime.
        return;
    }
    m_pTrackModel->setAlbumSongs(songs);
    rememberSongs(songs);
    // Selecting an album leaves the search mode.
    emit restoreSearch(QString());
    emit showTrackModel(m_pTrackModel);
}

void OpenSubsonicFeature::rememberSongs(
        const QList<mixxx::opensubsonic::Song>& songs) {
    for (const mixxx::opensubsonic::Song& song : songs) {
        if (!song.id.isEmpty()) {
            m_songsById.insert(song.id, song);
        }
    }
}

void OpenSubsonicFeature::slotLoadRemoteUrlToPlayer(
        const QUrl& url,
        const QString& group,
        bool play) {
    const QUrlQuery query(url);
    const QString songId = query.queryItemValue(QStringLiteral("id"));
    if (songId.isEmpty()) {
        kLogger.warning() << "Ignoring remote URL without song id:" << url;
        return;
    }
    const auto songIt = m_songsById.constFind(songId);
    if (songIt == m_songsById.constEnd()) {
        kLogger.warning() << "Ignoring unknown remote song" << songId;
        return;
    }
    const mixxx::opensubsonic::Song& song = songIt.value();
    kLogger.info() << "Loading remote track" << song.artist << "-"
                   << song.title << "into" << group;

    // The audio pipeline opens the stream URL directly, there is no local copy
    // of the audio data involved.
    TrackPointer pStreamTrack = createStreamTrack(song, url);
    if (!pStreamTrack) {
        kLogger.warning() << "Failed to create a stream track for" << url;
        return;
    }
    emit loadTrackToPlayer(pStreamTrack,
            group,
#ifdef __STEM__
            mixxx::StemChannelSelection(),
#endif
            play);
}

void OpenSubsonicFeature::slotPlayerTrackChanged(
        const QString& group,
        const TrackPointer& pNewTrack,
        const TrackPointer& pOldTrack) {
    Q_UNUSED(group);
    Q_UNUSED(pOldTrack);
    if (pNewTrack && pNewTrack->getFileInfo().isRemote()) {
        // Streamed tracks bypass the library, so their waveform has to be
        // computed here instead of by the library analysis scheduler.
        scheduleStreamTrackAnalysis(pNewTrack);
    }
}

void OpenSubsonicFeature::slotRequestFailed(
        const QString& context,
        const QString& errorMessage) {
    kLogger.warning() << "Request" << context << "failed:" << errorMessage;

    // Allow the user to retry by expanding the node again.
    m_loadedNodes.remove(context);

    const QPersistentModelIndex parentIndex = m_pendingRequests.take(context);
    if (parentIndex.isValid()) {
        showInfoItem(parentIndex, tr("Error: %1").arg(errorMessage));
    }
}

#include "moc_opensubsonicfeature.cpp"
