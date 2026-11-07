#include "BcClipExport.h"

#include "core/Log.h"

#include <QElapsedTimer>
#include <QFile>
#include <QMutex>
#include <QVector>

#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
}

namespace rl {

namespace {

constexpr AVRational kMicros{1, 1000000};
// Frame interval to assume when the device's microsecond clock is missing or
// jumps (a replay crossing a recording-file boundary).
constexpr quint32 kFallbackFrameUs = 40000;
// How much video to hold back waiting for the first audio frame before the MP4
// header (which fixes the track list) is written without an audio track.
constexpr qint64 kAudioWaitUs = 1500000;

// The VPS/SPS/PPS NAL units of an Annex-B keyframe, each re-prefixed with a
// 4-byte start code: the form the MP4 muxer turns into avcC/hvcC. Other NAL
// types are left out (the hvcC writer rejects slices).
QByteArray parameterSets(const QByteArray &annexB, bool hevc)
{
    const auto *d = reinterpret_cast<const uchar *>(annexB.constData());
    const int n = annexB.size();
    QVector<int> starts; // offset of each NAL's first byte (after its start code)
    for (int i = 0; i + 3 <= n; ++i)
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            starts.append(i + 3);
            i += 2;
        }
    QByteArray out;
    for (int k = 0; k < starts.size(); ++k) {
        const int b = starts[k];
        int e = k + 1 < starts.size() ? starts[k + 1] - 3 : n;
        while (e > b && d[e - 1] == 0)
            --e; // a 4-byte start code's leading zero belongs to no NAL
        if (e <= b)
            continue;
        const int type = hevc ? (d[b] >> 1) & 0x3f : d[b] & 0x1f;
        const bool ps = hevc ? (type >= 32 && type <= 34) : (type == 7 || type == 8);
        if (ps)
            out.append(QByteArray::fromRawData("\x00\x00\x00\x01", 4)).append(annexB.mid(b, e - b));
    }
    return out;
}

struct Adts {
    int objectType = 0;
    int sampleRateIndex = 0;
    int channels = 0;
    int headerLen = 0;
    int frameLen = 0;
};

bool parseAdts(const uchar *p, int n, Adts *out)
{
    if (n < 7 || p[0] != 0xFF || (p[1] & 0xF6) != 0xF0)
        return false;
    out->headerLen = (p[1] & 1) ? 7 : 9; // protection_absent
    out->objectType = ((p[2] >> 6) & 3) + 1;
    out->sampleRateIndex = (p[2] >> 2) & 0xF;
    out->channels = ((p[2] & 1) << 2) | (p[3] >> 6);
    out->frameLen = ((p[3] & 3) << 11) | (p[4] << 3) | (p[5] >> 5);
    return out->sampleRateIndex < 13 && out->channels > 0 && out->frameLen > out->headerLen
           && out->frameLen <= n;
}

int sampleRate(int index)
{
    static const int rates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                  22050, 16000, 12000, 11025, 8000,  7350};
    return rates[index];
}

// Stream-copies BCMedia frames into an MP4. Video is H.264/H.265 Annex-B (the
// muxer converts it to length-prefixed using the parameter-set extradata);
// AAC arrives as ADTS and is unwrapped to raw frames plus an AudioSpecificConfig.
class Mp4Clip
{
public:
    explicit Mp4Clip(QString path) : m_path(std::move(path)) {}
    ~Mp4Clip() { close(); }

    qint64 videoUs() const { return m_videoUs; }
    int videoFrames() const { return m_videoFrames; }
    bool hasAudio() const { return m_audioIndex >= 0; }

    void addVideo(const BcMediaParser::VideoFrame &f)
    {
        if (m_failed)
            return;
        if (!m_videoStarted) {
            if (!f.keyFrame || f.codec == BcMediaParser::Codec::Unknown)
                return;
            m_hevc = f.codec == BcMediaParser::Codec::H265;
            m_extradata = parameterSets(f.annexB, m_hevc);
            m_width = f.width;
            m_height = f.height;
            m_lastMicros = f.microseconds;
            m_videoStarted = true;
        } else {
            quint32 d = f.microseconds - m_lastMicros; // unsigned: survives wrap
            m_lastMicros = f.microseconds;
            if (d == 0 || d > 2000000)
                d = kFallbackFrameUs;
            m_videoUs += d;
        }
        queue({true, f.keyFrame, m_videoUs, f.annexB});
    }

    void addAudio(const BcMediaParser::AudioFrame &a)
    {
        if (m_failed || m_noAudio || !m_videoStarted)
            return;
        if (a.codec != BcMediaParser::AudioCodec::AAC) {
            disableAudio("ADPCM audio isn't exported yet");
            return;
        }
        const auto *p = reinterpret_cast<const uchar *>(a.data.constData());
        int off = 0;
        while (off < a.data.size()) {
            Adts h;
            if (!parseAdts(p + off, a.data.size() - off, &h)) {
                if (off == 0)
                    disableAudio("audio isn't ADTS-framed AAC");
                return;
            }
            if (!m_haveAudioCfg) {
                m_adts = h;
                m_haveAudioCfg = true;
                // Anchor the audio clock to the video time it first appears at.
                m_audioSamples = av_rescale_q(m_videoUs, kMicros, {1, sampleRate(h.sampleRateIndex)});
            }
            queue({false, true, m_audioSamples,
                   a.data.mid(off + h.headerLen, h.frameLen - h.headerLen)});
            m_audioSamples += 1024; // AAC-LC frame
            off += h.frameLen;
        }
    }

    // Write anything still held back and close the file. False if no video made it.
    bool finish(QString *error)
    {
        if (!m_failed && !m_headerWritten && m_videoStarted)
            writeHeader();
        const bool ok = !m_failed && m_videoFrames > 0;
        close();
        if (!ok) {
            QFile::remove(m_path);
            if (error && error->isEmpty())
                *error = m_failed ? m_error : QStringLiteral("no video received");
        }
        return ok;
    }

private:
    struct Pending {
        bool video;
        bool key;
        qint64 ts; // video: microseconds; audio: samples
        QByteArray data;
    };

    void disableAudio(const char *why)
    {
        if (!m_noAudio)
            qCInfo(lcMedia) << "Clip export without audio:" << why;
        m_noAudio = true;
    }

    void queue(Pending p)
    {
        if (m_headerWritten) {
            write(p);
            return;
        }
        m_pending.append(std::move(p));
        if (m_haveAudioCfg || m_noAudio || m_videoUs >= kAudioWaitUs)
            writeHeader();
    }

    void fail(const QString &why)
    {
        m_failed = true;
        m_error = why;
        qCWarning(lcMedia) << "Clip export failed:" << why;
    }

    void writeHeader()
    {
        m_headerWritten = true;
        const QByteArray path = m_path.toUtf8();
        if (avformat_alloc_output_context2(&m_oc, nullptr, "mp4", path.constData()) < 0 || !m_oc)
            return fail(QStringLiteral("could not create the MP4 muxer"));

        AVStream *v = avformat_new_stream(m_oc, nullptr);
        if (!v)
            return fail(QStringLiteral("could not add the video track"));
        v->time_base = {1, 90000};
        AVCodecParameters *vp = v->codecpar;
        vp->codec_type = AVMEDIA_TYPE_VIDEO;
        vp->codec_id = m_hevc ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
        if (m_hevc)
            vp->codec_tag = MKTAG('h', 'v', 'c', '1'); // what Apple players expect
        vp->width = m_width;
        vp->height = m_height;
        setExtradata(vp, m_extradata);
        m_videoIndex = v->index;

        if (m_haveAudioCfg) {
            AVStream *a = avformat_new_stream(m_oc, nullptr);
            if (!a)
                return fail(QStringLiteral("could not add the audio track"));
            const int rate = sampleRate(m_adts.sampleRateIndex);
            a->time_base = {1, rate};
            AVCodecParameters *ap = a->codecpar;
            ap->codec_type = AVMEDIA_TYPE_AUDIO;
            ap->codec_id = AV_CODEC_ID_AAC;
            ap->sample_rate = rate;
            ap->frame_size = 1024;
            av_channel_layout_default(&ap->ch_layout, m_adts.channels);
            // AudioSpecificConfig: objectType(5) sampleRateIndex(4) channels(4) 000.
            const quint16 asc = static_cast<quint16>((m_adts.objectType << 11)
                                                     | (m_adts.sampleRateIndex << 7)
                                                     | (m_adts.channels << 3));
            const char ascBytes[2] = {char(asc >> 8), char(asc & 0xff)};
            setExtradata(ap, QByteArray(ascBytes, 2));
            m_audioIndex = a->index;
        }

        if (avio_open(&m_oc->pb, path.constData(), AVIO_FLAG_WRITE) < 0)
            return fail(QStringLiteral("could not open %1").arg(m_path));
        AVDictionary *opts = nullptr;
        av_dict_set(&opts, "movflags", "+faststart", 0);
        const int rc = avformat_write_header(m_oc, &opts);
        av_dict_free(&opts);
        if (rc < 0)
            return fail(QStringLiteral("could not write the MP4 header"));

        const QVector<Pending> held = std::exchange(m_pending, {});
        for (const Pending &p : held)
            write(p);
    }

    static void setExtradata(AVCodecParameters *par, const QByteArray &bytes)
    {
        if (bytes.isEmpty())
            return;
        par->extradata = static_cast<uint8_t *>(
            av_mallocz(static_cast<size_t>(bytes.size()) + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!par->extradata)
            return;
        memcpy(par->extradata, bytes.constData(), static_cast<size_t>(bytes.size()));
        par->extradata_size = bytes.size();
    }

    void write(const Pending &p)
    {
        if (m_failed || !m_oc)
            return;
        const int index = p.video ? m_videoIndex : m_audioIndex;
        if (index < 0)
            return; // audio that arrived for a file written without an audio track
        AVPacket *pkt = av_packet_alloc();
        if (!pkt || av_new_packet(pkt, static_cast<int>(p.data.size())) < 0) {
            av_packet_free(&pkt);
            return;
        }
        memcpy(pkt->data, p.data.constData(), static_cast<size_t>(p.data.size()));
        pkt->stream_index = index;
        const AVRational tb = m_oc->streams[index]->time_base;
        if (p.video) {
            qint64 ts = av_rescale_q(p.ts, kMicros, tb);
            if (ts <= m_lastVideoTs)
                ts = m_lastVideoTs + 1; // the muxer needs strictly increasing dts
            m_lastVideoTs = ts;
            pkt->pts = pkt->dts = ts; // Reolink streams carry no B-frames
            if (p.key)
                pkt->flags |= AV_PKT_FLAG_KEY;
            ++m_videoFrames;
        } else {
            const AVRational srcTb{1, sampleRate(m_adts.sampleRateIndex)};
            pkt->pts = pkt->dts = av_rescale_q(p.ts, srcTb, tb);
            pkt->duration = av_rescale_q(1024, srcTb, tb);
        }
        if (av_interleaved_write_frame(m_oc, pkt) < 0 && p.video)
            --m_videoFrames;
        av_packet_free(&pkt);
    }

    void close()
    {
        if (!m_oc)
            return;
        if (m_oc->pb) {
            if (!m_failed)
                av_write_trailer(m_oc);
            avio_closep(&m_oc->pb);
        }
        avformat_free_context(m_oc);
        m_oc = nullptr;
    }

    QString m_path;
    AVFormatContext *m_oc = nullptr;
    bool m_headerWritten = false;
    bool m_failed = false;
    QString m_error;
    QVector<Pending> m_pending;

    bool m_videoStarted = false;
    bool m_hevc = false;
    QByteArray m_extradata;
    int m_width = 0;
    int m_height = 0;
    quint32 m_lastMicros = 0;
    qint64 m_videoUs = 0;
    qint64 m_lastVideoTs = -1;
    int m_videoIndex = -1;
    int m_videoFrames = 0;

    bool m_noAudio = false;
    bool m_haveAudioCfg = false;
    Adts m_adts;
    qint64 m_audioSamples = 0;
    int m_audioIndex = -1;
};

} // namespace

bool exportBaichuanClip(BaichuanClient::Params params, qint64 endEpoch, const QString &outPath,
                        QString *error, const std::atomic<bool> *cancel)
{
    const qint64 durationUs = (endEpoch - params.startEpoch) * 1000000;
    if (params.startEpoch <= 0 || durationUs <= 0) {
        if (error)
            *error = QStringLiteral("invalid clip range");
        return false;
    }

    // The callbacks run on the client's worker thread; the client's destructor
    // joins that thread, so everything here outlives every callback.
    QMutex mutex;
    Mp4Clip clip(outPath);
    bool done = false;
    QElapsedTimer clock;
    qint64 lastFrameMs = 0;

    params.onVideo = [&](const BcMediaParser::VideoFrame &f) {
        QMutexLocker lock(&mutex);
        if (done)
            return;
        // Stop at the clip end: by the frame's wall clock when the device sends
        // one, else by elapsed stream time.
        if ((f.time != 0 && qint64(f.time) >= endEpoch)
            || (f.time == 0 && clip.videoUs() >= durationUs)) {
            done = true;
            return;
        }
        clip.addVideo(f);
        lastFrameMs = clock.elapsed();
    };
    params.onAudio = [&](const BcMediaParser::AudioFrame &a) {
        QMutexLocker lock(&mutex);
        if (!done)
            clip.addAudio(a);
    };

    auto client = std::make_unique<BaichuanClient>(params);
    clock.start();
    client->start();

    // Give up when the device sends nothing at all, goes quiet mid-clip (the
    // recording ended before the clip did), or the whole thing runs far longer
    // than a replay should.
    const qint64 overallMs = 60000 + 3 * durationUs / 1000;
    QString why;
    for (;;) {
        if (client->waitFinished(200))
            break;
        QMutexLocker lock(&mutex);
        if (done)
            break;
        const qint64 now = clock.elapsed();
        if (cancel && cancel->load()) {
            why = QStringLiteral("cancelled");
            break;
        }
        if (clip.videoFrames() == 0 && now > 20000) {
            why = QStringLiteral("the device sent no video for this moment");
            break;
        }
        if (clip.videoFrames() > 0 && now - lastFrameMs > 10000)
            break; // footage ended before the clip's end: keep what we have
        if (now > overallMs)
            break;
    }
    client->stop();
    client.reset(); // joins the worker: no callback runs past this point

    const qint64 elapsedMs = clock.elapsed();
    QString err = why;
    const bool ok = why.isEmpty() && clip.finish(&err);
    if (!ok) {
        if (!why.isEmpty())
            clip.finish(nullptr); // close and remove the partial file
        if (error)
            *error = err;
        return false;
    }
    qCInfo(lcMedia) << "Exported" << clip.videoUs() / 1000 << "ms of video"
                    << (clip.hasAudio() ? "with audio" : "without audio") << "in" << elapsedMs
                    << "ms to" << outPath;
    return true;
}

} // namespace rl
