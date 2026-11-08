#include "media/AdpcmEncoder.h"
#include "protocol/BaichuanTalk.h"
#include "protocol/BcCrypto.h"

#include <QDeadlineTimer>
#include <QMutex>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtEndian>
#include <QtTest>

#include <cmath>
#include <vector>

using namespace rl;

namespace {

void putLE32(QByteArray &b, quint32 v)
{
    char t[4];
    qToLittleEndian(v, t);
    b.append(t, 4);
}

quint32 le32(const char *p)
{
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(p));
}

quint16 le16(const char *p)
{
    return qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(p));
}

// A modern-class (0x6414) reply from the fake device: 24-byte header, no Extension.
QByteArray reply(quint32 id, quint16 code, const QByteArray &body = {})
{
    QByteArray m;
    putLE32(m, 0x0ABCDEF0);
    putLE32(m, id);
    putLE32(m, static_cast<quint32>(body.size()));
    m.append(4, '\0'); // ch_id + mess_id
    m.append(char(code & 0xFF));
    m.append(char(code >> 8));
    m.append(char(0x14));
    m.append(char(0x64));
    putLE32(m, 0); // payload offset
    m.append(body);
    return m;
}

std::vector<qint16> tone(int samples, double hz = 440, double amplitude = 9000)
{
    std::vector<qint16> s(samples);
    for (int i = 0; i < samples; ++i)
        s[i] = static_cast<qint16>(amplitude * std::sin(2 * M_PI * hz * i / 16000.0));
    return s;
}

double snrDb(const qint16 *ref, const qint16 *got, int n)
{
    double sig = 0, err = 0;
    for (int i = 0; i < n; ++i) {
        sig += double(ref[i]) * ref[i];
        err += double(ref[i] - got[i]) * (ref[i] - got[i]);
    }
    return 10 * std::log10(sig / std::max(err, 1.0));
}

// The device side of a talk session, driven from the test thread.
struct FakeDevice {
    struct In {
        quint32 cmd = 0;
        quint8 chId = 0;
        QByteArray ext;     // decrypted Extension
        QByteArray payload; // as sent (talk audio is not encrypted)
    };

    QTcpServer server;
    QTcpSocket *conn = nullptr;
    QByteArray buf;
    const QByteArray key = bc::aesKey(QStringLiteral("abc"), QStringLiteral("pw"));

    bool accept()
    {
        if (!server.waitForNewConnection(5000))
            return false;
        conn = server.nextPendingConnection();
        return conn != nullptr;
    }

    bool read(In &m, int timeoutMs = 5000)
    {
        const QDeadlineTimer deadline(timeoutMs);
        for (;;) {
            if (buf.size() >= 20) {
                const quint32 len = le32(buf.constData() + 8);
                const quint16 cls = le16(buf.constData() + 18);
                const int hl = (cls == 0x6414 || cls == 0) ? 24 : 20;
                if (buf.size() >= hl + int(len)) {
                    m.cmd = le32(buf.constData() + 4);
                    m.chId = quint8(buf[12]);
                    const quint32 off = hl == 24 ? le32(buf.constData() + 20) : 0;
                    const QByteArray body = buf.mid(hl, len);
                    m.ext = off ? bc::aesCfb(body.left(off), key, true) : QByteArray();
                    m.payload = body.mid(off);
                    buf.remove(0, hl + len);
                    return true;
                }
            }
            if (deadline.hasExpired())
                return false;
            if (conn->waitForReadyRead(50))
                buf += conn->readAll();
        }
    }

    bool expect(quint32 cmd, In *out = nullptr)
    {
        In m;
        if (!read(m) || m.cmd != cmd) {
            qWarning() << "expected cmd" << cmd << "got" << m.cmd;
            return false;
        }
        if (out)
            *out = m;
        return true;
    }

    void send(const QByteArray &msg)
    {
        conn->write(msg);
        conn->flush();
    }

    // Hello -> nonce, login -> 200.
    bool login()
    {
        if (!accept() || !expect(1))
            return false;
        send(reply(1, 200, bc::xorCrypt("<body><Encryption><nonce>abc</nonce></Encryption></body>", 0)));
        if (!expect(1))
            return false;
        send(reply(1, 200));
        return true;
    }

    void sendAbility()
    {
        send(reply(10, 200,
                   bc::aesCfb("<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<body>\n<TalkAbility version=\"1.1\">\n"
                              "<duplexList><duplex>FDX</duplex></duplexList>\n<audioConfigList><audioConfig>\n"
                              "<priority>0</priority>\n<audioType>adpcm</audioType>\n<sampleRate>16000</sampleRate>\n"
                              "<samplePrecision>16</samplePrecision>\n<lengthPerEncoder>1024</lengthPerEncoder>\n"
                              "<soundTrack>mono</soundTrack>\n</audioConfig></audioConfigList>\n</TalkAbility>\n</body>\n",
                              key, false)));
    }
};

// Collects the worker's state reports (it calls from its own thread).
struct States {
    QMutex mutex;
    QList<BaichuanTalk::State> seen;
    QString error;

    BaichuanTalk::StateFn fn()
    {
        return [this](BaichuanTalk::State s, const QString &e) {
            QMutexLocker lock(&mutex);
            seen.append(s);
            if (!e.isEmpty())
                error = e;
        };
    }
    bool waitFor(BaichuanTalk::State s, int ms = 5000)
    {
        const QDeadlineTimer deadline(ms);
        while (!deadline.hasExpired()) {
            {
                QMutexLocker lock(&mutex);
                if (seen.contains(s))
                    return true;
            }
            QThread::msleep(10);
        }
        return false;
    }
};

std::shared_ptr<BaichuanTalk> startTalk(FakeDevice &dev, States &states, int channel = 2)
{
    BaichuanTalk::Params p;
    p.host = QStringLiteral("127.0.0.1");
    p.port = dev.server.serverPort();
    p.username = QStringLiteral("admin");
    p.password = QStringLiteral("pw");
    p.channel = channel;
    auto talk = BaichuanTalk::create(p, states.fn());
    talk->start();
    return talk;
}

} // namespace

class TestTalk : public QObject
{
    Q_OBJECT

private slots:
    // Encoder output decodes back to the input at the quality IMA ADPCM gives
    // (the real camera played this layout cleanly; see docs/research/talk.md).
    void encoderRoundTrip()
    {
        const auto in = tone(AdpcmEncoder::kSamplesPerBlock * 8);
        AdpcmEncoder enc;
        std::vector<qint16> out(in.size());
        for (size_t b = 0; b < in.size() / AdpcmEncoder::kSamplesPerBlock; ++b) {
            const QByteArray block = enc.encodeBlock(in.data() + b * AdpcmEncoder::kSamplesPerBlock);
            QCOMPARE(block.size(), AdpcmEncoder::kBlockBytes);
            QVERIFY(AdpcmEncoder::decodeBlock(block, out.data() + b * AdpcmEncoder::kSamplesPerBlock));
        }
        // Skip the first block: the step size starts tiny and has to ramp up.
        const int skip = AdpcmEncoder::kSamplesPerBlock;
        const double snr = snrDb(in.data() + skip, out.data() + skip, int(in.size()) - skip);
        QVERIFY2(snr > 28, qPrintable(QString::number(snr)));
    }

    // Each block's header is the decoder state carried in, so a block decodes
    // on its own and continues exactly where the previous one ended.
    void blockHeaderIsCarriedState()
    {
        const auto in = tone(AdpcmEncoder::kSamplesPerBlock * 2);
        AdpcmEncoder enc;
        const QByteArray b0 = enc.encodeBlock(in.data());
        const QByteArray b1 = enc.encodeBlock(in.data() + AdpcmEncoder::kSamplesPerBlock);
        std::vector<qint16> d0(AdpcmEncoder::kSamplesPerBlock);
        QVERIFY(AdpcmEncoder::decodeBlock(b0, d0.data()));
        const qint16 headerPredictor = qFromLittleEndian<qint16>(b1.constData());
        QCOMPARE(headerPredictor, d0.back());
        QCOMPARE(quint8(b0[0]), quint8(0)); // a fresh encoder starts from silence
        QCOMPARE(quint8(b0[2]), quint8(0));
        QCOMPARE(quint8(b1[3]), quint8(0));
    }

    // The first samples land in the HIGH nibble of the first byte.
    void highNibbleFirst()
    {
        std::vector<qint16> in(AdpcmEncoder::kSamplesPerBlock, 0);
        in[0] = 1000; // a jump on sample 0 only
        AdpcmEncoder enc;
        const QByteArray b = enc.encodeBlock(in.data());
        QVERIFY((quint8(b[4]) >> 4) != 0);   // sample 0 coded in the high nibble
        QVERIFY((quint8(b[4]) & 0x8) != 0);  // sample 1 steps back down (sign bit)
    }

    void mediaFrameLayout()
    {
        std::vector<qint16> in(AdpcmEncoder::kSamplesPerBlock, 0);
        AdpcmEncoder enc;
        const QByteArray block = enc.encodeBlock(in.data());
        const QByteArray f = AdpcmEncoder::mediaFrame(block);
        QCOMPARE(f.size(), AdpcmEncoder::kFrameBytes);
        QCOMPARE(f.left(4), QByteArray("01wb"));
        QCOMPARE(le16(f.constData() + 4), quint16(520));
        QCOMPARE(le16(f.constData() + 6), quint16(520));
        QCOMPARE(f.mid(8, 4), QByteArray("\x00\x01\x02\x00", 4));
        QCOMPARE(f.mid(12, AdpcmEncoder::kBlockBytes), block);
    }

    // The whole exchange: ability, config, audio messages (the partial last
    // block flushed with silence on stop), then the speaker released.
    void talkSession()
    {
        FakeDevice dev;
        QVERIFY(dev.server.listen(QHostAddress::LocalHost));
        States states;
        auto talk = startTalk(dev, states);
        QVERIFY(dev.login());

        FakeDevice::In m;
        QVERIFY(dev.expect(10, &m));
        QCOMPARE(m.chId, quint8(3)); // channel 2 + 1
        QVERIFY(m.ext.contains("<channelId>2</channelId>"));
        dev.sendAbility();

        QVERIFY(dev.expect(201, &m));
        const QByteArray cfg = bc::aesCfb(m.payload, dev.key, true);
        QVERIFY(cfg.contains("<TalkConfig version=\"1.1\">"));
        QVERIFY(cfg.contains("<audioType>adpcm</audioType>"));
        QVERIFY(cfg.contains("<lengthPerEncoder>1024</lengthPerEncoder>"));
        QVERIFY(!cfg.contains("<priority>"));
        dev.send(reply(201, 200));
        QVERIFY(states.waitFor(BaichuanTalk::State::Talking));

        const auto pcm = tone(2 * AdpcmEncoder::kSamplesPerBlock + 300);
        talk->pushPcm(pcm.data(), int(pcm.size()));
        for (int i = 0; i < 2; ++i) {
            QVERIFY(dev.expect(202, &m));
            QVERIFY(m.ext.contains("<binaryData>1</binaryData>"));
            QVERIFY(m.ext.contains("<channelId>2</channelId>"));
            QCOMPARE(m.payload.size(), AdpcmEncoder::kFrameBytes);
            QCOMPARE(m.payload.left(4), QByteArray("01wb")); // sent in the clear
            dev.send(reply(202, 200)); // the hub acknowledges each one
        }
        talk->stop();
        QVERIFY(dev.expect(202, &m)); // the 300 leftover samples, padded
        QCOMPARE(m.payload.size(), AdpcmEncoder::kFrameBytes);
        QVERIFY(dev.expect(11));
        dev.send(reply(11, 200));
        QVERIFY(states.waitFor(BaichuanTalk::State::Stopped));
    }

    // 422 on TalkConfig: release once, retry once.
    void busySpeakerIsResetOnce()
    {
        FakeDevice dev;
        QVERIFY(dev.server.listen(QHostAddress::LocalHost));
        States states;
        auto talk = startTalk(dev, states);
        QVERIFY(dev.login());
        QVERIFY(dev.expect(10));
        dev.sendAbility();
        QVERIFY(dev.expect(201));
        dev.send(reply(201, 422));
        QVERIFY(dev.expect(11));
        dev.send(reply(11, 200));
        QVERIFY(dev.expect(201));
        dev.send(reply(201, 200));
        QVERIFY(states.waitFor(BaichuanTalk::State::Talking));
        talk->stop();
        QVERIFY(dev.expect(11));
        dev.send(reply(11, 200));
        QVERIFY(states.waitFor(BaichuanTalk::State::Stopped));
    }

    void stillBusyFails()
    {
        FakeDevice dev;
        QVERIFY(dev.server.listen(QHostAddress::LocalHost));
        States states;
        auto talk = startTalk(dev, states);
        QVERIFY(dev.login());
        QVERIFY(dev.expect(10));
        dev.sendAbility();
        QVERIFY(dev.expect(201));
        dev.send(reply(201, 422));
        QVERIFY(dev.expect(11));
        dev.send(reply(11, 200));
        QVERIFY(dev.expect(201));
        dev.send(reply(201, 422));
        QVERIFY(states.waitFor(BaichuanTalk::State::Failed));
        QVERIFY(states.error.contains(QStringLiteral("Someone else")));
        QVERIFY(!states.seen.contains(BaichuanTalk::State::Talking));
    }

    void noTalkAbilityFails()
    {
        FakeDevice dev;
        QVERIFY(dev.server.listen(QHostAddress::LocalHost));
        States states;
        auto talk = startTalk(dev, states);
        QVERIFY(dev.login());
        QVERIFY(dev.expect(10));
        dev.send(reply(10, 400));
        QVERIFY(states.waitFor(BaichuanTalk::State::Failed));
        QVERIFY(!states.error.isEmpty());
    }

    void unreachableFails()
    {
        QTcpServer probe; // grab a free port, then close it
        QVERIFY(probe.listen(QHostAddress::LocalHost));
        const quint16 port = probe.serverPort();
        probe.close();
        States states;
        BaichuanTalk::Params p;
        p.host = QStringLiteral("127.0.0.1");
        p.port = port;
        auto talk = BaichuanTalk::create(p, states.fn());
        talk->start();
        QVERIFY(states.waitFor(BaichuanTalk::State::Failed, 10000));
    }
};

QTEST_GUILESS_MAIN(TestTalk)
#include "test_talk.moc"
