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

#include "analyzer/analyzerthread.h"
#include "library/libraryfeature.h"
#include "library/opensubsonic/opensubsonictypes.h"
#include "library/treeitemmodel.h"
#include "track/track_decl.h"
#include "util/db/dbconnectionpool.h"
#include "util/parented_ptr.h"
#include "waveform/waveform.h"

class OpenSubsonicTrackModel;
class QModelIndex;

namespace mixxx {
namespace opensubsonic {
class SubsonicClient;
} // namespace opensubsonic
} // namespace mixxx

/// Library feature that exposes the music library of an OpenSubsonic server
/// (Subsonic / OpenSubsonic API) in the library sidebar.
///
/// Nothing is imported into the local collection, so nothing has to be cleaned
/// up when the remote library changes. The only write access is the rating of
/// a song, which is stored on the server.
///
/// Songs are played as network streams, no audio data is copied to disk: the
/// players decode the stream URL directly and the waveform analysis reads the
/// same stream. Nothing survives the application run because nothing is
/// written.
class OpenSubsonicFeature : public LibraryFeature {
    Q_OBJECT

  public:
    OpenSubsonicFeature(Library* pLibrary,
            UserSettingsPointer pConfig,
            mixxx::DbConnectionPoolPtr pDbConnectionPool);
    ~OpenSubsonicFeature() override;

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
    void slotArtistsFetched(const QList<mixxx::opensubsonic::Artist>& artists);
    void slotAlbumsFetched(const QString& artistId,
            const QList<mixxx::opensubsonic::Album>& albums);
    void slotSongsFetched(const QString& albumId,
            const QList<mixxx::opensubsonic::Song>& songs);
    void slotRequestFailed(const QString& context, const QString& errorMessage);
    /// A remote stream URL (e.g. dropped onto a deck) should be played.
    void slotLoadRemoteUrlToPlayer(const QUrl& url,
            const QString& group,
            bool play);
    /// Hands a track that has been loaded into a player to the analyzer, so
    /// that its waveform is computed even though it is not in the library.
    void slotPlayerTrackChanged(const QString& group,
            const TrackPointer& pNewTrack,
            const TrackPointer& pOldTrack);

  private:
    void rememberSongs(const QList<mixxx::opensubsonic::Song>& songs);
    /// Streamed tracks are not part of the local library, so Mixxx never
    /// analyzes them and their waveform would stay empty. Once such a track
    /// has been loaded into a player it is handed to a dedicated analyzer
    /// thread that computes the waveform (and beat grid, key, ...) in the
    /// background.
    void scheduleStreamTrackAnalysis(const TrackPointer& pTrack);
    void submitPendingStreamTrackAnalyses();
    void startAnalyzerThread();
    void loadChildren(const QModelIndex& index);
    void replaceChildren(const QModelIndex& parentIndex,
            std::vector<std::unique_ptr<TreeItem>>&& children);
    void showInfoItem(const QModelIndex& parentIndex, const QString& message);

    parented_ptr<TreeItemModel> m_pSidebarModel;
    mixxx::opensubsonic::SubsonicClient* const m_pClient;
    OpenSubsonicTrackModel* m_pTrackModel;

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
    QHash<QString, mixxx::opensubsonic::Song> m_songsById;

    /// Analyzer thread for streamed tracks, created on demand.
    mixxx::DbConnectionPoolPtr m_pDbConnectionPool;
    AnalyzerThread::Pointer m_pAnalyzerThread;
    bool m_analyzerThreadIdle = false;
    QList<TrackPointer> m_pendingAnalysisTracks;

    /// Waveforms that have already been computed for a song, keyed by the
    /// song id. Streamed tracks are new objects on every load, so this avoids
    /// downloading and analyzing the same song over and over again.
    QHash<QString, ConstWaveformPointer> m_waveformsBySongId;
    QHash<QString, ConstWaveformPointer> m_waveformSummariesBySongId;
};
