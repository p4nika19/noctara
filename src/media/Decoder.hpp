#pragma once

#include "core/Error.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace noctara::media {

class StreamSource;

// What the audio device plays: interleaved 32-bit float frames.
struct AudioFormat {
    int sampleRate = 48000;
    int channels = 2;

    friend bool operator==(const AudioFormat&, const AudioFormat&) = default;
};

struct DecoderStart {
    std::int64_t frame = 0;   // stream frame (at the output rate) the source starts at (HLS: its first segment)
    std::int64_t trimTo = 0;  // frames before this one are decoded but dropped (accurate seeking)
};

// FFmpeg demuxing and decoding of one StreamSource, resampled to the output format. The demuxer reads only through
// the source (custom AVIOContext), never a URL. Used on one decoder thread; reads block on the source, and a stop
// request on the source's token makes the pending call fail with ErrorCode::Cancelled.
class Decoder {
public:
    [[nodiscard]] static Result<std::unique_ptr<Decoder>> open(StreamSource& source, AudioFormat output,
                                                               DecoderStart start = {});
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    Decoder(Decoder&&) = delete;
    Decoder& operator=(Decoder&&) = delete;

    // Appends at least one packet's worth of frames to `out`; false once the stream has ended (nothing appended).
    [[nodiscard]] Result<bool> decode(std::vector<float>& out);
    // Seekable sources only: the next frame decode() delivers is the one at `frame` (or the closest after it).
    [[nodiscard]] Result<void> seek(std::int64_t frame);
    // Stream frame of the next frame decode() delivers.
    [[nodiscard]] std::int64_t position() const noexcept { return m_position; }
    // As the container reports it; nullopt when unknown.
    [[nodiscard]] std::optional<std::int64_t> durationMs() const noexcept { return m_durationMs; }
    [[nodiscard]] AudioFormat output() const noexcept { return m_output; }

    // Recoverable errors in a row (damaged packets are skipped) before decoding gives up.
    static constexpr int kMaxBadPackets = 32;

private:
    struct Impl;
    explicit Decoder(std::unique_ptr<Impl> impl, AudioFormat output, std::int64_t position);
    void append(std::vector<float>& out, const float* samples, std::int64_t frames, std::int64_t firstFrame);

    std::unique_ptr<Impl> m_impl;
    AudioFormat m_output;
    std::int64_t m_position = 0;
    std::int64_t m_trimTo = 0;
    bool m_usePts = true;  // false for segmented sources, whose timestamps restart or are absolute per segment
    std::optional<std::int64_t> m_durationMs;
};

// Routes FFmpeg's own log output into noctara.media (errors only). Called once before the first decoder.
void installFfmpegLogging();

}  // namespace noctara::media
