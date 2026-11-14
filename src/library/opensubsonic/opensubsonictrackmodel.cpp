#include "library/opensubsonic/opensubsonictrackmodel.h"

#include <QFileInfo>
#include <QStringList>
#include <QUrl>
#include <algorithm>
#include <utility>

#include "library/dao/trackschema.h"
#include "library/opensubsonic/subsonicclient.h"
#include "library/starrating.h"
#include "track/bpm.h"
#include "track/track.h"
#include "util/fileaccess.h"
#include "util/fileinfo.h"
#include "util/logger.h"

namespace {

using mixxx::opensubsonic::Song;

/// All columns that this model provides. The names must match the column
/// names used by the Mixxx library table so that the generic column
/// machinery of BaseTrackTableModel can be reused.
QStringList opensubsonicTableColumns() {
    return {
            LIBRARYTABLE_ID,
            LIBRARYTABLE_ARTIST,
            LIBRARYTABLE_TITLE,
            LIBRARYTABLE_ALBUM,
            LIBRARYTABLE_ALBUMARTIST,
            LIBRARYTABLE_YEAR,
            LIBRARYTABLE_GENRE,
            LIBRARYTABLE_COMPOSER,
            LIBRARYTABLE_GROUPING,
            LIBRARYTABLE_TRACKNUMBER,
            LIBRARYTABLE_FILETYPE,
            LIBRARYTABLE_COMMENT,
            LIBRARYTABLE_DURATION,
            LIBRARYTABLE_BITRATE,
            LIBRARYTABLE_BPM,
            LIBRARYTABLE_REPLAYGAIN,
            LIBRARYTABLE_URL,
            LIBRARYTABLE_CHANNELS,
            LIBRARYTABLE_DATETIMEADDED,
            LIBRARYTABLE_TIMESPLAYED,
            LIBRARYTABLE_PLAYED,
            LIBRARYTABLE_RATING,
            LIBRARYTABLE_KEY,
            LIBRARYTABLE_KEY_ID,
            LIBRARYTABLE_BPM_LOCK,
            LIBRARYTABLE_BEATS_VERSION,
            LIBRARYTABLE_PREVIEW,
            LIBRARYTABLE_COLOR,
            LIBRARYTABLE_COVERART,
            LIBRARYTABLE_COVERART_SOURCE,
            LIBRARYTABLE_COVERART_TYPE,
            LIBRARYTABLE_COVERART_LOCATION,
            LIBRARYTABLE_COVERART_COLOR,
            LIBRARYTABLE_COVERART_DIGEST,
            LIBRARYTABLE_COVERART_HASH,
            LIBRARYTABLE_LAST_PLAYED_AT,
            TRACKLOCATIONSTABLE_LOCATION,
            TRACKLOCATIONSTABLE_DIRECTORY,
            TRACKLOCATIONSTABLE_FSDELETED,
    };
}

const QHash<ColumnCache::Column, TrackModel::SortColumnId>& sortColumnIdMap() {
    static const QHash<ColumnCache::Column, TrackModel::SortColumnId> kMap = {
            {ColumnCache::COLUMN_LIBRARYTABLE_ARTIST,
                    TrackModel::SortColumnId::Artist},
            {ColumnCache::COLUMN_LIBRARYTABLE_TITLE,
                    TrackModel::SortColumnId::Title},
            {ColumnCache::COLUMN_LIBRARYTABLE_ALBUM,
                    TrackModel::SortColumnId::Album},
            {ColumnCache::COLUMN_LIBRARYTABLE_ALBUMARTIST,
                    TrackModel::SortColumnId::AlbumArtist},
            {ColumnCache::COLUMN_LIBRARYTABLE_YEAR,
                    TrackModel::SortColumnId::Year},
            {ColumnCache::COLUMN_LIBRARYTABLE_GENRE,
                    TrackModel::SortColumnId::Genre},
            {ColumnCache::COLUMN_LIBRARYTABLE_COMPOSER,
                    TrackModel::SortColumnId::Composer},
            {ColumnCache::COLUMN_LIBRARYTABLE_GROUPING,
                    TrackModel::SortColumnId::Grouping},
            {ColumnCache::COLUMN_LIBRARYTABLE_TRACKNUMBER,
                    TrackModel::SortColumnId::TrackNumber},
            {ColumnCache::COLUMN_LIBRARYTABLE_FILETYPE,
                    TrackModel::SortColumnId::FileType},
            {ColumnCache::COLUMN_LIBRARYTABLE_COMMENT,
                    TrackModel::SortColumnId::Comment},
            {ColumnCache::COLUMN_LIBRARYTABLE_DURATION,
                    TrackModel::SortColumnId::Duration},
            {ColumnCache::COLUMN_LIBRARYTABLE_BITRATE,
                    TrackModel::SortColumnId::BitRate},
            {ColumnCache::COLUMN_LIBRARYTABLE_BPM,
                    TrackModel::SortColumnId::Bpm},
            {ColumnCache::COLUMN_LIBRARYTABLE_DATETIMEADDED,
                    TrackModel::SortColumnId::DateTimeAdded},
            {ColumnCache::COLUMN_LIBRARYTABLE_KEY,
                    TrackModel::SortColumnId::Key},
            {ColumnCache::COLUMN_TRACKLOCATIONSTABLE_LOCATION,
                    TrackModel::SortColumnId::NativeLocation},
    };
    return kMap;
}

} // namespace

OpenSubsonicTrackModel::OpenSubsonicTrackModel(
        QObject* parent,
        TrackCollectionManager* pTrackCollectionManager,
        const mixxx::opensubsonic::SubsonicClient* pClient)
        : BaseTrackTableModel(parent,
                  pTrackCollectionManager,
                  "mixxx.db.model.opensubsonic"),
          m_pClient(pClient),
          m_sortColumn(-1),
          m_sortOrder(Qt::AscendingOrder) {
    initTableColumnsAndHeaderProperties(opensubsonicTableColumns());

    for (auto it = sortColumnIdMap().cbegin(); it != sortColumnIdMap().cend(); ++it) {
        const int columnIndex = fieldIndex(it.key());
        if (columnIndex < 0) {
            continue;
        }
        m_sortColumnIdByColumnIndex.insert(columnIndex, it.value());
        m_columnIndexBySortColumnId.insert(static_cast<int>(it.value()),
                columnIndex);
    }
}

void OpenSubsonicTrackModel::setAlbumSongs(QList<Song> songs) {
    beginResetModel();
    m_albumSongs = std::move(songs);
    m_allSongs = m_albumSongs;
    m_searchText.clear();
    m_trackCache.clear();
    rebuildVisibleSongs();
    endResetModel();
}

void OpenSubsonicTrackModel::setSearchResults(
        const QString& query,
        const QList<Song>& songs) {
    if (query != m_searchText) {
        // The user continued typing, this reply is outdated.
        return;
    }
    beginResetModel();
    m_allSongs = songs;
    m_trackCache.clear();
    rebuildVisibleSongs();
    endResetModel();
}

void OpenSubsonicTrackModel::clearSearch() {
    if (m_searchText.isEmpty()) {
        return;
    }
    beginResetModel();
    m_searchText.clear();
    m_allSongs = m_albumSongs;
    m_trackCache.clear();
    rebuildVisibleSongs();
    endResetModel();
}

void OpenSubsonicTrackModel::clearSongs() {
    beginResetModel();
    m_albumSongs.clear();
    m_allSongs.clear();
    m_searchText.clear();
    m_trackCache.clear();
    rebuildVisibleSongs();
    endResetModel();
}

int OpenSubsonicTrackModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(m_visibleSongs.size());
}

void OpenSubsonicTrackModel::rebuildVisibleSongs() {
    m_visibleSongs = m_allSongs;

    if (m_sortColumn < 0) {
        return;
    }
    const auto sortColumn =
            static_cast<ColumnCache::Column>(mapColumn(m_sortColumn));
    if (sortColumn == ColumnCache::COLUMN_LIBRARYTABLE_INVALID) {
        return;
    }

    const Qt::SortOrder sortOrder = m_sortOrder;
    std::stable_sort(m_visibleSongs.begin(),
            m_visibleSongs.end(),
            [this, sortColumn, sortOrder](const Song& lhs, const Song& rhs) {
                const QVariant lhsValue = fieldValue(lhs, sortColumn);
                const QVariant rhsValue = fieldValue(rhs, sortColumn);

                bool lhsOk = false;
                bool rhsOk = false;
                const double lhsNumber = lhsValue.toDouble(&lhsOk);
                const double rhsNumber = rhsValue.toDouble(&rhsOk);
                int result;
                if (lhsOk && rhsOk && lhsValue.isValid() && rhsValue.isValid()) {
                    result = lhsNumber < rhsNumber ? -1 : (lhsNumber > rhsNumber ? 1 : 0);
                } else {
                    result = QString::compare(lhsValue.toString(),
                            rhsValue.toString(),
                            Qt::CaseInsensitive);
                }
                return sortOrder == Qt::AscendingOrder ? result < 0 : result > 0;
            });
}

void OpenSubsonicTrackModel::sort(int column, Qt::SortOrder order) {
    if (column < 0 || column >= columnCount()) {
        return;
    }
    beginResetModel();
    m_sortColumn = column;
    m_sortOrder = order;
    rebuildVisibleSongs();
    endResetModel();
}

QVariant OpenSubsonicTrackModel::fieldValue(
        const Song& song,
        ColumnCache::Column column) const {
    switch (column) {
    case ColumnCache::COLUMN_LIBRARYTABLE_ARTIST:
        return song.artist;
    case ColumnCache::COLUMN_LIBRARYTABLE_TITLE:
        return song.title;
    case ColumnCache::COLUMN_LIBRARYTABLE_ALBUM:
        return song.album;
    case ColumnCache::COLUMN_LIBRARYTABLE_ALBUMARTIST:
        return song.albumArtist;
    case ColumnCache::COLUMN_LIBRARYTABLE_YEAR:
        return song.year > 0 ? QVariant(QString::number(song.year)) : QVariant();
    case ColumnCache::COLUMN_LIBRARYTABLE_GENRE:
        return song.genre;
    case ColumnCache::COLUMN_LIBRARYTABLE_TRACKNUMBER:
        return song.trackNumber > 0
                ? QVariant(QString::number(song.trackNumber))
                : QVariant();
    case ColumnCache::COLUMN_LIBRARYTABLE_FILETYPE:
        return song.suffix.toLower();
    case ColumnCache::COLUMN_LIBRARYTABLE_COMMENT:
        return song.comment;
    case ColumnCache::COLUMN_LIBRARYTABLE_DURATION:
        return song.duration;
    case ColumnCache::COLUMN_LIBRARYTABLE_BITRATE:
        return song.bitRate;
    case ColumnCache::COLUMN_LIBRARYTABLE_BPM:
        return song.bpm > 0.0 ? QVariant(song.bpm) : QVariant();
    case ColumnCache::COLUMN_LIBRARYTABLE_DATETIMEADDED:
        // The column formatter requires a valid timestamp.
        return song.created.isValid() ? QVariant(song.created) : QVariant();
    case ColumnCache::COLUMN_LIBRARYTABLE_TIMESPLAYED:
        return 0;
    case ColumnCache::COLUMN_LIBRARYTABLE_PLAYED:
        return false;
    case ColumnCache::COLUMN_LIBRARYTABLE_RATING:
        return song.userRating;
    case ColumnCache::COLUMN_LIBRARYTABLE_BPM_LOCK:
        return false;
    case ColumnCache::COLUMN_LIBRARYTABLE_PREVIEW:
        return false;
    case ColumnCache::COLUMN_TRACKLOCATIONSTABLE_LOCATION:
        // There is no local file, the stream URL is the closest match.
        return m_pClient ? m_pClient->downloadUrl(song).toString() : QString();
    case ColumnCache::COLUMN_TRACKLOCATIONSTABLE_DIRECTORY:
        return QString();
    case ColumnCache::COLUMN_TRACKLOCATIONSTABLE_FSDELETED:
        return false;
    default:
        return {};
    }
}

QVariant OpenSubsonicTrackModel::rawValue(const QModelIndex& index) const {
    if (!index.isValid() || index.row() < 0 ||
            index.row() >= m_visibleSongs.size()) {
        return {};
    }
    return fieldValue(m_visibleSongs.at(index.row()), mapColumn(index.column()));
}

TrackPointer OpenSubsonicTrackModel::getTrack(const QModelIndex& index) const {
    if (!index.isValid() || index.row() < 0 ||
            index.row() >= m_visibleSongs.size()) {
        return {};
    }
    const int row = index.row();
    const auto cachedTrack = m_trackCache.constFind(row);
    if (cachedTrack != m_trackCache.constEnd()) {
        return cachedTrack.value();
    }

    const Song& song = m_visibleSongs.at(row);

    // No download, no temporary file: the track references the stream URL and
    // the audio pipeline decodes it directly from the server.
    if (!m_pClient) {
        return {};
    }
    auto pTrack = Track::newTemporary(mixxx::FileAccess(
            mixxx::FileInfo::fromRemoteUrl(m_pClient->downloadUrl(song))));
    pTrack->setTitle(song.title);
    pTrack->setArtist(song.artist);
    pTrack->setAlbum(song.album);
    pTrack->setAlbumArtist(song.albumArtist);
    if (!song.genre.isEmpty()) {
        pTrack->updateGenre(song.genre);
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

    m_trackCache.insert(row, pTrack);
    return pTrack;
}

QUrl OpenSubsonicTrackModel::getTrackUrl(const QModelIndex& index) const {
    if (!m_pClient || !index.isValid() || index.row() < 0 ||
            index.row() >= m_visibleSongs.size()) {
        return {};
    }
    return m_pClient->downloadUrl(m_visibleSongs.at(index.row()));
}

TrackModel::Capabilities OpenSubsonicTrackModel::getCapabilities() const {
    return Capability::LoadToDeck |
            Capability::LoadToSampler |
            Capability::LoadToPreviewDeck |
            Capability::Sorting;
}

Qt::ItemFlags OpenSubsonicTrackModel::readWriteFlags(
        const QModelIndex& index) const {
    if (!index.isValid()) {
        return Qt::ItemIsEnabled;
    }
    Qt::ItemFlags flags =
            QAbstractItemModel::flags(index) | Qt::ItemIsDragEnabled;
    // Ratings are the only editable column. The new value is written back to
    // the server through the ratingChangeRequested() signal.
    if (index.column() ==
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_RATING)) {
        flags |= Qt::ItemIsEditable;
    }
    return flags;
}

bool OpenSubsonicTrackModel::setTrackValueForColumn(
        const TrackPointer& pTrack,
        int column,
        const QVariant& value,
        int role) {
    if (role != Qt::EditRole) {
        return false;
    }
    if (column != fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_RATING)) {
        // Everything else would have to be written to the file tags of the
        // remote server, which is not supported.
        return false;
    }
    const int rating = value.value<StarRating>().starCount();
    if (rating < 0 || rating > 5) {
        return false;
    }

    // Find the row that owns this track to get hold of the song id.
    int row = -1;
    for (auto it = m_trackCache.constBegin();
            it != m_trackCache.constEnd();
            ++it) {
        if (it.value().get() == pTrack.get()) {
            row = it.key();
            break;
        }
    }
    if (row < 0 || row >= m_visibleSongs.size()) {
        return false;
    }
    const QString songId = m_visibleSongs.at(row).id;

    pTrack->setRating(rating);
    updateSongRating(songId, rating);

    const QModelIndex changedIndex = index(row, column);
    emit dataChanged(changedIndex,
            changedIndex,
            {Qt::DisplayRole, Qt::EditRole});
    emit ratingChangeRequested(songId, rating);
    return true;
}

bool OpenSubsonicTrackModel::updateSongRating(
        const QString& songId, int rating) {
    bool updated = false;
    const auto updateSongs = [&](QList<mixxx::opensubsonic::Song>* pSongs) {
        for (auto& song : *pSongs) {
            if (song.id == songId) {
                song.userRating = rating;
                updated = true;
            }
        }
    };
    updateSongs(&m_albumSongs);
    updateSongs(&m_allSongs);
    updateSongs(&m_visibleSongs);
    return updated;
}

QString OpenSubsonicTrackModel::getTrackLocation(const QModelIndex& index) const {
    if (!index.isValid() || index.row() < 0 ||
            index.row() >= m_visibleSongs.size()) {
        return {};
    }
    const Song& song = m_visibleSongs.at(index.row());
    // The stream URL travels through the drag & drop pipeline into
    // Library::slotLoadLocationToPlayer(), which forwards it to the feature
    // that turns it into a playable track.
    if (m_pClient) {
        return m_pClient->downloadUrl(song).toString();
    }
    return {};
}

TrackId OpenSubsonicTrackModel::getTrackId(const QModelIndex& index) const {
    Q_UNUSED(index);
    // Remote tracks are not part of the internal collection (yet).
    return TrackId();
}

CoverInfo OpenSubsonicTrackModel::getCoverInfo(const QModelIndex& index) const {
    Q_UNUSED(index);
    return CoverInfo();
}

const QVector<int> OpenSubsonicTrackModel::getTrackRows(TrackId trackId) const {
    Q_UNUSED(trackId);
    return {};
}

void OpenSubsonicTrackModel::search(const QString& searchText) {
    const QString trimmedSearchText = searchText.trimmed();
    if (trimmedSearchText == m_searchText) {
        return;
    }

    beginResetModel();
    m_searchText = trimmedSearchText;
    m_trackCache.clear();
    if (trimmedSearchText.isEmpty()) {
        // Back from search mode to the songs of the selected album.
        m_allSongs = m_albumSongs;
        rebuildVisibleSongs();
        endResetModel();
        return;
    }
    // The results are provided asynchronously by the server.
    m_allSongs.clear();
    m_visibleSongs.clear();
    endResetModel();

    emit searchRequested(trimmedSearchText);
}

const QString OpenSubsonicTrackModel::currentSearch() const {
    return m_searchText;
}

bool OpenSubsonicTrackModel::isColumnInternal(int column) {
    const auto field = mapColumn(column);
    switch (field) {
    case ColumnCache::COLUMN_LIBRARYTABLE_ID:
    case ColumnCache::COLUMN_LIBRARYTABLE_URL:
    case ColumnCache::COLUMN_LIBRARYTABLE_CUEPOINT:
    case ColumnCache::COLUMN_LIBRARYTABLE_SAMPLERATE:
    case ColumnCache::COLUMN_LIBRARYTABLE_CHANNELS:
    case ColumnCache::COLUMN_LIBRARYTABLE_MIXXXDELETED:
    case ColumnCache::COLUMN_LIBRARYTABLE_HEADERPARSED:
    case ColumnCache::COLUMN_LIBRARYTABLE_PLAYED:
    case ColumnCache::COLUMN_LIBRARYTABLE_KEY_ID:
    case ColumnCache::COLUMN_LIBRARYTABLE_BPM_LOCK:
    case ColumnCache::COLUMN_LIBRARYTABLE_BEATS_VERSION:
    case ColumnCache::COLUMN_LIBRARYTABLE_PREVIEW:
    case ColumnCache::COLUMN_LIBRARYTABLE_COVERART_SOURCE:
    case ColumnCache::COLUMN_LIBRARYTABLE_COVERART_TYPE:
    case ColumnCache::COLUMN_LIBRARYTABLE_COVERART_LOCATION:
    case ColumnCache::COLUMN_LIBRARYTABLE_COVERART_COLOR:
    case ColumnCache::COLUMN_LIBRARYTABLE_COVERART_DIGEST:
    case ColumnCache::COLUMN_LIBRARYTABLE_COVERART_HASH:
    case ColumnCache::COLUMN_TRACKLOCATIONSTABLE_DIRECTORY:
    case ColumnCache::COLUMN_TRACKLOCATIONSTABLE_FSDELETED:
        return true;
    default:
        return false;
    }
}

TrackModel::SortColumnId OpenSubsonicTrackModel::sortColumnIdFromColumnIndex(
        int index) const {
    return m_sortColumnIdByColumnIndex.value(
            index, TrackModel::SortColumnId::Invalid);
}

int OpenSubsonicTrackModel::columnIndexFromSortColumnId(
        TrackModel::SortColumnId sortColumn) const {
    return m_columnIndexBySortColumnId.value(static_cast<int>(sortColumn), -1);
}

QString OpenSubsonicTrackModel::modelKey(bool noSearch) const {
    if (noSearch) {
        return QStringLiteral("opensubsonic");
    }
    return QStringLiteral("opensubsonic:") + m_searchText;
}

#include "moc_opensubsonictrackmodel.cpp"
