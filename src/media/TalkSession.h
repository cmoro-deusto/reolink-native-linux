#pragma once

#include "protocol/BaichuanTalk.h"

#include <QAudioFormat>
#include <QObject>
#include <QPointer>

#include <memory>

class QAudioSource;
class QIODevice;
struct SwrContext;

namespace rl {

// One two-way talk session: the default microphone, captured with Qt
// Multimedia and converted to 16 kHz mono, streamed to a camera through
// BaichuanTalk. Lives on the GUI thread; state changes arrive as signals there.
//
// Microphone capture starts only once the camera has reserved its speaker, so
// nothing is recorded while connecting or after a refusal.
class TalkSession : public QObject
{
    Q_OBJECT
public:
    enum class State { Connecting, Talking, Stopped, Failed };
    Q_ENUM(State)

    explicit TalkSession(BaichuanTalk::Params params, QObject *parent = nullptr);
    ~TalkSession() override;

    void start();
    // Stop capturing and let the worker finish (last audio out, speaker
    // released). Never blocks.
    void stop();

    State state() const { return m_state; }

signals:
    // `error` is user-facing text, set for Failed.
    void stateChanged(rl::TalkSession::State state, const QString &error);

private slots:
    void onTalkState(int state, const QString &error);
    void readMicrophone();

private:
    bool startMicrophone();
    void stopMicrophone();

    BaichuanTalk::Params m_params;
    std::shared_ptr<BaichuanTalk> m_talk;
    State m_state = State::Stopped;

    QAudioSource *m_source = nullptr; // child
    QPointer<QIODevice> m_micIo;
    QAudioFormat m_format;
    SwrContext *m_swr = nullptr;
    QByteArray m_convBuf;
};

} // namespace rl
