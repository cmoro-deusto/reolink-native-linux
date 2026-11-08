#pragma once

#include "media/Adts.h"

#include <QByteArray>
#include <QIODevice>
#include <QMutex>
#include <QPointer>

#include <atomic>
#include <memory>
#include <vector>

struct AVCodecContext;
struct AVCodecParameters;
struct AVCodecParserContext;
struct AVFrame;
struct AVPacket;
struct SwrContext;

namespace rl {

// Live audio for one stream session: compressed audio in, PCM out to a
// QAudioSink. Decodes with FFmpeg and resamples to one fixed output format, so
// the sink never has to be reopened when a camera's rate or layout differs.
//
// Two producers feed it, one at a time per session:
//  - StreamPlayer's decode worker, with the demuxed audio stream of an RTSP/FLV
//    source (openDecoder + decodePacket);
//  - BaichuanClient's worker, with the ADTS-framed AAC that rides alongside a
//    native Baichuan video stream (pushAdts).
// The consumer is the sink's pull-mode QIODevice on the GUI thread (read()).
//
// Nothing is decoded while disabled (muted), so a grid of silent panes costs
// nothing. The PCM queue is a jitter buffer. Its target fill is the larger of
// the video playout delay (keeping sound roughly in step with the picture) and
// its own learned jitter: Home Hub cameras deliver audio in bursts (64 ms AAC
// frames up to ~330 ms apart, measured), so with Smooth off a fixed small
// buffer ran dry every burst. Each underrun grows the learned jitter; a long
// clean run gives some back. It rebuffers after an underrun and drops the
// oldest audio when it runs well ahead.
class AudioFeed
{
public:
    // Output format: 48 kHz stereo, signed 16-bit interleaved.
    static constexpr int kRate = 48000;
    static constexpr int kChannels = 2;
    static constexpr int kBytesPerFrame = kChannels * 2;
    // Least audio held before playing (absorbs ordinary delivery jitter), and how
    // far past the target the queue may run before the oldest audio is dropped.
    static constexpr qint64 kMinBufferUs = 60000;
    static constexpr qint64 kSlackUs = 120000;
    // Learned jitter: +kJitterStepUs per underrun up to kMaxJitterUs. After
    // kJitterRelaxUs of clean playback it gives back half the headroom the
    // buffer never touched in that window (the lowest fill seen, less a
    // margin), so shrinking can't itself cause a dropout.
    static constexpr qint64 kJitterStepUs = 40000;
    static constexpr qint64 kMaxJitterUs = 400000;
    static constexpr qint64 kJitterRelaxUs = 10000000;
    // Rough output latency of the sink itself, deducted from the target so the
    // total matches the video delay.
    static constexpr qint64 kSinkUs = 50000;

    AudioFeed();
    ~AudioFeed();
    AudioFeed(const AudioFeed &) = delete;
    AudioFeed &operator=(const AudioFeed &) = delete;

    // ---- Producer side (one thread at a time) ------------------------------
    // Set up a decoder for a demuxed audio stream. Returns false (and the stream
    // is ignored) when FFmpeg has no decoder for it. Announces audio either way
    // it succeeds, so the UI can offer the speaker button while still muted.
    bool openDecoder(const AVCodecParameters *par);
    // Decode one demuxed packet of the stream given to openDecoder().
    void decodePacket(const AVPacket *packet);
    // Baichuan AAC: one or more ADTS frames, split with FFmpeg's AAC parser.
    // videoUs: device time of the latest video frame when this arrived (-1 if
    // unknown), used to place the audio in a recording.
    void pushAdts(const QByteArray &adts, qint64 videoUs = -1);

    // ---- Recording tap (Baichuan AAC) ---------------------------------------
    // While on, every ADTS frame is also kept, header stripped, for the
    // recorder to take — muted or not. The demux path doesn't need it: the
    // recorder reads that audio from the demuxer itself.
    struct TapFrame {
        QByteArray aac;        // raw AAC frame (no ADTS header)
        adts::Header header;   // its ADTS parameters
        qint64 videoUs = -1;   // see pushAdts
    };
    void setTap(bool on);
    std::vector<TapFrame> takeTap();
    // Parameters of the last ADTS frame seen, so a recording can declare its
    // audio track before the first frame lands. False if none seen yet.
    bool lastAdts(adts::Header *out) const;

    // ---- Shared -------------------------------------------------------------
    void setEnabled(bool on);
    bool enabled() const { return m_enabled.load(); }
    bool hasAudio() const { return m_hasAudio.load(); }
    // Video playout delay to match (0 when smoothing is off).
    void setTargetLatencyUs(qint64 us) { m_targetUs.store(us); }
    // Object to tell, once, that this session carries audio: its
    // audioDetected() slot is invoked (queued) on its own thread. Clear it
    // before the object goes away.
    void setNotify(QObject *receiver);

    // ---- Consumer side ------------------------------------------------------
    // Fill exactly maxlen bytes (rounded down to whole frames), padding with
    // silence while priming or on underrun. Returns the bytes written.
    qint64 read(char *data, qint64 maxlen);

    // Append resampled PCM in the output format. Public for tests.
    void writePcm(const char *data, qint64 len);
    qint64 bufferedBytes() const;
    qint64 jitterUs() const;

private:
    void markHasAudio();
    void decodeFrames();
    void closeDecoder();
    qint64 targetBytes() const;

    std::atomic<bool> m_enabled{false};
    std::atomic<bool> m_hasAudio{false};
    std::atomic<qint64> m_targetUs{0};

    // Producer-only decoder state.
    AVCodecContext *m_dec = nullptr;
    AVCodecParserContext *m_parser = nullptr; // ADTS path only
    SwrContext *m_swr = nullptr;
    AVFrame *m_frame = nullptr;
    AVPacket *m_pkt = nullptr;
    int m_swrInRate = 0;
    int m_swrInFormat = -1;
    int m_swrInChannels = 0;
    QByteArray m_convBuf;

    mutable QMutex m_mutex; // guards the fields below
    QByteArray m_pcm;
    bool m_priming = true;
    bool m_tapOn = false;
    std::vector<TapFrame> m_tap;
    adts::Header m_lastAdts;
    bool m_haveAdts = false;
    qint64 m_jitterUs = kMinBufferUs;
    qint64 m_cleanBytes = 0; // played since the last underrun / relax
    qint64 m_minFill = -1;   // lowest queue level after a read in this window
    QPointer<QObject> m_notify;
};

// Pull-mode QIODevice handing a feed's PCM to a QAudioSink. Never runs dry: it
// returns silence rather than 0, so the sink doesn't go idle between bursts.
class AudioFeedDevice : public QIODevice
{
    Q_OBJECT
public:
    explicit AudioFeedDevice(std::shared_ptr<AudioFeed> feed, QObject *parent = nullptr);

    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override;

protected:
    qint64 readData(char *data, qint64 maxlen) override;
    qint64 writeData(const char *, qint64) override { return -1; }

private:
    std::shared_ptr<AudioFeed> m_feed;
};

} // namespace rl
