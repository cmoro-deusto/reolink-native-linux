#include "AudioFeed.h"

#include "core/Log.h"

#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace rl {

namespace {

// Never queue more than this: a producer with nobody reading (sink not yet
// started, or stalled) must not grow the queue without bound.
constexpr qint64 kMaxBufferUs = 2000000;

qint64 usToBytes(qint64 us)
{
    return us * AudioFeed::kRate / 1000000 * AudioFeed::kBytesPerFrame;
}

} // namespace

AudioFeed::AudioFeed() = default;

AudioFeed::~AudioFeed()
{
    closeDecoder();
}

void AudioFeed::closeDecoder()
{
    avcodec_free_context(&m_dec);
    if (m_parser) {
        av_parser_close(m_parser);
        m_parser = nullptr;
    }
    swr_free(&m_swr);
    av_frame_free(&m_frame);
    av_packet_free(&m_pkt);
    m_swrInRate = 0;
    m_swrInFormat = -1;
    m_swrInChannels = 0;
}

void AudioFeed::setNotify(QObject *receiver)
{
    QMutexLocker lock(&m_mutex);
    m_notify = receiver;
    // Audio may have shown up before anyone was listening (a packet source
    // starts feeding before the player's session exists).
    if (m_notify && m_hasAudio.load())
        QMetaObject::invokeMethod(m_notify, "audioDetected", Qt::QueuedConnection);
}

void AudioFeed::markHasAudio()
{
    if (m_hasAudio.exchange(true))
        return;
    QMutexLocker lock(&m_mutex);
    if (m_notify)
        QMetaObject::invokeMethod(m_notify, "audioDetected", Qt::QueuedConnection);
}

void AudioFeed::setEnabled(bool on)
{
    m_enabled.store(on);
    if (!on) {
        QMutexLocker lock(&m_mutex);
        m_pcm.clear();
        m_priming = true;
    }
}

bool AudioFeed::openDecoder(const AVCodecParameters *par)
{
    closeDecoder();
    const AVCodec *codec = avcodec_find_decoder(par->codec_id);
    if (!codec) {
        qCInfo(lcMedia) << "live audio: no decoder for" << avcodec_get_name(par->codec_id);
        return false;
    }
    m_dec = avcodec_alloc_context3(codec);
    if (!m_dec || avcodec_parameters_to_context(m_dec, par) < 0 ||
        avcodec_open2(m_dec, codec, nullptr) < 0) {
        qCInfo(lcMedia) << "live audio: could not open" << codec->name;
        closeDecoder();
        return false;
    }
    m_frame = av_frame_alloc();
    m_pkt = av_packet_alloc();
    qCInfo(lcMedia) << "live audio:" << codec->name << par->sample_rate << "Hz,"
                    << par->ch_layout.nb_channels << "ch";
    markHasAudio();
    return true;
}

void AudioFeed::decodePacket(const AVPacket *packet)
{
    if (!m_dec || !m_enabled.load())
        return;
    if (avcodec_send_packet(m_dec, packet) < 0)
        return; // tolerate a damaged packet; the next one decodes on its own
    decodeFrames();
}

void AudioFeed::setTap(bool on)
{
    QMutexLocker lock(&m_mutex);
    m_tapOn = on;
    m_tap.clear();
}

std::vector<AudioFeed::TapFrame> AudioFeed::takeTap()
{
    QMutexLocker lock(&m_mutex);
    std::vector<TapFrame> out;
    out.swap(m_tap);
    return out;
}

bool AudioFeed::lastAdts(adts::Header *out) const
{
    QMutexLocker lock(&m_mutex);
    if (m_haveAdts)
        *out = m_lastAdts;
    return m_haveAdts;
}

void AudioFeed::pushAdts(const QByteArray &adtsData, qint64 videoUs)
{
    markHasAudio();
    {
        // Remember the format, and feed a recording, before the muted check.
        const auto *p = reinterpret_cast<const uchar *>(adtsData.constData());
        QMutexLocker lock(&m_mutex);
        adts::Header h;
        for (int off = 0; off < adtsData.size() && adts::parse(p + off, adtsData.size() - off, &h);
             off += h.frameLen) {
            m_lastAdts = h;
            m_haveAdts = true;
            if (m_tapOn && m_tap.size() < 1000) // bounded if nobody takes them
                m_tap.push_back({adtsData.mid(off + h.headerLen, h.frameLen - h.headerLen), h, videoUs});
        }
    }
    const QByteArray &adts = adtsData;
    if (!m_enabled.load() || adts.isEmpty())
        return;
    if (!m_dec) {
        const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
        m_parser = av_parser_init(AV_CODEC_ID_AAC);
        m_dec = codec ? avcodec_alloc_context3(codec) : nullptr;
        // No extradata: the AAC decoder configures itself from the ADTS headers.
        if (!codec || !m_parser || !m_dec || avcodec_open2(m_dec, codec, nullptr) < 0) {
            qCInfo(lcMedia) << "live audio: could not open the AAC decoder";
            closeDecoder();
            return;
        }
        m_frame = av_frame_alloc();
        m_pkt = av_packet_alloc();
    }
    const auto *in = reinterpret_cast<const uint8_t *>(adts.constData());
    int left = static_cast<int>(adts.size());
    while (left > 0) {
        uint8_t *out = nullptr;
        int outSize = 0;
        const int used = av_parser_parse2(m_parser, m_dec, &out, &outSize, in, left,
                                          AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (used < 0)
            return;
        in += used;
        left -= used;
        if (outSize > 0) {
            m_pkt->data = out;
            m_pkt->size = outSize;
            if (avcodec_send_packet(m_dec, m_pkt) >= 0)
                decodeFrames();
            m_pkt->data = nullptr;
            m_pkt->size = 0;
        }
        if (used == 0 && outSize == 0)
            break;
    }
}

void AudioFeed::decodeFrames()
{
    while (avcodec_receive_frame(m_dec, m_frame) == 0) {
        AVChannelLayout inLayout;
        if (m_frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
            av_channel_layout_default(&inLayout, m_frame->ch_layout.nb_channels);
        else
            av_channel_layout_copy(&inLayout, &m_frame->ch_layout);

        // (Re)build the resampler whenever the input format changes.
        if (!m_swr || m_swrInRate != m_frame->sample_rate || m_swrInFormat != m_frame->format ||
            m_swrInChannels != inLayout.nb_channels) {
            swr_free(&m_swr);
            const AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;
            if (swr_alloc_set_opts2(&m_swr, &outLayout, AV_SAMPLE_FMT_S16, kRate, &inLayout,
                                    static_cast<AVSampleFormat>(m_frame->format),
                                    m_frame->sample_rate, 0, nullptr) < 0 ||
                swr_init(m_swr) < 0) {
                swr_free(&m_swr);
                av_channel_layout_uninit(&inLayout);
                av_frame_unref(m_frame);
                continue;
            }
            m_swrInRate = m_frame->sample_rate;
            m_swrInFormat = m_frame->format;
            m_swrInChannels = inLayout.nb_channels;
        }
        av_channel_layout_uninit(&inLayout);

        const int maxOut = swr_get_out_samples(m_swr, m_frame->nb_samples);
        if (maxOut > 0) {
            m_convBuf.resize(maxOut * kBytesPerFrame);
            auto *dst = reinterpret_cast<uint8_t *>(m_convBuf.data());
            const int got = swr_convert(m_swr, &dst, maxOut,
                                        const_cast<const uint8_t **>(m_frame->extended_data),
                                        m_frame->nb_samples);
            if (got > 0)
                writePcm(m_convBuf.constData(), qint64(got) * kBytesPerFrame);
        }
        av_frame_unref(m_frame);
    }
}

qint64 AudioFeed::targetBytes() const
{
    const qint64 us = qMax(m_jitterUs, m_targetUs.load() - kSinkUs);
    return usToBytes(us);
}

qint64 AudioFeed::jitterUs() const
{
    QMutexLocker lock(&m_mutex);
    return m_jitterUs;
}

void AudioFeed::writePcm(const char *data, qint64 len)
{
    if (!m_enabled.load() || len <= 0)
        return;
    QMutexLocker lock(&m_mutex);
    m_pcm.append(data, len);
    const qint64 cap = usToBytes(kMaxBufferUs);
    if (m_pcm.size() > cap) {
        const qint64 drop = (m_pcm.size() - cap) / kBytesPerFrame * kBytesPerFrame;
        m_pcm.remove(0, drop);
    }
}

qint64 AudioFeed::bufferedBytes() const
{
    QMutexLocker lock(&m_mutex);
    return m_pcm.size();
}

qint64 AudioFeed::read(char *data, qint64 maxlen)
{
    maxlen -= maxlen % kBytesPerFrame;
    if (maxlen <= 0)
        return 0;
    QMutexLocker lock(&m_mutex);
    const qint64 target = targetBytes();
    if (m_priming && m_pcm.size() >= target)
        m_priming = false;
    qint64 n = 0;
    if (!m_priming) {
        // Running ahead (a delivery burst, or the target just shrank): drop the
        // oldest audio so latency stays near the target instead of growing.
        if (m_pcm.size() > target + usToBytes(kSlackUs)) {
            const qint64 drop = (m_pcm.size() - target) / kBytesPerFrame * kBytesPerFrame;
            m_pcm.remove(0, drop);
        }
        n = qMin<qint64>(maxlen, m_pcm.size());
        memcpy(data, m_pcm.constData(), static_cast<size_t>(n));
        m_pcm.remove(0, n);
        // Underrun: play out what there was, then wait for the buffer to refill
        // to a (now larger) target instead of crackling through a trickle.
        if (n < maxlen) {
            m_priming = true;
            m_jitterUs = qMin(kMaxJitterUs, m_jitterUs + kJitterStepUs);
            m_cleanBytes = 0;
            m_minFill = -1;
        } else {
            m_minFill = m_minFill < 0 ? m_pcm.size() : qMin<qint64>(m_minFill, m_pcm.size());
            if ((m_cleanBytes += n) >= usToBytes(kJitterRelaxUs)) {
                const qint64 spareUs = m_minFill / kBytesPerFrame * 1000000 / kRate;
                const qint64 margin = kJitterStepUs / 2;
                if (spareUs > margin)
                    m_jitterUs = qMax(kMinBufferUs, m_jitterUs - (spareUs - margin) / 2);
                m_cleanBytes = 0;
                m_minFill = -1;
            }
        }
    }
    if (n < maxlen)
        memset(data + n, 0, static_cast<size_t>(maxlen - n));
    return maxlen;
}

// ---- AudioFeedDevice ------------------------------------------------------

AudioFeedDevice::AudioFeedDevice(std::shared_ptr<AudioFeed> feed, QObject *parent)
    : QIODevice(parent), m_feed(std::move(feed))
{
}

qint64 AudioFeedDevice::bytesAvailable() const
{
    // Always "has data" (silence when the queue is empty) so the sink keeps
    // pulling and never parks itself in the idle state.
    return usToBytes(100000) + QIODevice::bytesAvailable();
}

qint64 AudioFeedDevice::readData(char *data, qint64 maxlen)
{
    return m_feed->read(data, maxlen);
}

} // namespace rl
