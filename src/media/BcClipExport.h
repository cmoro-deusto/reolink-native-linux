#pragma once

#include "protocol/BaichuanClient.h"

#include <QString>

#include <atomic>

// Clip export over native Baichuan recorded playback, for devices whose HTTP
// download endpoints are missing (Home Hub Pro v3.3.0.339 answers Download and
// Playback with 404). The device replays the recording faster than realtime;
// the frames are stream-copied (no re-encode) into an MP4 with their own
// timestamps — video plus AAC audio when the recording has it.
namespace rl {

// Export [params.startEpoch, endEpoch] to `outPath`. `params` must describe a
// recorded-playback session (startEpoch > 0); its callbacks are replaced.
// Blocks until the clip is complete, the stream ends, or `cancel` is set.
// Returns false with *error set when nothing usable was written.
bool exportBaichuanClip(BaichuanClient::Params params, qint64 endEpoch, const QString &outPath,
                        QString *error, const std::atomic<bool> *cancel = nullptr);

} // namespace rl
