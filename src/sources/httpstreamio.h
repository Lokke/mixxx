#pragma once

#include <memory>

#include <QByteArray>
#include <QUrl>

class QEventLoop;
class QNetworkAccessManager;
class QNetworkReply;
struct AVIOContext;

namespace mixxx {

/// Blocking reader for HTTP(S) audio streams that feeds libav through a
/// custom AVIOContext.
///
/// libav is able to open network streams itself, but only if it has been
/// compiled with the HTTP protocol and a TLS backend. Builds without one (the
/// dependency packages of the Windows and macOS installers for example) fail
/// with "https protocol not found". Reading the stream with Qt avoids that
/// dependency completely, because Qt uses the TLS stack of the platform.
///
/// The class is not thread safe, it must be used from the thread that created
/// it. All operations block until the requested data arrived, the timeout
/// expired or the stream ended. It is therefore only suitable for threads
/// that are allowed to block, which is the case for the caching reader and
/// the analyzer threads but not for the engine thread.
class HttpStreamIo final {
  public:
    explicit HttpStreamIo(const QUrl& url);
    HttpStreamIo(const HttpStreamIo&) = delete;
    HttpStreamIo(HttpStreamIo&&) = delete;
    HttpStreamIo& operator=(const HttpStreamIo&) = delete;
    HttpStreamIo& operator=(HttpStreamIo&&) = delete;
    ~HttpStreamIo();

    /// Creates an AVIOContext that reads from this stream.
    ///
    /// Ownership of the returned AVIOContext, including its buffer, is
    /// transferred to the caller. It must be released with
    /// releaseAvioContext() after the AVFormatContext has been closed.
    AVIOContext* createAvioContext();
    static void releaseAvioContext(AVIOContext** ppAvioContext);

    /// Absolute position of the next byte that read() will return.
    qint64 position() const {
        return m_bufferStartPosition + m_bufferOffset;
    }
    /// Total size of the stream in bytes, -1 if it is not known yet.
    qint64 size() const {
        return m_size;
    }

  private:
    static int readPacket(void* pOpaque, uint8_t* pBuffer, int bufferSize);
    static int64_t seekStream(void* pOpaque, int64_t offset, int whence);

    int read(uint8_t* pBuffer, int bufferSize);
    int64_t seek(int64_t offset, int whence);

    /// Ensures that a request for the current position is running.
    bool startRequestIfNeeded();
    /// Blocks until at least one byte is buffered. Returns false if the stream
    /// ended or failed.
    bool waitForData();
    /// Moves the bytes of the current reply into the buffer and throws away
    /// the part that precedes the requested position.
    void consumeReplyData();
    void checkResponse();
    void abortReply();
    bool hasBufferedData() const {
        return m_bufferOffset < m_downloadBuffer.size();
    }
    int bufferedBytes() const {
        return static_cast<int>(m_downloadBuffer.size()) - m_bufferOffset;
    }

    const QUrl m_url;
    std::unique_ptr<QNetworkAccessManager> m_pNetworkAccessManager;
    QNetworkReply* m_pReply;

    QByteArray m_downloadBuffer;
    /// Offset of the next unconsumed byte inside m_downloadBuffer.
    int m_bufferOffset;
    /// Absolute stream position of the byte at m_bufferOffset.
    qint64 m_bufferStartPosition;
    /// Position the running request has been started at.
    qint64 m_requestPosition;
    /// Absolute stream position of the next byte the server will send.
    qint64 m_nextDataPosition;
    /// Size of the stream in bytes, -1 if unknown.
    qint64 m_size;
    bool m_responseChecked;
    bool m_rangeRequestsSupported;
    bool m_replyFinished;
    bool m_reachedEnd;
    bool m_failed;
};

} // namespace mixxx
