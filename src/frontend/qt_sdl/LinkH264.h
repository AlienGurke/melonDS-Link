// Copyright melonDS-Link contributors
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

// The bottom screen as H.264 for the New 3DS (hardware decoder there): OpenH264,
// baseline profile, no B-frames, one slice, screen content mode. Only built when
// OpenH264 was found (LINK_HAVE_OPENH264); otherwise Available() is false.

#pragma once
// (the same as in Azahar-Link)

#include <cstdint>
#include <memory>
#include <vector>

namespace Link {

class H264Encoder {
public:
    H264Encoder();
    ~H264Encoder();

    /// Whether this build has an H.264 encoder at all.
    static bool Available();

    /// width/height of the coded picture (multiples of 16), bits per second.
    bool Open(int width, int height, int fps, int bitrate);
    void Close();
    bool IsOpen() const;
    void SetBitrate(int bitrate);

    /// I420 input (Y, then U, then V, no padding). `force_key`: start with SPS/PPS + IDR.
    /// `time_ms`: when the picture was taken (the rate control works with real time).
    /// `out`: Annex-B NAL units; empty when the encoder skipped the frame.
    bool Encode(const uint8_t* i420, bool force_key, int64_t time_ms, std::vector<uint8_t>& out,
                bool& is_key);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Link
