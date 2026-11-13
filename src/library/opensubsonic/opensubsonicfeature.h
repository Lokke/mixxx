#pragma once

#include <QHash>
#include <QList>
#include <QPersistentModelIndex>
#include <QSet>
#include <QString>
#include <QUrl>
#include <QVariant>
#include <memory>
#include <vector>

#include "library/libraryfeature.h"
#include "library/navidrome/navidrometypes.h"
#include "library/treeitemmodel.h"
#include "util/parented_ptr.h"

class NavidromeTrackModel;
class QModelIndex;

namespace mixxx {
namespace navidrome {
class NavidromeDownloader;
class SubsonicClient;
} // namespace navidrome
} // namespace mixxx

/// Library feature that exposes the music library of a Navidrome server
/// (Subsonic / OpenSubsonic API) in the Mixxx library sidebar.
///
/// This is a read-only browser for now. Neither audio data nor metadata are
/// imported into the Mixxx database, so nothing has to be cleaned up when the
/// remote library changes.
class NavidromeFeature : public LibraryFeature {
    Q_OBJECT

  public:
    NavidromeFeature(Library* pLibrary, UserSettingsPointer pConfig);
    ~NavidromeFeature() override = default;

    QVariant title() override;
    TreeItemModel* sidebarModel() const override {
        return m_pSidebarModel.get();
    }
    bool hasTrackTable() override {
        return true;
    }

  public slots:
    void activate() override;
    void activateChild(const QModelIndex& index) override;
    void onLazyChildExpandation(const QModelIndex& index) override;
    /// Re-reads server url and credentials from the preferences and rebuilds
    /// the sidebar tree. Called at startup and whenever the settings changed.
    void reloadSettings();

  private slots:
    void slotArtistsFetched(const QList<mixxx::navidrome::Artist>& artists);
    void slotAlbumsFetched(const QString& artistId,
            const QList<mixxx::navidrome::Album>& albums);
    void slotSongsFetched(const QString& albumId,
            const QList<mixxx::navidrome::Song>& songs);
    void slotRequestFailed(const QString& context, const QString& errorMessage);
    /// A remote stream URL (e.g. dropped onto a deck) should be played.
    void slotLoadRemoteUrlToPlayer(const QUrl& url,
            const QString& group,
            bool play);
    void slotSongDownloaded(const QString& songId, const QString& localPath);
    void slotDownloadFailed(const QString& songId, const QString& errorMessage);

  private:
    struct PendingLoad {
        QString group;
        bool play = false;
    };

    void rememberSongs(const QList<mixxx::navidrome::Song>& songs);
    void loadChildren(const QModelIndex& index);
    void replaceChildren(const QModelIndex& parentIndex,
            std::vector<std::unique_ptr<TreeItem>>&& children);
    void showInfoItem(const QModelIndex& parentIndex, const QString& message);

    parented_ptr<TreeItemModel> m_pSidebarModel;
    mixxx::navidrome::SubsonicClient* const m_pClient;
    mixxx::navidrome::NavidromeDownloader* m_pDownloader;
    NavidromeTrackModel* m_pTrackModel;

    /// Node identifiers ("artists", "artist:<id>") that have already been
    /// populated, so that expanding a node repeatedly does not trigger
    /// redundant network requests.
    QSet<QString> m_loadedNodes;
    /// Maps a pending request context to the sidebar node that is going to
    /// receive the result. Asynchronous replies may arrive in any order.
    QHash<QString, QPersistentModelIndex> m_pendingRequests;

    QString m_selectedAlbumId;

    /// Metadata of all songs that have been seen so far, keyed by Subsonic id.
    /// Used to resolve a stream URL back to its song when it is dropped onto
    /// a player.
    QHash<QString, mixxx::navidrome::Song> m_songsById;
    /// Player loads that are waiting for the download to finish.
    QHash<QString, PendingLoad> m_pendingLoads;
};
