#include "sources/httpstreamio.h"

#include <cstring>

#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QTimer>

extern "C" {

#include <libavformat/avio.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>

} // extern "C"

#include <algorithm>

#include "util/logger.h"

namespace mixxx {

namespace {

const Logger kLogger("HttpStreamIo");

constexpr int kAvioBufferSize = 64 * 1024;
constexpr int kRequestTimeoutMillis = 30000;

} // namespace

HttpStreamIo::HttpStreamIo(const QUrl& url)
        : m_url(url),
          m_pNetworkAccessManager(std::make_unique<QNetworkAccessManager>()),
          m_pReply(nullptr),
          m_bufferOffset(0),
          m_bufferStartPosition(0),
          m_requestPosition(0),
          m_nextDataPosition(0),
          m_size(-1),
          m_responseChecked(false),
          m_rangeRequestsSupported(false),
          m_replyFinished(false),
          m_reachedEnd(false),
          m_failed(false) {
    DEBUG_ASSERT(m_url.isValid());
    DEBUG_ASSERT(!m_url.isLocalFile());
}

HttpStreamIo::~HttpStreamIo() {
    abortReply();
}

AVIOContext* HttpStreamIo::createAvioContext() {
    auto* pBuffer = static_cast<unsigned char*>(av_malloc(kAvioBufferSize));
    if (pBuffer == nullptr) {
        kLogger.warning() << "Failed to allocate the input buffer";
        return nullptr;
    }
    AVIOContext* pAvioContext = avio_alloc_context(pBuffer,
            kAvioBufferSize,
            /*write_flag*/ 0,
            this,
            &HttpStreamIo::readPacket,
            /*write_packet*/ nullptr,
            &HttpStreamIo::seekStream);
    if (pAvioContext == nullptr) {
        av_free(pBuffer);
        kLogger.warning() << "Failed to allocate the AVIO context";
        return nullptr;
    }
    return pAvioContext;
}

//static
void HttpStreamIo::releaseAvioContext(AVIOContext** ppAvioContext) {
    if (ppAvioContext == nullptr || *ppAvioContext == nullptr) {
        return;
    }
    av_freep(&(*ppAvioContext)->buffer);
    avio_context_free(ppAvioContext);
}

//static
int HttpStreamIo::readPacket(void* pOpaque, uint8_t* pBuffer, int bufferSize) {
    DEBUG_ASSERT(pOpaque != nullptr);
    auto* pThis = static_cast<HttpStreamIo*>(pOpaque);
    return pThis->read(pBuffer, bufferSize);
}

//static
int64_t HttpStreamIo::seekStream(
        void* pOpaque, int64_t offset, int whence) {
    DEBUG_ASSERT(pOpaque != nullptr);
    auto* pThis = static_cast<HttpStreamIo*>(pOpaque);
    return pThis->seek(offset, whence);
}

bool HttpStreamIo::startRequestIfNeeded() {
    if (m_pReply != nullptr) {
        return true;
    }
    if (m_reachedEnd) {
        return false;
    }
    DEBUG_ASSERT(!hasBufferedData());
    m_requestPosition = position();
    m_nextDataPosition = m_requestPosition;
    m_responseChecked = false;
    m_replyFinished = false;

    QNetworkRequest request(m_url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
            QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setRawHeader("Range",
            QByteArray("bytes=") + QByteArray::number(m_requestPosition) + '-');

    m_pReply = m_pNetworkAccessManager->get(request);
    if (m_pReply == nullptr) {
        kLogger.warning() << "Failed to request" << m_url;
        return false;
    }
    kLogger.debug() << "Requesting" << m_url << "from byte"
                    << m_requestPosition;
    return true;
}

void HttpStreamIo::checkResponse() {
    if (m_responseChecked || m_pReply == nullptr) {
        return;
    }
    const int statusCode = m_pReply->attribute(
                                           QNetworkRequest::HttpStatusCodeAttribute)
                                   .toInt();
    if (statusCode == 0) {
        // No response yet, will be checked again later.
        return;
    }
    m_responseChecked = true;
    if (statusCode != 200 && statusCode != 206) {
        kLogger.warning() << "Unexpected HTTP status" << statusCode << "for"
                          << m_url;
        m_failed = true;
        return;
    }
    m_rangeRequestsSupported = (statusCode == 206);
    if (!m_rangeRequestsSupported && m_requestPosition > 0) {
        // The server ignored the range request and started at the beginning,
        // the bytes before the requested position have to be dropped.
        kLogger.debug() << "Server does not support range requests, skipping"
                        << m_requestPosition << "bytes";
        m_nextDataPosition = 0;
    }
    const QVariant contentLength =
            m_pReply->header(QNetworkRequest::ContentLengthHeader);
    if (contentLength.isValid() && contentLength.toLongLong() >= 0) {
        m_size = m_nextDataPosition + contentLength.toLongLong();
    }
    kLogger.debug() << "Stream size" << m_size << "range requests:"
                    << m_rangeRequestsSupported;
}

void HttpStreamIo::consumeReplyData() {
    if (m_pReply == nullptr || m_failed) {
        return;
    }
    checkResponse();
    if (m_failed) {
        return;
    }
    const QByteArray data = m_pReply->readAll();
    if (data.isEmpty()) {
        if (m_replyFinished) {
            m_reachedEnd = true;
        }
        return;
    }
    int offset = 0;
    if (m_nextDataPosition < m_requestPosition) {
        // Drop the part of the stream that precedes the requested position.
        const qint64 bytesToSkip = std::min<qint64>(
                m_requestPosition - m_nextDataPosition, data.size());
        offset = static_cast<int>(bytesToSkip);
    }
    if (offset < data.size()) {
        if (!hasBufferedData()) {
            // Start over with a clean buffer, the consumed part is no longer
            // needed.
            m_downloadBuffer.clear();
            m_bufferOffset = 0;
            m_bufferStartPosition = m_nextDataPosition + offset;
        }
        m_downloadBuffer.append(data.constData() + offset, data.size() - offset);
        m_nextDataPosition += data.size();
    } else {
        m_nextDataPosition += offset;
    }
}

bool HttpStreamIo::waitForData() {
    while (!hasBufferedData()) {
        if (m_failed || m_reachedEnd) {
            return false;
        }
        if (!startRequestIfNeeded()) {
            m_failed = true;
            return false;
        }
        if (!m_pReply->isFinished()) {
            QEventLoop eventLoop;
            QTimer timeoutTimer;
            timeoutTimer.setSingleShot(true);
            bool timeoutTriggered = false;
            const auto readyReadConnection = QObject::connect(
                    m_pReply, &QIODevice::readyRead, &eventLoop, &QEventLoop::quit);
            const auto finishedConnection = QObject::connect(
                    m_pReply, &QNetworkReply::finished, &eventLoop, &QEventLoop::quit);
            const auto timeoutConnection = QObject::connect(
                    &timeoutTimer, &QTimer::timeout, &eventLoop, [&]() {
                        timeoutTriggered = true;
                        eventLoop.quit();
                    });

            timeoutTimer.start(kRequestTimeoutMillis);
            eventLoop.exec(QEventLoop::ExcludeUserInputEvents);
            timeoutTimer.stop();
            QObject::disconnect(readyReadConnection);
            QObject::disconnect(finishedConnection);
            QObject::disconnect(timeoutConnection);

            if (timeoutTriggered) {
                kLogger.warning() << "Timeout while reading from" << m_url;
                abortReply();
                m_failed = true;
                return false;
            }
        }
        m_replyFinished = m_pReply->isFinished();
        consumeReplyData();
        if (!hasBufferedData() && m_replyFinished) {
            // The whole stream has been received and consumed.
            abortReply();
            m_reachedEnd = true;
            return false;
        }
    }
    return true;
}

void HttpStreamIo::abortReply() {
    if (m_pReply == nullptr) {
        return;
    }
    QNetworkReply* pReply = m_pReply;
    m_pReply = nullptr;
    QObject::disconnect(pReply, nullptr, nullptr, nullptr);
    pReply->abort();
    pReply->deleteLater();
}

int HttpStreamIo::read(uint8_t* pBuffer, int bufferSize) {
    if (bufferSize <= 0) {
        return AVERROR(EINVAL);
    }
    if (!waitForData()) {
        return m_failed ? AVERROR(EIO) : AVERROR_EOF;
    }
    const int bytesToCopy = std::min(bufferSize, bufferedBytes());
    std::memcpy(pBuffer, m_downloadBuffer.constData() + m_bufferOffset, bytesToCopy);
    m_bufferOffset += bytesToCopy;
    if (!hasBufferedData()) {
        // Keep track of the absolute stream position before the buffer is
        // dropped.
        m_bufferStartPosition += m_downloadBuffer.size();
        m_downloadBuffer.clear();
        m_bufferOffset = 0;
    }
    return bytesToCopy;
}

int64_t HttpStreamIo::seek(int64_t offset, int whence) {
    if (whence == AVSEEK_SIZE) {
        return m_size;
    }
    whence &= ~AVSEEK_FORCE;
    int64_t target;
    switch (whence) {
    case SEEK_SET:
        target = offset;
        break;
    case SEEK_CUR:
        target = position() + offset;
        break;
    case SEEK_END:
        if (m_size < 0) {
            kLogger.warning() << "Cannot seek relative to the end of an unknown stream size";
            return AVERROR(ENOSYS);
        }
        target = m_size + offset;
        break;
    default:
        return AVERROR(EINVAL);
    }
    if (target < 0) {
        return AVERROR(EINVAL);
    }
    if (m_size >= 0 && target > m_size) {
        target = m_size;
    }
    const qint64 bufferStart = m_bufferStartPosition;
    const qint64 bufferEnd = bufferStart + m_downloadBuffer.size();
    if (target >= bufferStart && target <= bufferEnd) {
        // Already buffered, just move the read position.
        m_bufferOffset = static_cast<int>(target - bufferStart);
        return target;
    }
    kLogger.debug() << "Seeking to" << target << "of" << m_size;
    abortReply();
    m_downloadBuffer.clear();
    m_bufferOffset = 0;
    m_bufferStartPosition = target;
    m_requestPosition = target;
    m_nextDataPosition = target;
    m_reachedEnd = false;
    m_failed = false;
    m_responseChecked = false;
    return target;
}

} // namespace mixxx
