#include "library/opensubsonic/subsonicclient.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QUrlQuery>

#include <algorithm>

#include "util/logger.h"

namespace mixxx {
namespace opensubsonic {

namespace {

const Logger kLogger("OpenSubsonicClient");

const QString kApiVersion = QStringLiteral("1.16.1");
const QString kClientName = QStringLiteral("Mixxx");
constexpr int kRequestTimeoutMillis = 20000;

QString generateSalt() {
    quint64 randomValue = QRandomGenerator::global()->generate64();
    return QString::number(randomValue, 16).leftJustified(8, QLatin1Char('0'));
}

QString md5Hex(const QString& value) {
    return QString::fromLatin1(
            QCryptographicHash::hash(value.toUtf8(), QCryptographicHash::Md5)
                    .toHex());
}

/// Subsonic (JSON) serializes lists as arrays, but be defensive about
/// single-element objects returned by some implementations.
QJsonArray jsonArrayOrSingle(const QJsonValue& value) {
    if (value.isArray()) {
        return value.toArray();
    }
    if (value.isObject()) {
        return QJsonArray{value.toObject()};
    }
    return {};
}

QString jsonString(const QJsonObject& object, const char* key) {
    return object.value(QLatin1String(key)).toString();
}

/// Extracts the id from a context identifier like "artist:<id>".
QString contextPayload(const QString& context) {
    const int separatorIndex = context.indexOf(QLatin1Char(':'));
    if (separatorIndex < 0) {
        return {};
    }
    return context.mid(separatorIndex + 1);
}

Artist parseArtist(const QJsonObject& object) {
    Artist artist;
    artist.id = jsonString(object, "id");
    artist.name = jsonString(object, "name");
    artist.albumCount = object.value(QLatin1String("albumCount")).toInt();
    return artist;
}

Album parseAlbum(const QJsonObject& object) {
    Album album;
    album.id = jsonString(object, "id");
    album.name = jsonString(object, "name");
    album.artist = jsonString(object, "artist");
    album.artistId = jsonString(object, "artistId");
    album.coverId = jsonString(object, "coverArt");
    album.year = object.value(QLatin1String("year")).toInt();
    album.songCount = object.value(QLatin1String("songCount")).toInt();
    return album;
}

/// Parses an ISO 8601 timestamp as returned by OpenSubsonic, which may carry
/// nanosecond precision (e.g. "2025-12-11T19:45:21.75930859Z").
QDateTime parseSubsonicDateTime(const QString& value) {
    QString text = value;
    if (text.isEmpty()) {
        return {};
    }
    const int fractionalStart = text.indexOf(QLatin1Char('.'));
    if (fractionalStart > 0) {
        int fractionalEnd = fractionalStart + 1;
        while (fractionalEnd < text.size() &&
                text.at(fractionalEnd).isDigit()) {
            ++fractionalEnd;
        }
        text.remove(fractionalStart, fractionalEnd - fractionalStart);
    }
    const QDateTime dateTime = QDateTime::fromString(text, Qt::ISODate);
    return dateTime.isValid() ? dateTime.toUTC() : QDateTime();
}

/// Splits a "rating:<songId>:<rating>" context string into its parts.
bool parseRatingContext(
        const QString& context, QString* pSongId, int* pRating) {
    const QString payload = context.mid(
            static_cast<int>(qstrlen("rating:")));
    const int separatorIndex = payload.lastIndexOf(QLatin1Char(':'));
    if (separatorIndex <= 0) {
        return false;
    }
    *pSongId = payload.left(separatorIndex);
    bool ok = false;
    *pRating = QStringView(payload).mid(separatorIndex + 1).toInt(&ok);
    return ok && !pSongId->isEmpty();
}

Song parseSong(const QJsonObject& object) {
    Song song;
    song.id = jsonString(object, "id");
    song.albumId = jsonString(object, "albumId");
    song.artistId = jsonString(object, "artistId");
    song.album = jsonString(object, "album");
    song.artist = jsonString(object, "artist");
    song.displayArtist = jsonString(object, "displayArtist");
    song.title = jsonString(object, "title");
    song.sortName = jsonString(object, "sortName");
    song.genre = jsonString(object, "genre");
    song.suffix = jsonString(object, "suffix");
    song.contentType = jsonString(object, "contentType");
    song.coverId = jsonString(object, "coverArt");
    song.comment = jsonString(object, "comment");
    song.musicBrainzId = jsonString(object, "musicBrainzId");
    song.userRating = object.value(QLatin1String("userRating")).toInt();
    song.trackNumber = object.value(QLatin1String("track")).toInt();
    song.discNumber = object.value(QLatin1String("discNumber")).toInt();
    song.year = object.value(QLatin1String("year")).toInt();
    song.bitRate = object.value(QLatin1String("bitRate")).toInt();
    song.bitDepth = object.value(QLatin1String("bitDepth")).toInt();
    song.samplingRate = object.value(QLatin1String("samplingRate")).toInt();
    song.channelCount = object.value(QLatin1String("channelCount")).toInt();
    song.size = static_cast<qint64>(object.value(QLatin1String("size")).toDouble());
    song.duration = object.value(QLatin1String("duration")).toDouble();
    song.bpm = object.value(QLatin1String("bpm")).toDouble();
    song.starred = !jsonString(object, "starred").isEmpty();
    song.played = parseSubsonicDateTime(jsonString(object, "played"));

    // OpenSubsonic: full list of artists/genres of the song.
    const QJsonArray artists = jsonArrayOrSingle(
            object.value(QLatin1String("artists")));
    for (const QJsonValue& artistValue : artists) {
        const QString name = jsonString(artistValue.toObject(), "name");
        if (!name.isEmpty()) {
            song.artists.append(name);
        }
    }
    const QJsonArray genres = jsonArrayOrSingle(
            object.value(QLatin1String("genres")));
    for (const QJsonValue& genreValue : genres) {
        const QString name = jsonString(genreValue.toObject(), "name");
        if (!name.isEmpty()) {
            song.genres.append(name);
        }
    }

    // Prefer the album artist if the server provides it (OpenSubsonic).
    const QJsonArray albumArtists = jsonArrayOrSingle(
            object.value(QLatin1String("albumArtists")));
    for (const QJsonValue& artistValue : albumArtists) {
        const QString name = jsonString(artistValue.toObject(), "name");
        if (!name.isEmpty()) {
            song.albumArtists.append(name);
        }
    }
    if (!albumArtists.isEmpty()) {
        song.albumArtist = jsonString(albumArtists.first().toObject(), "name");
    }
    const QString displayAlbumArtist =
            jsonString(object, "displayAlbumArtist");
    if (!displayAlbumArtist.isEmpty()) {
        song.albumArtist = displayAlbumArtist;
    }

    song.created = parseSubsonicDateTime(jsonString(object, "created"));
    return song;
}

} // namespace

SubsonicClient::SubsonicClient(QObject* parent)
        : QObject(parent) {
}

void SubsonicClient::configure(
        const QString& serverUrl,
        const QString& userName,
        const QString& password) {
    m_serverUrl = serverUrl.trimmed();
    while (m_serverUrl.endsWith(QLatin1Char('/'))) {
        m_serverUrl.chop(1);
    }
    m_userName = userName;
    m_password = password;
}

bool SubsonicClient::isConfigured() const {
    return !m_serverUrl.isEmpty() && !m_userName.isEmpty();
}

QUrl SubsonicClient::buildUrl(
        const QString& endpoint,
        const QUrlQuery& query) const {
    QUrl url(m_serverUrl + QStringLiteral("/rest/") + endpoint);
    url.setQuery(query);
    return url;
}

void SubsonicClient::addAuthParams(QUrlQuery* pQuery) const {
    const QString salt = generateSalt();
    pQuery->addQueryItem(QStringLiteral("u"), m_userName);
    pQuery->addQueryItem(QStringLiteral("s"), salt);
    pQuery->addQueryItem(
            QStringLiteral("t"), md5Hex(m_password + salt));
    pQuery->addQueryItem(QStringLiteral("v"), kApiVersion);
    pQuery->addQueryItem(QStringLiteral("c"), kClientName);
    pQuery->addQueryItem(QStringLiteral("f"), QStringLiteral("json"));
}

QUrl SubsonicClient::downloadUrl(const Song& song) const {
    QUrlQuery query;
    addAuthParams(&query);
    query.addQueryItem(QStringLiteral("id"), song.id);
    return buildUrl(QStringLiteral("download"), query);
}

QUrl SubsonicClient::coverArtUrl(const QString& coverId, int size) const {
    QUrlQuery query;
    addAuthParams(&query);
    query.addQueryItem(QStringLiteral("id"), coverId);
    if (size > 0) {
        query.addQueryItem(QStringLiteral("size"), QString::number(size));
    }
    return buildUrl(QStringLiteral("getCoverArt"), query);
}

void SubsonicClient::testConnection() {
    QUrlQuery query;
    addAuthParams(&query);
    requestJson(QStringLiteral("ping"), query, QStringLiteral("ping"));
}

void SubsonicClient::fetchArtists() {
    QUrlQuery query;
    addAuthParams(&query);
    requestJson(QStringLiteral("getArtists"), query, QStringLiteral("artists"));
}

void SubsonicClient::fetchArtistAlbums(const QString& artistId) {
    QUrlQuery query;
    addAuthParams(&query);
    query.addQueryItem(QStringLiteral("id"), artistId);
    requestJson(QStringLiteral("getArtist"),
            query,
            QStringLiteral("artist:") + artistId);
}

void SubsonicClient::fetchAlbumSongs(const QString& albumId) {
    QUrlQuery query;
    addAuthParams(&query);
    query.addQueryItem(QStringLiteral("id"), albumId);
    requestJson(QStringLiteral("getAlbum"),
            query,
            QStringLiteral("album:") + albumId);
}

void SubsonicClient::searchSongs(const QString& queryText) {
    const QString trimmedQuery = queryText.trimmed();
    if (trimmedQuery.isEmpty()) {
        return;
    }
    QUrlQuery query;
    addAuthParams(&query);
    query.addQueryItem(QStringLiteral("query"), trimmedQuery);
    // Only songs are requested, the library tree is used for artists/albums.
    query.addQueryItem(QStringLiteral("songCount"), QStringLiteral("200"));
    query.addQueryItem(QStringLiteral("artistCount"), QStringLiteral("0"));
    query.addQueryItem(QStringLiteral("albumCount"), QStringLiteral("0"));
    requestJson(QStringLiteral("search3"),
            query,
            QStringLiteral("search:") + trimmedQuery);
}

void SubsonicClient::setRating(const QString& songId, int rating) {
    VERIFY_OR_DEBUG_ASSERT(isConfigured()) {
        emit ratingUpdateFinished(songId, rating, false);
        return;
    }
    const int clampedRating = std::clamp(rating, 0, 5);
    QUrlQuery query;
    addAuthParams(&query);
    query.addQueryItem(QStringLiteral("id"), songId);
    query.addQueryItem(
            QStringLiteral("rating"), QString::number(clampedRating));
    requestJson(QStringLiteral("setRating"),
            query,
            QStringLiteral("rating:") + songId + QLatin1Char(':') +
                    QString::number(clampedRating));
}

void SubsonicClient::requestJson(
        const QString& endpoint,
        const QUrlQuery& query,
        const QString& context) {
    VERIFY_OR_DEBUG_ASSERT(isConfigured()) {
        emit requestFailed(context,
                tr("OpenSubsonic server is not configured."));
        return;
    }

    const QUrl url = buildUrl(endpoint, query);
    QNetworkRequest request(url);
    // The Subsonic API works on top of plain HTTP. Do not let Qt follow
    // redirects silently, since that would drop the query parameters.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
            QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(kRequestTimeoutMillis);

    kLogger.debug() << "Requesting" << endpoint;

    QNetworkReply* pReply = m_network.get(request);
    connect(pReply,
            &QNetworkReply::finished,
            this,
            [this, pReply, context]() {
                pReply->deleteLater();

                if (pReply->error() != QNetworkReply::NoError) {
                    const QString errorMessage = pReply->errorString();
                    kLogger.warning()
                            << "Request failed:" << context << errorMessage;
                    emit requestFailed(context, errorMessage);
                    return;
                }

                const QByteArray payload = pReply->readAll();
                QJsonParseError parseError;
                const QJsonDocument document =
                        QJsonDocument::fromJson(payload, &parseError);
                if (parseError.error != QJsonParseError::NoError) {
                    kLogger.warning()
                            << "Invalid JSON response:" << context
                            << parseError.errorString();
                    emit requestFailed(context, parseError.errorString());
                    return;
                }

                const QJsonObject response =
                        document.object()
                                .value(QLatin1String("subsonic-response"))
                                .toObject();
                if (response.isEmpty()) {
                    emit requestFailed(context,
                            tr("Unexpected response format."));
                    return;
                }
                const QString status =
                        jsonString(response, "status");
                if (status != QLatin1String("ok")) {
                    const QJsonObject error =
                            response.value(QLatin1String("error")).toObject();
                    QString message = jsonString(error, "message");
                    if (message.isEmpty()) {
                        message = tr("Server returned status '%1'.")
                                          .arg(status);
                    }
                    kLogger.warning()
                            << "Request rejected:" << context << message;
                    emit requestFailed(context, message);
                    return;
                }

                if (context == QLatin1String("ping")) {
                    emit connectionTestFinished(
                            true, jsonString(response, "version"));
                } else if (context == QLatin1String("artists")) {
                    handleArtists(response);
                } else if (context.startsWith(QLatin1String("artist:"))) {
                    handleArtist(contextPayload(context), response);
                } else if (context.startsWith(QLatin1String("album:"))) {
                    handleAlbum(contextPayload(context), response);
                } else if (context.startsWith(QLatin1String("search:"))) {
                    handleSearch(contextPayload(context), response);
                } else if (context.startsWith(QLatin1String("rating:"))) {
                    handleRating(context, true);
                }
            });
}

void SubsonicClient::handleRating(const QString& context, bool success) {
    QString songId;
    int rating = 0;
    if (!parseRatingContext(context, &songId, &rating)) {
        kLogger.warning() << "Malformed rating context:" << context;
        return;
    }
    if (success) {
        kLogger.debug() << "Stored rating" << rating << "for song" << songId;
    } else {
        kLogger.warning() << "Failed to store rating" << rating << "for song"
                          << songId;
    }
    emit ratingUpdateFinished(songId, rating, success);
}

void SubsonicClient::handleArtists(const QJsonObject& response) {
    QList<Artist> artists;
    const QJsonObject artistsObject =
            response.value(QLatin1String("artists")).toObject();
    const QJsonArray indices = jsonArrayOrSingle(
            artistsObject.value(QLatin1String("index")));
    for (const QJsonValue& indexValue : indices) {
        const QJsonArray artistArray = jsonArrayOrSingle(
                indexValue.toObject().value(QLatin1String("artist")));
        for (const QJsonValue& artistValue : artistArray) {
            Artist artist = parseArtist(artistValue.toObject());
            if (!artist.id.isEmpty()) {
                artists.append(std::move(artist));
            }
        }
    }
    kLogger.debug() << "Received" << artists.size() << "artists";
    emit artistsFetched(artists);
}

void SubsonicClient::handleArtist(
        const QString& artistId,
        const QJsonObject& response) {
    QList<Album> albums;
    const QJsonObject artistObject =
            response.value(QLatin1String("artist")).toObject();
    const QJsonArray albumArray = jsonArrayOrSingle(
            artistObject.value(QLatin1String("album")));
    for (const QJsonValue& albumValue : albumArray) {
        Album album = parseAlbum(albumValue.toObject());
        if (!album.id.isEmpty()) {
            albums.append(std::move(album));
        }
    }
    kLogger.debug() << "Received" << albums.size()
                    << "albums for artist" << artistId;
    emit albumsFetched(artistId, albums);
}

void SubsonicClient::handleAlbum(
        const QString& albumId,
        const QJsonObject& response) {
    const QJsonObject albumObject =
            response.value(QLatin1String("album")).toObject();
    const QString albumArtistRaw = jsonString(albumObject, "artist");

    QList<Song> songs;
    const QJsonArray songArray = jsonArrayOrSingle(
            albumObject.value(QLatin1String("song")));
    for (const QJsonValue& songValue : songArray) {
        Song song = parseSong(songValue.toObject());
        if (song.id.isEmpty()) {
            continue;
        }
        if (song.albumArtist.isEmpty()) {
            song.albumArtist = albumArtistRaw;
        }
        if (song.album.isEmpty()) {
            song.album = jsonString(albumObject, "name");
        }
        songs.append(std::move(song));
    }
    kLogger.debug() << "Received" << songs.size()
                    << "songs for album" << albumId;
    emit songsFetched(albumId, songs);
}

void SubsonicClient::handleSearch(
        const QString& query,
        const QJsonObject& response) {
    QList<Song> songs;
    const QJsonObject searchResult =
            response.value(QLatin1String("searchResult3")).toObject();
    const QJsonArray songArray = jsonArrayOrSingle(
            searchResult.value(QLatin1String("song")));
    for (const QJsonValue& songValue : songArray) {
        Song song = parseSong(songValue.toObject());
        if (!song.id.isEmpty()) {
            songs.append(std::move(song));
        }
    }
    kLogger.debug() << "Search" << query << "returned" << songs.size()
                    << "songs";
    emit searchResultsFetched(query, songs);
}

} // namespace opensubsonic
} // namespace mixxx

#include "moc_subsonicclient.cpp"
