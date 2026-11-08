#pragma once

#include <QMutex>
#include <QString>
#include <QWaitCondition>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace rl {

// Two-way talk to one camera over its own Baichuan connection (TCP 9000), the
// way Reolink's own clients do it (docs/research/talk.md):
//   login -> cmd 10 TalkAbility -> cmd 201 TalkConfig (speaker reserved)
//   -> cmd 202 audio, one IMA ADPCM block per message, at real time
//   -> cmd 11 TalkReset (speaker released)
// A busy speaker (201 answered 422) gets one TalkReset and one retry.
//
// Feed it 16 kHz mono PCM from any thread with pushPcm(); a block goes out as
// soon as 1024 samples are queued, so a live microphone paces it naturally.
// The network runs on a detached worker that holds its own reference, so
// stop() and destruction never block the caller; the worker finishes the
// session (flushes the last partial block, releases the speaker) on its own.
class BaichuanTalk : public std::enable_shared_from_this<BaichuanTalk>
{
public:
    static constexpr int kSampleRate = 16000;

    struct Params {
        QString host;
        int port = 9000;
        QString username;
        QString password;
        int channel = 0;
    };

    enum class State { Connecting, Talking, Stopped, Failed };
    // Called on the worker thread. `error` is set (user-facing) for Failed.
    using StateFn = std::function<void(State, const QString &error)>;

    static std::shared_ptr<BaichuanTalk> create(Params params, StateFn onState);
    ~BaichuanTalk();

    void start();
    // End the session: whatever audio is queued still goes out, then the
    // speaker is released. Safe to call more than once.
    void stop();

    // Queue 16 kHz mono samples (any thread). If the network falls behind,
    // the oldest queued audio is dropped so latency stays bounded.
    void pushPcm(const qint16 *samples, int count);

private:
    BaichuanTalk(Params params, StateFn onState);
    void run();
    void report(State state, const QString &error = QString());

    Params m_p;
    StateFn m_onState;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_started{false};

    QMutex m_mutex;
    QWaitCondition m_cond;
    std::vector<qint16> m_queue; // guarded by m_mutex
};

} // namespace rl
