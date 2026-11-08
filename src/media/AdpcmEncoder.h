#pragma once

#include <QByteArray>
#include <QtGlobal>

namespace rl {

// IMA ADPCM encoder for Baichuan two-way talk (docs/research/talk.md).
//
// The camera advertises adpcm / 16 kHz / mono / lengthPerEncoder 1024 and plays
// blocks in this layout:
//   i16 LE predictor  - decoder state carried INTO the block (the last sample
//                       before it), not an extra output sample
//   u8 step index     - IMA step-table index for that predictor
//   u8 0
//   512 bytes         - 1024 4-bit codes, HIGH nibble first in each byte
// Blocks advance by exactly 1024 samples. The encoder keeps its state across
// blocks, so consecutive blocks join without a seam. Getting the nibble order
// or the header meaning wrong plays recognisably but rough, with a pop at every
// block boundary.
class AdpcmEncoder
{
public:
    static constexpr int kSamplesPerBlock = 1024;
    static constexpr int kBlockBytes = 4 + kSamplesPerBlock / 2; // 516
    static constexpr int kFrameBytes = 528; // "01wb" media frame around one block

    // Encode exactly kSamplesPerBlock samples into one block.
    QByteArray encodeBlock(const qint16 *samples);
    // Start over from silence (a new talk session).
    void reset();

    // Wrap a block as the "01wb" media frame a talk message carries:
    // "01wb", u16 520, u16 520, 00 01 02 00, then the block (8-byte aligned).
    static QByteArray mediaFrame(const QByteArray &block);

    // Reference decoder for the same layout: kSamplesPerBlock samples to `out`.
    // Returns false if `block` is not kBlockBytes long.
    static bool decodeBlock(const QByteArray &block, qint16 *out);

private:
    int encodeSample(int sample);

    int m_predictor = 0;
    int m_index = 0;
};

} // namespace rl
