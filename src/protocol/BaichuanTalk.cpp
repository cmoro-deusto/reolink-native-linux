#include "BaichuanTalk.h"

#include "core/Log.h"
#include "media/AdpcmEncoder.h"
#include "protocol/BaichuanControl.h"

#include <QCoreApplication>

#include <algorithm>
#include <thread>

namespace rl {

namespace {

constexpr quint32 kCmdTalkAbility = 10;
constexpr quint32 kCmdTalkReset = 11;
constexpr quint32 kCmdTalkConfig = 201;
constexpr quint32 kCmdTalk = 202;
constexpr quint16 kStatusOk = 200;
constexpr quint16 kStatusBusy = 422;
// Most audio held back before the oldest is dropped: a stalled network must
// not turn into seconds of delay once it recovers.
constexpr int kMaxQueuedSamples = BaichuanTalk::kSampleRate / 2;

QString tr(const char *text)
{
    return QCoreApplication::translate("BaichuanTalk", text);
}

QByteArray tagValue(const QByteArray &xml, const char *tag)
{
    const QByteArray open = QByteArray("<") + tag + '>';
    const int a = xml.indexOf(open);
    if (a < 0)
        return {};
    const int b = xml.indexOf("</", a + open.size());
    return b < 0 ? QByteArray() : xml.mid(a + open.size(), b - a - open.size()).trimmed();
}

QByteArray talkConfigXml(int channel)
{
    return QByteArrayLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<body>\n"
                             "<TalkConfig version=\"1.1\">\n<channelId>")
        + QByteArray::number(channel)
        + QByteArrayLiteral("</channelId>\n<duplex>FDX</duplex>\n"
                            "<audioStreamMode>followVideoStream</audioStreamMode>\n"
                            "<audioConfig>\n<audioType>adpcm</audioType>\n"
                            "<sampleRate>16000</sampleRate>\n<samplePrecision>16</samplePrecision>\n"
                            "<lengthPerEncoder>1024</lengthPerEncoder>\n<soundTrack>mono</soundTrack>\n"
                            "</audioConfig>\n</TalkConfig>\n</body>\n");
}

QByteArray audioExtension(int channel)
{
    return QByteArrayLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n"
                             "<Extension version=\"1.1\">\n<binaryData>1</binaryData>\n<channelId>")
        + QByteArray::number(channel) + QByteArrayLiteral("</channelId>\n</Extension>\n");
}

} // namespace

std::shared_ptr<BaichuanTalk> BaichuanTalk::create(Params params, StateFn onState)
{
    return std::shared_ptr<BaichuanTalk>(new BaichuanTalk(std::move(params), std::move(onState)));
}

BaichuanTalk::BaichuanTalk(Params params, StateFn onState)
    : m_p(std::move(params)), m_onState(std::move(onState))
{
}

BaichuanTalk::~BaichuanTalk() = default;

void BaichuanTalk::start()
{
    if (m_started.exchange(true))
        return;
    // The worker owns a reference: the session completes (speaker released)
    // even if the caller drops its handle right after stop().
    std::thread([self = shared_from_this()] { self->run(); }).detach();
}

void BaichuanTalk::stop()
{
    m_stop.store(true);
    QMutexLocker lock(&m_mutex);
    m_cond.wakeAll();
}

void BaichuanTalk::pushPcm(const qint16 *samples, int count)
{
    if (count <= 0 || m_stop.load())
        return;
    QMutexLocker lock(&m_mutex);
    m_queue.insert(m_queue.end(), samples, samples + count);
    if (m_queue.size() > size_t(kMaxQueuedSamples))
        m_queue.erase(m_queue.begin(), m_queue.end() - kMaxQueuedSamples);
    if (m_queue.size() >= size_t(AdpcmEncoder::kSamplesPerBlock))
        m_cond.wakeAll();
}

void BaichuanTalk::report(State state, const QString &error)
{
    if (m_onState)
        m_onState(state, error);
}

void BaichuanTalk::run()
{
    report(State::Connecting);
    BaichuanControl ctl({m_p.host, m_p.port, m_p.username, m_p.password});
    if (!ctl.open()) {
        report(State::Failed, tr("Couldn't connect to the camera for talk"));
        return;
    }

    // What the camera can play. Only the one format we encode is accepted.
    quint16 status = 0;
    const QByteArray ability = ctl.transact(kCmdTalkAbility, m_p.channel, {}, &status);
    if (status != kStatusOk) {
        report(State::Failed, tr("This camera doesn't offer two-way talk"));
        return;
    }
    if (tagValue(ability, "audioType") != "adpcm" || tagValue(ability, "sampleRate") != "16000"
        || tagValue(ability, "lengthPerEncoder") != "1024") {
        qCWarning(lcProto) << "talk: unsupported audio format" << tagValue(ability, "audioType")
                           << tagValue(ability, "sampleRate") << tagValue(ability, "lengthPerEncoder");
        report(State::Failed, tr("This camera's talk audio format isn't supported yet"));
        return;
    }

    // Reserve the speaker. 422 = busy (another client, or a talk session left
    // over from a crash): release it once and try again.
    const QByteArray config = talkConfigXml(m_p.channel);
    ctl.transact(kCmdTalkConfig, m_p.channel, config, &status);
    if (status == kStatusBusy) {
        quint16 resetStatus = 0;
        ctl.transact(kCmdTalkReset, m_p.channel, {}, &resetStatus);
        ctl.transact(kCmdTalkConfig, m_p.channel, config, &status);
    }
    if (status != kStatusOk) {
        report(State::Failed, status == kStatusBusy
                                  ? tr("Someone else is talking on this camera")
                                  : tr("The camera refused to start talk"));
        return;
    }
    if (m_stop.load()) { // stopped while connecting: release and leave
        ctl.transact(kCmdTalkReset, m_p.channel, {}, &status);
        report(State::Stopped);
        return;
    }
    report(State::Talking);
    qCInfo(lcProto) << m_p.host << "talk: speaker reserved on channel" << m_p.channel;

    AdpcmEncoder encoder;
    const QByteArray ext = audioExtension(m_p.channel);
    std::vector<qint16> block(AdpcmEncoder::kSamplesPerBlock);
    bool lost = false;
    while (!lost) {
        bool last = false;
        {
            QMutexLocker lock(&m_mutex);
            while (m_queue.size() < block.size() && !m_stop.load())
                m_cond.wait(&m_mutex, 200);
            if (m_queue.size() >= block.size()) {
                std::copy_n(m_queue.begin(), block.size(), block.begin());
                m_queue.erase(m_queue.begin(), m_queue.begin() + block.size());
            } else {
                // Stopping: send what's left, padded with silence, then finish.
                if (m_queue.empty())
                    break;
                std::fill(block.begin(), block.end(), 0);
                std::copy(m_queue.begin(), m_queue.end(), block.begin());
                m_queue.clear();
                last = true;
            }
        }
        const QByteArray frame = AdpcmEncoder::mediaFrame(encoder.encodeBlock(block.data()));
        if (!ctl.sendBinary(kCmdTalk, m_p.channel, ext, frame) || !ctl.drain())
            lost = true;
        if (last)
            break;
    }
    if (lost) {
        qCWarning(lcProto) << m_p.host << "talk: connection lost";
        report(State::Failed, tr("Lost the connection to the camera while talking"));
        return;
    }
    ctl.transact(kCmdTalkReset, m_p.channel, {}, &status);
    qCInfo(lcProto) << m_p.host << "talk: speaker released (" << status << ")";
    report(State::Stopped);
}

} // namespace rl
