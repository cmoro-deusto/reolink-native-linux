#include "media/AudioFeed.h"
#include "media/StreamPlayer.h"

#include <QAudioSink>
#include <QMediaDevices>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
}

using namespace rl;

namespace {

qint64 msToBytes(qint64 ms)
{
    return ms * AudioFeed::kRate / 1000 * AudioFeed::kBytesPerFrame;
}

// A tone at non-zero amplitude, so played audio is distinguishable from the
// silence the feed pads with.
QByteArray tone(qint64 ms)
{
    QByteArray b(msToBytes(ms), Qt::Uninitialized);
    auto *s = reinterpret_cast<qint16 *>(b.data());
    for (qint64 i = 0; i < b.size() / 2; ++i)
        s[i] = 1000;
    return b;
}

bool allZero(const QByteArray &b)
{
    return std::all_of(b.cbegin(), b.cend(), [](char c) { return c == 0; });
}

// Encode `seconds` of a 440 Hz mono tone at `rate` to ADTS-framed AAC, the way
// a Baichuan stream carries it.
QByteArray encodeAdts(int rate, double seconds)
{
    QByteArray result;
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec)
        return result;
    AVCodecContext *enc = avcodec_alloc_context3(codec);
    enc->sample_rate = rate;
    enc->sample_fmt = AV_SAMPLE_FMT_FLTP;
    const AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
    av_channel_layout_copy(&enc->ch_layout, &mono);
    enc->bit_rate = 64000;
    enc->time_base = {1, rate};
    if (avcodec_open2(enc, codec, nullptr) < 0) {
        avcodec_free_context(&enc);
        return result;
    }

    AVFormatContext *oc = nullptr;
    avformat_alloc_output_context2(&oc, nullptr, "adts", nullptr);
    AVStream *st = avformat_new_stream(oc, nullptr);
    avcodec_parameters_from_context(st->codecpar, enc);
    st->time_base = enc->time_base;
    avio_open_dyn_buf(&oc->pb);
    if (avformat_write_header(oc, nullptr) < 0)
        return result;

    AVFrame *f = av_frame_alloc();
    f->nb_samples = enc->frame_size;
    f->format = enc->sample_fmt;
    av_channel_layout_copy(&f->ch_layout, &enc->ch_layout);
    av_frame_get_buffer(f, 0);
    AVPacket *pkt = av_packet_alloc();
    const int total = int(rate * seconds);
    auto drain = [&] {
        while (avcodec_receive_packet(enc, pkt) == 0) {
            pkt->stream_index = 0;
            av_packet_rescale_ts(pkt, enc->time_base, st->time_base);
            av_write_frame(oc, pkt);
        }
    };
    for (int pos = 0; pos < total; pos += f->nb_samples) {
        av_frame_make_writable(f);
        auto *d = reinterpret_cast<float *>(f->data[0]);
        for (int i = 0; i < f->nb_samples; ++i)
            d[i] = 0.5f * std::sin(2 * M_PI * 440 * (pos + i) / rate);
        f->pts = pos;
        avcodec_send_frame(enc, f);
        drain();
    }
    avcodec_send_frame(enc, nullptr);
    drain();
    av_write_trailer(oc);

    uint8_t *buf = nullptr;
    const int size = avio_close_dyn_buf(oc->pb, &buf);
    result = QByteArray(reinterpret_cast<const char *>(buf), size);
    av_free(buf);
    oc->pb = nullptr;
    avformat_free_context(oc);
    av_packet_free(&pkt);
    av_frame_free(&f);
    avcodec_free_context(&enc);
    return result;
}

} // namespace

class TestAudio : public QObject
{
    Q_OBJECT

private slots:
    void silentWhileDisabled()
    {
        AudioFeed feed;
        feed.writePcm(tone(500).constData(), msToBytes(500));
        QCOMPARE(feed.bufferedBytes(), 0); // muted: nothing is queued
    }

    void primesToTargetThenPlays()
    {
        AudioFeed feed;
        feed.setEnabled(true);
        // Less than the minimum buffer: still priming, so silence comes out.
        feed.writePcm(tone(40).constData(), msToBytes(40));
        QByteArray out(msToBytes(10), Qt::Uninitialized);
        QCOMPARE(feed.read(out.data(), out.size()), out.size());
        QVERIFY(allZero(out));
        QCOMPARE(feed.bufferedBytes(), msToBytes(40));

        // Past the target: real audio flows.
        feed.writePcm(tone(40).constData(), msToBytes(40));
        feed.read(out.data(), out.size());
        QVERIFY(!allZero(out));
        QCOMPARE(feed.bufferedBytes(), msToBytes(70));
    }

    void underrunRebuffers()
    {
        AudioFeed feed;
        feed.setEnabled(true);
        feed.writePcm(tone(60).constData(), msToBytes(60));
        QByteArray out(msToBytes(100), Qt::Uninitialized);
        feed.read(out.data(), out.size()); // 60 ms of tone, 40 ms of silence
        QVERIFY(!allZero(out.left(msToBytes(60))));
        QVERIFY(allZero(out.mid(msToBytes(60))));
        // A trickle after the underrun is held until the target refills.
        feed.writePcm(tone(20).constData(), msToBytes(20));
        QByteArray small(msToBytes(10), Qt::Uninitialized);
        feed.read(small.data(), small.size());
        QVERIFY(allZero(small));
    }

    void runningAheadDropsOldest()
    {
        AudioFeed feed;
        feed.setEnabled(true);
        feed.setTargetLatencyUs(150000); // 150 ms video delay → 100 ms queue target
        feed.writePcm(tone(500).constData(), msToBytes(500));
        QByteArray out(msToBytes(10), Qt::Uninitialized);
        feed.read(out.data(), out.size());
        // Trimmed to the target, then 10 ms played.
        QCOMPARE(feed.bufferedBytes(), msToBytes(90));
    }

    // Smooth off (no video delay to follow) on a Home Hub camera: 64 ms AAC
    // frames arrive in bursts with ~230 ms gaps (measured on the Office cam).
    // A fixed 60 ms buffer ran dry every burst; the learned jitter must grow
    // until playback is continuous.
    void burstyArrivalWithoutSmoothing()
    {
        AudioFeed feed;
        feed.setEnabled(true);
        feed.setTargetLatencyUs(0);
        const QByteArray frame = tone(64);
        const int pattern[] = {10, 10, 10, 226}; // ms between arrivals; avg 64
        QByteArray out(msToBytes(10), Qt::Uninitialized);
        int nextArrival = 0, gapIdx = 0, lateUnderruns = 0;
        for (int t = 0; t < 30000; t += 10) { // simulated ms; sink pulls 10 ms
            while (nextArrival <= t) {
                feed.writePcm(frame.constData(), frame.size());
                nextArrival += pattern[gapIdx++ % 4];
            }
            feed.read(out.data(), out.size());
            if (t >= 10000 && allZero(out)) // after settling: any silence is a dropout
                ++lateUnderruns;
        }
        QCOMPARE(lateUnderruns, 0);
        QVERIFY(feed.jitterUs() > AudioFeed::kMinBufferUs);
        QVERIFY(feed.jitterUs() <= AudioFeed::kMaxJitterUs);
    }

    void muteClearsQueue()
    {
        AudioFeed feed;
        feed.setEnabled(true);
        feed.writePcm(tone(200).constData(), msToBytes(200));
        feed.setEnabled(false);
        QCOMPARE(feed.bufferedBytes(), 0);
    }

    void decodesAdtsAac()
    {
        // Reolink cameras commonly send 16 kHz mono AAC; out comes 48 kHz stereo.
        const QByteArray adts = encodeAdts(16000, 1.0);
        if (adts.isEmpty())
            QSKIP("FFmpeg built without an AAC encoder");
        AudioFeed feed;
        QVERIFY(!feed.hasAudio());
        feed.setEnabled(true);
        // Split at arbitrary points, as BC frames may cut across ADTS frames.
        for (int i = 0; i < adts.size(); i += 333)
            feed.pushAdts(adts.mid(i, 333));
        QVERIFY(feed.hasAudio());
        const qint64 got = feed.bufferedBytes();
        // ~1 s of output, minus decoder/parser delay at the edges.
        QVERIFY2(got > msToBytes(850) && got <= msToBytes(1100),
                 qPrintable(QStringLiteral("decoded %1 bytes").arg(got)));
    }

    void adtsAnnouncesAudioWhileMuted()
    {
        const QByteArray adts = encodeAdts(16000, 0.2);
        if (adts.isEmpty())
            QSKIP("FFmpeg built without an AAC encoder");
        AudioFeed feed; // disabled
        feed.pushAdts(adts);
        QVERIFY(feed.hasAudio()); // the speaker button can appear while muted
        QCOMPARE(feed.bufferedBytes(), 0);
    }

    // End to end through StreamPlayer: a file with video + AAC audio is demuxed,
    // the audio stream is announced, and unmuting opens a sink that plays it.
    void playerPlaysFileAudio()
    {
        if (QMediaDevices::defaultAudioOutput().isNull())
            QSKIP("no audio output device");
        QTemporaryDir dir;
        const QString file = dir.filePath(QStringLiteral("av.mp4"));
        QProcess ff;
        ff.start(QStringLiteral("ffmpeg"),
                 {"-v", "error", "-f", "lavfi", "-i", "testsrc=size=320x240:rate=15",
                  "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=16000", "-t", "4",
                  "-c:v", "libx264", "-c:a", "aac", "-shortest", file});
        if (!ff.waitForFinished(30000) || ff.exitCode() != 0)
            QSKIP("ffmpeg CLI with libx264 not available");

        StreamPlayer player;
        player.setSource(file);
        player.start();
        QTRY_VERIFY_WITH_TIMEOUT(player.hasAudio(), 5000);
        QVERIFY(!player.findChild<QAudioSink *>()); // muted: no sink

        player.setMuted(false);
        auto *sink = player.findChild<QAudioSink *>();
        QVERIFY(sink);
        QTRY_VERIFY_WITH_TIMEOUT(sink->processedUSecs() > 500000, 5000);
        QCOMPARE(sink->error(), QAudio::NoError);

        player.setMuted(true);
        QVERIFY(!player.findChild<QAudioSink *>());
        player.stop();
        QVERIFY(!player.hasAudio());
    }
};

QTEST_GUILESS_MAIN(TestAudio)
#include "test_audio.moc"
