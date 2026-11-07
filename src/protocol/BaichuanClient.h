#pragma once

#include "media/BcMediaParser.h"

#include <QByteArray>
#include <QMutex>
#include <QQueue>
#include <QString>
#include <QWaitCondition>

#include <atomic>
#include <functional>
#include <thread>

class QTcpSocket;

// Native Reolink "Baichuan" (BC, TCP 9000) client — the transport the official
// apps use, and the only one that streams the HEVC main stream in real time for
// both live and recorded playback (HTTP-FLV is H.264-only on NVR firmware and the
// cmd=Download endpoint is throttled below realtime).
//
// Runs its network loop on a detached worker thread and exposes a blocking read()
// that yields a raw H.264/H.265 Annex-B elementary stream, designed to back a
// libavformat AVIOContext so StreamPlayer's existing decode path is reused as-is.
//
// Clean-room: framing/crypto from the documented wire format + MIT reolink_aio /
// reolink-aio-ts; the recorded-playback command flow (search cmd 14 -> list cmd 15
// -> replay cmd 5, with cmd 93 keep-alive) reverse-engineered against an RLN8-410.
// No AGPL neolink code.
namespace rl {

class BaichuanClient
{
public:
    struct Params {
        QString host;
        int port = 9000;
        QString username;
        QString password;
        int channel = 0;
        QString uid;                    // camera UID (from GetChannelstatus)
        bool mainStream = true;
        qint64 startEpoch = 0;          // 0 => live preview; else recorded playback from here
        // Export mode: when onVideo is set, frames are handed to these callbacks
        // (on the worker thread) instead of being queued for read(), and recorded
        // playback runs at the device's full speed rather than paced to 1x.
        std::function<void(const BcMediaParser::VideoFrame &)> onVideo;
        std::function<void(const BcMediaParser::AudioFrame &)> onAudio;
    };

    explicit BaichuanClient(Params params);
    ~BaichuanClient();

    // Spawn the worker (connect + login + start the stream). Non-blocking.
    void start();
    // Signal the worker to stop and unblock any pending read(). Never joins on the
    // caller thread.
    void stop();

    // Seek an active recorded-playback session to a new instant, in place (reissues
    // the by-time command on the same connection — no reconnect/re-login). Returns
    // false if the session has already ended (caller should start a fresh one).
    bool seek(qint64 startEpoch);

    // Block up to `ms` for the worker to finish (connection closed, login or
    // stream failure, or stop()). Returns true once it has.
    bool waitFinished(int ms);

    // Blocking read of decoded Annex-B bytes (for an AVIOContext read callback).
    // Returns the number of bytes written, 0 on clean end, or <0 on abort/error.
    int read(unsigned char *buf, int size);

    // Device time (µs, monotonic across the 32-bit wrap) of the next frame that
    // read() has handed out or will hand out, in order: the raw elementary stream
    // carries no timestamps, so the player takes them from here for live
    // smoothing. False when none is queued.
    bool popFrameTime(qint64 *us);

    // "h264" / "hevc" once known (after the first frame); empty until then.
    QString ffmpegFormat();

private:
    void run();                                    // worker entry
    bool connectAndLogin(QTcpSocket &sock, QByteArray &aesKey);
    bool startStream(QTcpSocket &sock, const QByteArray &aesKey);
    void sendPlayback(QTcpSocket &sock, const QByteArray &aesKey, qint64 startEpoch);
    void pumpMedia(QTcpSocket &sock, const QByteArray &aesKey, quint16 streamMsgNum);
    void pushAnnexB(const QByteArray &bytes, quint32 micros);

    Params m_p;
    std::thread m_thread;
    std::atomic<bool> m_abort{false};

    QMutex m_mutex;
    QWaitCondition m_cond;
    QByteArray m_ring;        // decoded Annex-B awaiting the reader
    QQueue<qint64> m_frameTimes; // device time of each frame in m_ring/handed out
    quint32 m_lastMicros = 0;    // worker-only: unwraps the 32-bit µs clock
    qint64 m_microsTotal = -1;
    bool m_finished = false;  // worker done producing
    qint64 m_seekEpoch = 0;   // pending in-place seek target (0 = none)
    QString m_format;         // "h264"/"hevc", guarded by m_mutex
    bool m_started = false;
    QByteArray m_loginTail;   // socket bytes buffered past the login reply
    QByteArray m_streamTail;  // socket bytes buffered past the stream-start reply
};

} // namespace rl
