#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QVector>

#include "library/basetracktablemodel.h"
#include "library/opensubsonic/opensubsonictypes.h"

namespace mixxx {
namespace opensubsonic {
class OpenSubsonicDownloader;
class SubsonicClient;
} // namespace opensubsonic
} // namespace mixxx

/// Read-only track table model for songs of a remote OpenSubsonic server.
///
/// The model does not import anything into the Mixxx database. It creates
/// temporary Track objects on demand, which is exactly what
/// BaseTrackTableModel has been designed for.
class OpenSubsonicTrackModel : public BaseTrackTableModel {
    Q_OBJECT

  public:
    OpenSubsonicTrackModel(QObject* parent,
            TrackCollectionManager* pTrackCollectionManager,
            const mixxx::opensubsonic::SubsonicClient* pClient,
            mixxx::opensubsonic::OpenSubsonicDownloader* pDownloader);
    ~OpenSubsonicTrackModel() override = default;

    /// Songs of the selected album. Also leaves search mode.
    void setAlbumSongs(QList<mixxx::opensubsonic::Song> songs);
    /// Shows the result of a server side search. Ignored if the query does
    /// not match the current search text anymore (outdated reply).
    void setSearchResults(const QString& query,
            const QList<mixxx::opensubsonic::Song>& songs);
    /// Leaves search mode and restores the songs of the selected album.
    void clearSearch();
    void clearSongs();

  signals:
    /// Emitted when a non-empty search text is set. The owner is expected to
    /// query the server and to feed the result back via setSearchResults().
    void searchRequested(const QString& searchText);
    /// Emitted when the user changed the rating of a song in the table. The
    /// owner is expected to push the new value to the server (setRating).
    void ratingChangeRequested(const QString& songId, int rating);

  public:
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;

    // TrackModel
    TrackPointer getTrack(const QModelIndex& index) const override;
    QUrl getTrackUrl(const QModelIndex& index) const override;
    QString getTrackLocation(const QModelIndex& index) const override;
    TrackId getTrackId(const QModelIndex& index) const override;
    CoverInfo getCoverInfo(const QModelIndex& index) const override;
    const QVector<int> getTrackRows(TrackId trackId) const override;
    void search(const QString& searchText) override;
    const QString currentSearch() const override;
    bool isColumnInternal(int column) override;
    TrackModel::SortColumnId sortColumnIdFromColumnIndex(int index) const override;
    int columnIndexFromSortColumnId(TrackModel::SortColumnId sortColumn) const override;
    QString modelKey(bool noSearch) const override;
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;
    TrackModel::Capabilities getCapabilities() const override;

  protected:
    QVariant rawValue(const QModelIndex& index) const override;
    Qt::ItemFlags readWriteFlags(const QModelIndex& index) const override;
    bool setTrackValueForColumn(const TrackPointer& pTrack,
            int column,
            const QVariant& value,
            int role) override;

  private:
    static QVariant fieldValue(
            const mixxx::opensubsonic::Song& song,
            ColumnCache::Column column);
    bool updateSongRating(const QString& songId, int rating);
    void rebuildVisibleSongs();

    const mixxx::opensubsonic::SubsonicClient* const m_pClient;
    mixxx::opensubsonic::OpenSubsonicDownloader* const m_pDownloader;

    /// Songs of the currently selected album.
    QList<mixxx::opensubsonic::Song> m_albumSongs;
    /// Songs that are currently displayed (album songs or search results).
    QList<mixxx::opensubsonic::Song> m_allSongs;
    QList<mixxx::opensubsonic::Song> m_visibleSongs;
    mutable QHash<int, TrackPointer> m_trackCache;

    QString m_searchText;
    int m_sortColumn;
    Qt::SortOrder m_sortOrder;

    QHash<int, TrackModel::SortColumnId> m_sortColumnIdByColumnIndex;
    QHash<int, int> m_columnIndexBySortColumnId;
};
