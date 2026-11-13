#pragma once

#include <QByteArray>
#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QUrl>

#include "library/opensubsonic/opensubsonictypes.h"

class QJsonObject;
class QUrlQuery;

namespace mixxx {
namespace opensubsonic {

/// Minimal asynchronous client for the Subsonic / OpenSubsonic REST API.
///
/// Only the endpoints that are required for browsing the remote library are
/// implemented. Playback related endpoints (download) are exposed as URLs so
/// that they can be fetched by the download cache later on.
class SubsonicClient : public QObject {
    Q_OBJECT

  public:
    explicit SubsonicClient(QObject* parent = nullptr);
    ~SubsonicClient() override = default;

    void configure(const QString& serverUrl,
            const QString& userName,
            const QString& password);

    bool isConfigured() const;

    QString serverUrl() const {
        return m_serverUrl;
    }

    /// Unauthenticated check whether the endpoint is reachable at all.
    void testConnection();

    /// getArtists -> emits artistsFetched()
    void fetchArtists();

    /// getArtist -> emits albumsFetched()
    void fetchArtistAlbums(const QString& artistId);

    /// getAlbum -> emits songsFetched()
    void fetchAlbumSongs(const QString& albumId);

    /// search3 -> emits searchResultsFetched()
    void searchSongs(const QString& query);

    /// setRating -> emits ratingUpdateFinished() once the server answered.
    /// A rating of 0 removes the rating on the server.
    void setRating(const QString& songId, int rating);

    /// URL that returns the original audio file without transcoding.
    QUrl downloadUrl(const Song& song) const;

    /// URL for the (downscaled) cover art image of the given cover id.
    QUrl coverArtUrl(const QString& coverId, int size = 0) const;

  signals:
    void connectionTestFinished(bool success, const QString& message);
    void artistsFetched(const QList<mixxx::opensubsonic::Artist>& artists);
    void albumsFetched(const QString& artistId,
            const QList<mixxx::opensubsonic::Album>& albums);
    void songsFetched(const QString& albumId,
            const QList<mixxx::opensubsonic::Song>& songs);
    void searchResultsFetched(const QString& query,
            const QList<mixxx::opensubsonic::Song>& songs);
    void requestFailed(const QString& context, const QString& errorMessage);
    /// Emitted after a setRating request was answered by the server.
    void ratingUpdateFinished(
            const QString& songId, int rating, bool success);

  private:
    void requestJson(const QString& endpoint,
            const QUrlQuery& query,
            const QString& context);
    void addAuthParams(QUrlQuery* pQuery) const;
    QUrl buildUrl(const QString& endpoint, const QUrlQuery& query) const;

    void handleArtists(const QJsonObject& response);
    void handleArtist(const QString& artistId, const QJsonObject& response);
    void handleAlbum(const QString& albumId, const QJsonObject& response);
    void handleSearch(const QString& query, const QJsonObject& response);
    void handleRating(const QString& context, bool success);

    QNetworkAccessManager m_network;
    QString m_serverUrl;
    QString m_userName;
    QString m_password;
};

} // namespace opensubsonic
} // namespace mixxx
