#include "TalkSession.h"

#include "core/Log.h"

#include <QAudioSource>
#include <QCoreApplication>
#include <QMediaDevices>

#include <cmath>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace rl {

namespace {

AVSampleFormat toAv(QAudioFormat::SampleFormat f)
{
    switch (f) {
    case QAudioFormat::UInt8:
        return AV_SAMPLE_FMT_U8;
    case QAudioFormat::Int16:
        return AV_SAMPLE_FMT_S16;
    case QAudioFormat::Int32:
        return AV_SAMPLE_FMT_S32;
    case QAudioFormat::Float:
        return AV_SAMPLE_FMT_FLT;
    default:
        return AV_SAMPLE_FMT_NONE;
    }
}

} // namespace

TalkSession::TalkSession(BaichuanTalk::Params params, const QByteArray &microphoneId,
                         QObject *parent)
    : QObject(parent), m_params(std::move(params)), m_microphoneId(microphoneId)
{
}

TalkSession::~TalkSession()
{
    stopMicrophone();
    if (m_talk)
        m_talk->stop(); // the worker finishes on its own
}

void TalkSession::start()
{
    if (m_talk)
        return;
    m_state = State::Connecting;
    // The worker reports on its own thread; hop to ours. QPointer makes a
    // report that lands after this object is gone a no-op.
    QPointer<TalkSession> self(this);
    m_talk = BaichuanTalk::create(m_params, [self](BaichuanTalk::State s, const QString &error) {
        QMetaObject::invokeMethod(
            qApp,
            [self, s, error] {
                if (self)
                    self->onTalkState(static_cast<int>(s), error);
            },
            Qt::QueuedConnection);
    });
    m_talk->start();
    emit stateChanged(m_state, QString());
}

void TalkSession::stop()
{
    stopMicrophone();
    if (m_talk)
        m_talk->stop();
}

void TalkSession::onTalkState(int state, const QString &error)
{
    switch (static_cast<BaichuanTalk::State>(state)) {
    case BaichuanTalk::State::Connecting:
        return;
    case BaichuanTalk::State::Talking:
        if (!startMicrophone()) {
            m_talk->stop();
            m_state = State::Failed;
            emit stateChanged(m_state, tr("No microphone is available"));
            return;
        }
        m_state = State::Talking;
        break;
    case BaichuanTalk::State::Stopped:
        stopMicrophone();
        m_state = State::Stopped;
        break;
    case BaichuanTalk::State::Failed:
        stopMicrophone();
        m_state = State::Failed;
        break;
    }
    emit stateChanged(m_state, error);
}

bool TalkSession::startMicrophone()
{
    QAudioDevice device = QMediaDevices::defaultAudioInput();
    if (!m_microphoneId.isEmpty()) {
        for (const QAudioDevice &d : QMediaDevices::audioInputs())
            if (d.id() == m_microphoneId)
                device = d;
    }
    if (device.isNull())
        return false;
    // Ask for the camera's format directly; fall back to what the device
    // prefers and convert.
    QAudioFormat want;
    want.setSampleRate(BaichuanTalk::kSampleRate);
    want.setChannelCount(1);
    want.setSampleFormat(QAudioFormat::Int16);
    m_format = device.isFormatSupported(want) ? want : device.preferredFormat();
    const AVSampleFormat inFmt = toAv(m_format.sampleFormat());
    if (inFmt == AV_SAMPLE_FMT_NONE || m_format.channelCount() < 1)
        return false;

    AVChannelLayout inLayout;
    av_channel_layout_default(&inLayout, m_format.channelCount());
    const AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_MONO;
    swr_free(&m_swr);
    const bool ok = swr_alloc_set_opts2(&m_swr, &outLayout, AV_SAMPLE_FMT_S16,
                                        BaichuanTalk::kSampleRate, &inLayout, inFmt,
                                        m_format.sampleRate(), 0, nullptr) >= 0
        && swr_init(m_swr) >= 0;
    av_channel_layout_uninit(&inLayout);
    if (!ok) {
        swr_free(&m_swr);
        return false;
    }

    m_source = new QAudioSource(device, m_format, this);
    // Small buffer: talk latency matters more than a rare dropout.
    m_source->setBufferSize(m_format.bytesForDuration(40000));
    m_micIo = m_source->start();
    if (!m_micIo) {
        stopMicrophone();
        return false;
    }
    connect(m_micIo, &QIODevice::readyRead, this, &TalkSession::readMicrophone);
    qCInfo(lcMedia) << "talk: microphone" << device.description() << m_format.sampleRate()
                    << "Hz," << m_format.channelCount() << "ch";
    return true;
}

void TalkSession::stopMicrophone()
{
    if (m_level != 0) {
        m_level = 0;
        emit levelChanged(0);
    }
    if (m_source) {
        m_source->stop();
        delete m_source;
        m_source = nullptr;
    }
    m_micIo = nullptr;
    swr_free(&m_swr);
}

void TalkSession::readMicrophone()
{
    if (!m_micIo || !m_swr || !m_talk)
        return;
    const QByteArray in = m_micIo->readAll();
    const int frameBytes = m_format.bytesPerFrame();
    const int frames = frameBytes > 0 ? static_cast<int>(in.size() / frameBytes) : 0;
    if (frames <= 0)
        return;
    const int maxOut = swr_get_out_samples(m_swr, frames);
    if (maxOut <= 0)
        return;
    m_convBuf.resize(maxOut * 2);
    auto *dst = reinterpret_cast<uint8_t *>(m_convBuf.data());
    const auto *src = reinterpret_cast<const uint8_t *>(in.constData());
    const int got = swr_convert(m_swr, &dst, maxOut, &src, frames);
    if (got <= 0)
        return;
    const auto *pcm = reinterpret_cast<const qint16 *>(m_convBuf.constData());
    m_talk->pushPcm(pcm, got);

    // Level meter: the chunk's RMS on a -60..0 dBFS scale, falling back slowly
    // so speech doesn't flicker, and reported at most ~20 times a second.
    double sum = 0;
    for (int i = 0; i < got; ++i)
        sum += double(pcm[i]) * pcm[i];
    const double rms = std::sqrt(sum / got) / 32768.0;
    const qreal now = qBound(0.0, (20 * std::log10(std::max(rms, 1e-6)) + 60) / 60, 1.0);
    m_level = std::max(now, m_level * 0.85);
    if (!m_levelClock.isValid() || m_levelClock.elapsed() >= 50) {
        m_levelClock.start();
        emit levelChanged(m_level);
    }
}

} // namespace rl
