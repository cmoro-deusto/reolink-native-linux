#pragma once

#include <QByteArray>
#include <QtGlobal>

namespace rl::adts {

// One ADTS frame header (AAC as Reolink cameras send it over Baichuan).
struct Header {
    int objectType = 0;      // AAC object type (2 = LC)
    int sampleRateIndex = 0; // MPEG-4 sampling-frequency index
    int channels = 0;
    int headerLen = 0;       // 7, or 9 with a CRC
    int frameLen = 0;        // header + payload
};

// Parse the ADTS header at `p` (n bytes available). False if it isn't one, or
// the whole frame isn't there.
inline bool parse(const uchar *p, int n, Header *out)
{
    if (n < 7 || p[0] != 0xFF || (p[1] & 0xF6) != 0xF0)
        return false;
    out->headerLen = (p[1] & 1) ? 7 : 9; // protection_absent
    out->objectType = ((p[2] >> 6) & 3) + 1;
    out->sampleRateIndex = (p[2] >> 2) & 0xF;
    out->channels = ((p[2] & 1) << 2) | (p[3] >> 6);
    out->frameLen = ((p[3] & 3) << 11) | (p[4] << 3) | (p[5] >> 5);
    return out->sampleRateIndex < 13 && out->channels > 0 && out->frameLen > out->headerLen
           && out->frameLen <= n;
}

inline int sampleRate(int index)
{
    static const int rates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                  22050, 16000, 12000, 11025, 8000,  7350};
    return index >= 0 && index < 13 ? rates[index] : 0;
}

// The 2-byte AudioSpecificConfig an MP4 audio track needs:
// objectType(5) sampleRateIndex(4) channels(4) 000.
inline QByteArray audioSpecificConfig(const Header &h)
{
    QByteArray asc(2, '\0');
    asc[0] = char((h.objectType << 3) | (h.sampleRateIndex >> 1));
    asc[1] = char(((h.sampleRateIndex & 1) << 7) | (h.channels << 3));
    return asc;
}

} // namespace rl::adts
