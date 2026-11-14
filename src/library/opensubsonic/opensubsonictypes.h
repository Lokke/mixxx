#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>

namespace mixxx {
namespace opensubsonic {

/// Subset of the Subsonic "ArtistID3" entity that is relevant for browsing.
struct Artist final {
    QString id;
    QString name;
    int albumCount = 0;
};

/// Subset of the Subsonic "AlbumID3" entity that is relevant for browsing.
struct Album final {
    QString id;
    QString name;
    QString artist;
    QString artistId;
    QString coverId;
    int year = 0;
    int songCount = 0;
};

/// Subset of the Subsonic "Child" (song) entity that is relevant for
/// displaying and playing a track.
///
/// Mixxx itself only knows a single artist/album artist string per track
/// (mixxx::TrackInfo::artist), so the OpenSubsonic fields for multiple
/// artists (artists[], albumArtists[]) are collected here and rendered via
/// displayArtist()/displayAlbumArtist() when available.
struct Song final {
    QString id;
    QString albumId;
    QString artistId;
    QString album;
    /// Raw value of the legacy "artist" field.
    QString artist;
    /// OpenSubsonic "displayArtist", e.g. "Artist 1 feat. Artist 2".
    QString displayArtist;
    QString albumArtist;
    /// OpenSubsonic "displayAlbumArtist".
    QString displayAlbumArtist;
    /// Names of the OpenSubsonic artists[] array.
    QStringList artists;
    /// Names of the OpenSubsonic albumArtists[] array.
    QStringList albumArtists;
    QString title;
    QString sortName;
    QString genre;
    QStringList genres;
    QString suffix;
    QString contentType;
    QString coverId;
    QString comment;
    QString musicBrainzId;
    /// OpenSubsonic "userRating": 0 = unrated, 1..5 stars.
    int userRating = 0;
    int trackNumber = 0;
    int discNumber = 0;
    int year = 0;
    int bitRate = 0;
    int bitDepth = 0;
    int samplingRate = 0;
    int channelCount = 0;
    qint64 size = 0;
    double duration = 0.0;
    double bpm = 0.0;
    bool starred = false;
    QDateTime created;
    QDateTime played;

    /// Single artist string for display, preferring the server side
    /// multi-artist rendering.
    QString artistForDisplay() const {
        if (!displayArtist.isEmpty()) {
            return displayArtist;
        }
        if (!artists.isEmpty()) {
            return artists.join(QStringLiteral(", "));
        }
        return artist;
    }

    /// Single album artist string for display.
    QString albumArtistForDisplay() const {
        if (!displayAlbumArtist.isEmpty()) {
            return displayAlbumArtist;
        }
        if (!albumArtists.isEmpty()) {
            return albumArtists.join(QStringLiteral(", "));
        }
        return albumArtist;
    }

    /// Genre string for display, including the OpenSubsonic genres[] list.
    QString genreForDisplay() const {
        if (!genre.isEmpty()) {
            return genre;
        }
        return genres.join(QStringLiteral(", "));
    }
};

} // namespace opensubsonic
} // namespace mixxx
