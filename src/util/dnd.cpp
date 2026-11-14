#include "util/dnd.h"

#include <QDirIterator>

#include "control/controlobject.h"
#include "library/parser.h"
#include "mixer/playermanager.h"
#include "preferences/dialog/dlgprefdeck.h"
#include "sources/soundsourceproxy.h"
#include "track/track.h"

namespace {

QDrag* dragUrls(
        const QList<QUrl>& trackUrls,
        QWidget* pDragSource,
        const QString& sourceIdentifier) {
    if (trackUrls.isEmpty()) {
        return nullptr;
    }

    QMimeData* mimeData = new QMimeData();
    mimeData->setUrls(trackUrls);
    mimeData->setText(sourceIdentifier);

    QDrag* drag = new QDrag(pDragSource);
    drag->setMimeData(mimeData);
    drag->setPixmap(QPixmap(":/images/library/ic_library_drag_and_drop.svg"));
    drag->exec(Qt::CopyAction);

    return drag;
}

bool addFileToList(
        mixxx::FileInfo fileInfo,
        QList<mixxx::FileInfo>* fileInfos) {
    if (!fileInfo.checkFileExists()) {
        return false;
    }

    // Filter out invalid URLs (eg. files that aren't supported audio
    // filetypes, etc.)
    if (!SoundSourceProxy::isFileSupported(fileInfo)) {
        return false;
    }

    fileInfos->append(std::move(fileInfo));
    return true;
}

QList<mixxx::FileInfo> dropEventFiles(
        const QMimeData& mimeData,
        const QString& sourceIdentifier,
        bool stopOnFirstMatch,
        bool acceptPlaylists) {
    qDebug() << "dropEventFiles()" << mimeData.hasUrls() << mimeData.urls();
    qDebug() << "mimeData.hasText()" << mimeData.hasText() << mimeData.text();

    if (!mimeData.hasUrls() ||
            (mimeData.hasText() && mimeData.text() == sourceIdentifier)) {
        return {};
    }

    return DragAndDropHelper::supportedTracksFromUrls(
            mimeData.urls(),
            stopOnFirstMatch,
            acceptPlaylists);
}

/// Whether the URL references a remote audio stream instead of a local file.
///
/// Such URLs are resolved after the drop by the library feature that owns
/// them (e.g. "OpenSubsonic"), which turns them into a playable track.
bool isRemoteAudioUrl(const QUrl& url) {
    const QString scheme = url.scheme().toLower();
    return scheme == QLatin1String("http") || scheme == QLatin1String("https");
}

QUrl firstRemoteAudioUrl(const QList<QUrl>& urls) {
    for (const QUrl& url : urls) {
        if (isRemoteAudioUrl(url)) {
            return url;
        }
    }
    return QUrl();
}

// Allow loading to a player if the player isn't playing
// or the settings allow interrupting a playing player.
bool allowLoadToPlayer(
        const QString& group,
        UserSettingsPointer pConfig) {
    // Always allow loads to preview decks
    if (PlayerManager::isPreviewDeckGroup(group)) {
        return true;
    }

    // Allow if deck is not playing
    if (ControlObject::get(ConfigKey(group, "play")) <= 0.0) {
        return true;
    }

    bool allowLoadTrackIntoPlayingDeck = false;
    if (pConfig->exists(kConfigKeyLoadWhenDeckPlaying)) {
        int loadWhenDeckPlaying =
                pConfig->getValueString(kConfigKeyLoadWhenDeckPlaying).toInt();
        switch (static_cast<LoadWhenDeckPlaying>(loadWhenDeckPlaying)) {
        case LoadWhenDeckPlaying::Allow:
        case LoadWhenDeckPlaying::AllowButStopDeck:
            allowLoadTrackIntoPlayingDeck = true;
            break;
        case LoadWhenDeckPlaying::Reject:
            break;
        }
    } else {
        // support older version of this flag
        allowLoadTrackIntoPlayingDeck =
                pConfig->getValue<bool>(kConfigKeyAllowTrackLoadToPlayingDeck);
    }
    return allowLoadTrackIntoPlayingDeck;
}

// Helper function for DragAndDropHelper::mousePressed and DragAndDropHelper::mouseMoveInitiatesDrag
bool mouseMoveInitiatesDragHelper(QMouseEvent* pEvent, bool isPress) {
    if (pEvent->buttons() != Qt::LeftButton) {
        return false;
    }
    const qreal x = pEvent->position().x();
    const qreal y = pEvent->position().y();

    static qreal pressX{};
    static qreal pressY{};

    if (isPress) {
        pressX = x;
        pressY = y;
        return false;
    }

    const qreal threshold = static_cast<qreal>(QApplication::startDragDistance());
    // X and Y distance since press
    const qreal dx = x - pressX;
    const qreal dy = y - pressY;

    // Check if the distance surpasses the threshold. Per Pythagoras Theorem:
    // distance = sqrt(dx^2 + dy^2)
    // We can avoid the sqrt by squaring the threshold.

    return dx * dx + dy * dy >= threshold * threshold;
}

} // anonymous namespace

// static
bool DragAndDropHelper::urlsContainSupportedTrackFiles(
        const QList<QUrl>& urls,
        bool acceptPlaylists) {
    return !supportedTracksFromUrls(urls, true, acceptPlaylists).isEmpty();
}

//static
QList<mixxx::FileInfo> DragAndDropHelper::supportedTracksFromUrls(
        const QList<QUrl>& urls,
        bool stopOnFirstMatch,
        bool acceptPlaylists) {
    QList<mixxx::FileInfo> fileInfos;
    for (const QUrl& url : urls) {
        // XXX: Possible WTF alert - Previously we thought we needed
        // toString() here but what you actually want in any case when
        // converting a QUrl to a file system path is
        // QUrl::toLocalFile(). This is the second time we have flip-flopped
        // on this, but I think toLocalFile() should work in any
        // case. toString() absolutely does not work when you pass the
        // result to a (this comment was never finished by the original
        // author).
        QString file(url.toLocalFile());

        // If the file is on a network share, try just converting the URL to
        // a string...
        if (file.isEmpty()) {
            file = url.toString();
        }

        if (file.isEmpty()) {
            continue;
        }

        if (acceptPlaylists && Parser::isPlaylistFilenameSupported(file)) {
            // Url is a playlist file
            const QList<QString> track_list = Parser::parse(file);
            for (const auto& playlistFile : track_list) {
                addFileToList(mixxx::FileInfo(playlistFile), &fileInfos);
            }
        } else {
            // Url is a file or a directory
            mixxx::FileInfo fileInfo = mixxx::FileInfo::fromQUrl(url);
            if (fileInfo.isFile()) {
                addFileToList(fileInfo, &fileInfos);
            } else if (fileInfo.isDir()) {
                // Collect supported files recursively
                const QStringList nameFilters = SoundSourceProxy::getSupportedFileNamePatterns();

                QDirIterator fileIt(fileInfo.location(),
                        nameFilters,
                        QDir::Files | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories);

                while (fileIt.hasNext()) {
                    addFileToList(mixxx::FileInfo(fileIt.next()), &fileInfos);
                    if (stopOnFirstMatch && !fileInfos.isEmpty()) {
                        break;
                    }
                }
            }
        }

        if (stopOnFirstMatch && !fileInfos.isEmpty()) {
            break;
        }
    }

    return fileInfos;
}

//static
bool DragAndDropHelper::allowDeckCloneAttempt(
        const QDropEvent& event,
        const QString& group) {
    // only allow clones to decks
    if (!PlayerManager::isDeckGroup(group, nullptr)) {
        return false;
    }

    // forbid clone if shift is pressed
    const auto modifiers = event.modifiers();
    if (modifiers.testFlag(Qt::ShiftModifier)) {
        return false;
    }

    if (!event.mimeData()->hasText() ||
            // prevent cloning to ourself
            event.mimeData()->text() == group ||
            // only allow clone from decks
            !PlayerManager::isDeckGroup(event.mimeData()->text(), nullptr)) {
        return false;
    }

    return true;
}

// static
void DragAndDropHelper::mousePressed(QMouseEvent* pEvent) {
    mouseMoveInitiatesDragHelper(pEvent, true);
}

// static
bool DragAndDropHelper::mouseMoveInitiatesDrag(QMouseEvent* pEvent) {
    return mouseMoveInitiatesDragHelper(pEvent, false);
}

//static
bool DragAndDropHelper::dragEnterAccept(
        const QMimeData& mimeData,
        const QString& sourceIdentifier,
        bool stopOnFirstMatch,
        bool acceptPlaylists) {
    // Remote audio streams have no local file to inspect, they are resolved
    // by the owning library feature after the drop.
    if (!firstRemoteAudioUrl(mimeData.urls()).isEmpty()) {
        return !(mimeData.hasText() && mimeData.text() == sourceIdentifier);
    }
    // TODO(XXX): This operation blocks the UI when many
    // files are selected!
    const auto files = dropEventFiles(
            mimeData, sourceIdentifier, stopOnFirstMatch, acceptPlaylists);
    return !files.isEmpty();
}

//static
QDrag* DragAndDropHelper::dragTrack(
        TrackPointer pTrack,
        QWidget* pDragSource,
        const QString& sourceIdentifier) {
    QList<QUrl> trackUrls;
    trackUrls.append(pTrack->getFileInfo().toQUrl());
    return dragUrls(trackUrls, pDragSource, sourceIdentifier);
}

//static
QDrag* DragAndDropHelper::dragTrackLocations(
        const QList<QString>& locations,
        QWidget* pDragSource,
        const QString& sourceIdentifier) {
    QList<QUrl> trackUrls;
    foreach (QString location, locations) {
        const QUrl url(location);
        if (isRemoteAudioUrl(url)) {
            // Keep the stream URL intact, FileInfo::toQUrl() would turn it
            // into a broken local file URL.
            trackUrls.append(url);
        } else {
            trackUrls.append(mixxx::FileInfo(location).toQUrl());
        }
    }
    return dragUrls(trackUrls, pDragSource, sourceIdentifier);
}

//static
void DragAndDropHelper::handleTrackDragEnterEvent(
        QDragEnterEvent* pEvent,
        const QString& group,
        UserSettingsPointer pConfig) {
    if (allowLoadToPlayer(group, pConfig) &&
            dragEnterAccept(*pEvent->mimeData(), group, true, false)) {
        pEvent->acceptProposedAction();
    } else {
        qDebug() << "Ignoring drag enter event, loading not allowed";
        pEvent->ignore();
    }
}

//static
void DragAndDropHelper::handleTrackDropEvent(
        QDropEvent* pEvent,
        TrackDropTarget& target,
        const QString& group,
        UserSettingsPointer pConfig) {
    if (allowLoadToPlayer(group, pConfig)) {
        if (allowDeckCloneAttempt(*pEvent, group)) {
            pEvent->accept();
            target.emitCloneDeck(pEvent->mimeData()->text(), group);
            return;
        } else {
            // A remote stream URL is passed through unchanged. The library
            // feature that owns the URL decides how to load it.
            const QUrl remoteUrl = firstRemoteAudioUrl(pEvent->mimeData()->urls());
            if (!remoteUrl.isEmpty()) {
                pEvent->accept();
                target.emitTrackDropped(remoteUrl.toString(), group);
                return;
            }
            const QList<mixxx::FileInfo> files = dropEventFiles(
                    *pEvent->mimeData(), group, true, false);
            if (!files.isEmpty()) {
                pEvent->accept();
                target.emitTrackDropped(files.at(0).location(), group);
                return;
            }
        }
    }
    pEvent->ignore();
}
