#include "library/navidrome/navidromefeature.h"

#include <QModelIndex>
#include <QUrlQuery>

#include "library/library.h"
#include "library/navidrome/navidromecache.h"
#include "library/navidrome/navidromedownloader.h"
#include "library/navidrome/navidrometrackmodel.h"
#include "library/navidrome/subsonicclient.h"
#include "library/treeitem.h"
#include "preferences/configobject.h"
#include "util/logger.h"

namespace {

const mixxx::Logger kLogger("NavidromeFeature");

const ConfigKey kServerUrlConfigKey("[Navidrome]", "server_url");
const ConfigKey kUserNameConfigKey("[Navidrome]", "username");
const ConfigKey kPasswordConfigKey("[Navidrome]", "password");

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
        // Convenience for development builds without a settings dialog.
        value = qEnvironmentVariable(pEnvironmentVariableName);
    }
    return value;
}

} // namespace

NavidromeFeature::NavidromeFeature(
        Library* pLibrary,
        UserSettingsPointer pConfig)
        : LibraryFeature(pLibrary, pConfig, QStringLiteral("tracks")),
          m_pSidebarModel(make_parented<TreeItemModel>(this)),
          m_pClient(new mixxx::navidrome::SubsonicClient(this)),
          m_pDownloader(new mixxx::navidrome::NavidromeDownloader(m_pClient, this)),
          m_pTrackModel(new NavidromeTrackModel(
                  this, pLibrary->trackCollectionManager(), m_pClient, m_pDownloader)) {
    connect(m_pClient,
            &mixxx::navidrome::SubsonicClient::artistsFetched,
            this,
            &NavidromeFeature::slotArtistsFetched);
    connect(m_pClient,
            &mixxx::navidrome::SubsonicClient::albumsFetched,
            this,
            &NavidromeFeature::slotAlbumsFetched);
    connect(m_pClient,
            &mixxx::navidrome::SubsonicClient::songsFetched,
            this,
            &NavidromeFeature::slotSongsFetched);
    connect(m_pClient,
            &mixxx::navidrome::SubsonicClient::requestFailed,
            this,
            &NavidromeFeature::slotRequestFailed);

    // The Mixxx search box calls search() on the currently shown track model.
    // Forward that to a server side search and feed the reply back.
    connect(m_pTrackModel,
            &NavidromeTrackModel::searchRequested,
            m_pClient,
            &mixxx::navidrome::SubsonicClient::searchSongs);
    connect(m_pClient,
            &mixxx::navidrome::SubsonicClient::searchResultsFetched,
            m_pTrackModel,
            &NavidromeTrackModel::setSearchResults);
    connect(m_pClient,
            &mixxx::navidrome::SubsonicClient::searchResultsFetched,
            this,
            [this](const QString&, const QList<mixxx::navidrome::Song>& songs) {
                rememberSongs(songs);
            });

    // Ratings edited in the track table are stored on the server (setRating).
    connect(m_pTrackModel,
            &NavidromeTrackModel::ratingChangeRequested,
            m_pClient,
            &mixxx::navidrome::SubsonicClient::setRating);

    // A remote stream URL (dropped onto a deck) is resolved here: the audio
    // data is downloaded and the resulting local file is handed to the player.
    connect(m_pLibrary,
            &Library::loadRemoteUrlToPlayer,
            this,
            &NavidromeFeature::slotLoadRemoteUrlToPlayer);
    connect(m_pDownloader,
            &mixxx::navidrome::NavidromeDownloader::songDownloaded,
            this,
            &NavidromeFeature::slotSongDownloaded);
    connect(m_pDownloader,
            &mixxx::navidrome::NavidromeDownloader::downloadFailed,
            this,
            &NavidromeFeature::slotDownloadFailed);

    reloadSettings();
}

void NavidromeFeature::reloadSettings() {
    const QString serverUrl =
            readSetting(m_pConfig, kServerUrlConfigKey, "NAVIDROME_URL");
    const QString userName =
            readSetting(m_pConfig, kUserNameConfigKey, "NAVIDROME_USER");
    const QString password =
            readSetting(m_pConfig, kPasswordConfigKey, "NAVIDROME_PASSWORD");
    m_pClient->configure(serverUrl, userName, password);
    kLogger.info() << "Navidrome server:" << serverUrl
                   << "configured:" << m_pClient->isConfigured();

    // Forget everything that belongs to the previous server/account.
    m_loadedNodes.clear();
    m_pendingRequests.clear();
    m_pendingLoads.clear();
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
                << "Navidrome is not configured."
                << "Set the server url and the credentials in"
                << "Preferences > Library > Navidrome"
                << "or use the NAVIDROME_URL/NAVIDROME_USER/NAVIDROME_PASSWORD"
                << "environment variables.";
        pRootItem->appendChild(
                tr("Not configured"),
                kInfoNode);
    }
    m_pSidebarModel->setRootItem(std::move(pRootItem));
}

QVariant NavidromeFeature::title() {
    return QVariant(tr("Navidrome"));
}

void NavidromeFeature::activate() {
    emit showTrackModel(m_pTrackModel);
}

void NavidromeFeature::activateChild(const QModelIndex& index) {
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

void NavidromeFeature::onLazyChildExpandation(const QModelIndex& index) {
    loadChildren(index);
}

void NavidromeFeature::loadChildren(const QModelIndex& index) {
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

void NavidromeFeature::replaceChildren(
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

void NavidromeFeature::showInfoItem(
        const QModelIndex& parentIndex,
        const QString& message) {
    std::vector<std::unique_ptr<TreeItem>> items;
    items.push_back(std::make_unique<TreeItem>(message, kInfoNode));
    replaceChildren(parentIndex, std::move(items));
}

void NavidromeFeature::slotArtistsFetched(
        const QList<mixxx::navidrome::Artist>& artists) {
    const QPersistentModelIndex parentIndex =
            m_pendingRequests.take(kArtistsNode);
    if (!parentIndex.isValid()) {
        return;
    }

    std::vector<std::unique_ptr<TreeItem>> items;
    items.reserve(static_cast<size_t>(artists.size()));
    for (const mixxx::navidrome::Artist& artist : artists) {
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

void NavidromeFeature::slotAlbumsFetched(
        const QString& artistId,
        const QList<mixxx::navidrome::Album>& albums) {
    const QString context = kArtistNodePrefix + artistId;
    const QPersistentModelIndex parentIndex = m_pendingRequests.take(context);
    if (!parentIndex.isValid()) {
        return;
    }

    std::vector<std::unique_ptr<TreeItem>> items;
    items.reserve(static_cast<size_t>(albums.size()));
    for (const mixxx::navidrome::Album& album : albums) {
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

void NavidromeFeature::slotSongsFetched(
        const QString& albumId,
        const QList<mixxx::navidrome::Song>& songs) {
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

void NavidromeFeature::rememberSongs(
        const QList<mixxx::navidrome::Song>& songs) {
    for (const mixxx::navidrome::Song& song : songs) {
        if (!song.id.isEmpty()) {
            m_songsById.insert(song.id, song);
        }
    }
}

void NavidromeFeature::slotLoadRemoteUrlToPlayer(
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
    const mixxx::navidrome::Song& song = songIt.value();
    kLogger.info() << "Loading remote track" << song.artist << "-"
                   << song.title << "into" << group;

    const QString localPath = mixxx::navidrome::cachedSongFilePath(song);
    if (mixxx::navidrome::NavidromeDownloader::isCached(song)) {
        m_pLibrary->slotLoadLocationToPlayer(localPath, group, play);
        return;
    }

    PendingLoad pendingLoad;
    pendingLoad.group = group;
    pendingLoad.play = play;
    m_pendingLoads.insert(songId, pendingLoad);
    m_pDownloader->enqueue(song);
}

void NavidromeFeature::slotSongDownloaded(
        const QString& songId,
        const QString& localPath) {
    const auto pendingIt = m_pendingLoads.find(songId);
    if (pendingIt == m_pendingLoads.end()) {
        return;
    }
    const PendingLoad pendingLoad = pendingIt.value();
    m_pendingLoads.erase(pendingIt);
    m_pLibrary->slotLoadLocationToPlayer(
            localPath, pendingLoad.group, pendingLoad.play);
}

void NavidromeFeature::slotDownloadFailed(
        const QString& songId,
        const QString& errorMessage) {
    if (m_pendingLoads.remove(songId) > 0) {
        kLogger.warning() << "Failed to load remote track" << songId
                          << errorMessage;
    }
}

void NavidromeFeature::slotRequestFailed(
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

#include "moc_navidromefeature.cpp"
