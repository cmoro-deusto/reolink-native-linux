#include "AdpcmEncoder.h"

#include <QtEndian>

#include <algorithm>

namespace rl {

namespace {

// The standard IMA ADPCM tables.
constexpr int kStepTable[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,
    25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,
    88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,   253,   279,
    307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,   876,   963,
    1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,  3327,
    3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
constexpr int kIndexTable[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

// Apply one 4-bit code to the decoder state (shared by encoder and decoder so
// they can never disagree).
void applyCode(int code, int &predictor, int &index)
{
    const int step = kStepTable[index];
    int diff = step >> 3;
    if (code & 4)
        diff += step;
    if (code & 2)
        diff += step >> 1;
    if (code & 1)
        diff += step >> 2;
    predictor = std::clamp(predictor + ((code & 8) ? -diff : diff), -32768, 32767);
    index = std::clamp(index + kIndexTable[code], 0, 88);
}

} // namespace

void AdpcmEncoder::reset()
{
    m_predictor = 0;
    m_index = 0;
}

int AdpcmEncoder::encodeSample(int sample)
{
    int step = kStepTable[m_index];
    int delta = sample - m_predictor;
    int code = 0;
    if (delta < 0) {
        code = 8;
        delta = -delta;
    }
    if (delta >= step) {
        code |= 4;
        delta -= step;
    }
    step >>= 1;
    if (delta >= step) {
        code |= 2;
        delta -= step;
    }
    step >>= 1;
    if (delta >= step)
        code |= 1;
    applyCode(code, m_predictor, m_index);
    return code;
}

QByteArray AdpcmEncoder::encodeBlock(const qint16 *samples)
{
    QByteArray block(kBlockBytes, Qt::Uninitialized);
    auto *out = reinterpret_cast<uchar *>(block.data());
    qToLittleEndian<qint16>(static_cast<qint16>(m_predictor), out);
    out[2] = static_cast<uchar>(m_index);
    out[3] = 0;
    for (int i = 0; i < kSamplesPerBlock / 2; ++i) {
        const int first = encodeSample(samples[2 * i]);
        const int second = encodeSample(samples[2 * i + 1]);
        out[4 + i] = static_cast<uchar>((first << 4) | second);
    }
    return block;
}

QByteArray AdpcmEncoder::mediaFrame(const QByteArray &block)
{
    QByteArray f;
    f.reserve(kFrameBytes);
    f.append("01wb", 4);
    const quint16 size = static_cast<quint16>(block.size() + 4); // sub-header + block
    uchar le[2];
    qToLittleEndian(size, le);
    f.append(reinterpret_cast<const char *>(le), 2);
    f.append(reinterpret_cast<const char *>(le), 2);
    f.append("\x00\x01\x02\x00", 4);
    f.append(block);
    while (f.size() % 8)
        f.append('\0');
    return f;
}

bool AdpcmEncoder::decodeBlock(const QByteArray &block, qint16 *out)
{
    if (block.size() != kBlockBytes)
        return false;
    const auto *in = reinterpret_cast<const uchar *>(block.constData());
    int predictor = qFromLittleEndian<qint16>(in);
    int index = std::clamp<int>(in[2], 0, 88);
    for (int i = 0; i < kSamplesPerBlock / 2; ++i) {
        applyCode(in[4 + i] >> 4, predictor, index);
        out[2 * i] = static_cast<qint16>(predictor);
        applyCode(in[4 + i] & 0x0F, predictor, index);
        out[2 * i + 1] = static_cast<qint16>(predictor);
    }
    return true;
}

} // namespace rl
