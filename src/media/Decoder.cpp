#include "media/Decoder.hpp"

#include "core/Log.hpp"
#include "media/StreamSource.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>
}

#include <QByteArray>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace noctara::media {

namespace {

struct FormatDeleter {
    void operator()(AVFormatContext* context) const noexcept { avformat_close_input(&context); }
};
struct IoDeleter {
    void operator()(AVIOContext* io) const noexcept
    {
        av_freep(static_cast<void*>(&io->buffer));
        avio_context_free(&io);
    }
};
struct CodecDeleter {
    void operator()(AVCodecContext* context) const noexcept { avcodec_free_context(&context); }
};
struct ResamplerDeleter {
    void operator()(SwrContext* context) const noexcept { swr_free(&context); }
};
struct PacketDeleter {
    void operator()(AVPacket* packet) const noexcept { av_packet_free(&packet); }
};
struct FrameDeleter {
    void operator()(AVFrame* frame) const noexcept { av_frame_free(&frame); }
};

constexpr int kIoBufferSize = 32 * 1024;

std::string ffmpegError(int code)
{
    std::array<char, AV_ERROR_MAX_STRING_SIZE> text{};
    av_strerror(code, text.data(), text.size());
    return {text.data()};
}

__attribute__((format(printf, 3, 0))) void ffmpegLog(void* /*context*/, int level, const char* format, va_list args)
{
    if (level > AV_LOG_ERROR) {
        return;
    }
    std::array<char, 512> line{};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): FFmpeg hands over a printf-style va_list
    (void)std::vsnprintf(line.data(), line.size(), format, args);
    qCInfo(lcMedia).noquote() << "ffmpeg:" << QByteArray(line.data()).trimmed();
}

}  // namespace

struct Decoder::Impl {
    StreamSource& source;
    std::optional<Error> sourceError;  // why the last read through the source failed
    std::unique_ptr<AVIOContext, IoDeleter> io;
    std::unique_ptr<AVFormatContext, FormatDeleter> format;
    std::unique_ptr<AVCodecContext, CodecDeleter> codec;
    std::unique_ptr<SwrContext, ResamplerDeleter> resampler;
    std::unique_ptr<AVPacket, PacketDeleter> packet{av_packet_alloc()};
    std::unique_ptr<AVFrame, FrameDeleter> frame{av_frame_alloc()};
    int stream = -1;
    std::vector<float> scratch;
    bool draining = false;  // the demuxer is done; the codec is being flushed
    int badPackets = 0;

    explicit Impl(StreamSource& s)
        : source(s)
    {
    }

    static int read(void* opaque, std::uint8_t* buffer, int size)
    {
        auto* self = static_cast<Impl*>(opaque);
        const Result<std::size_t> got =
            self->source.read(std::span(reinterpret_cast<std::byte*>(buffer), static_cast<std::size_t>(size)));
        if (!got) {
            self->sourceError = got.error();
            return got.error().code == ErrorCode::Cancelled ? AVERROR_EXIT : AVERROR(EIO);
        }
        return *got == 0 ? AVERROR_EOF : static_cast<int>(*got);
    }

    static std::int64_t seek(void* opaque, std::int64_t offset, int whence)
    {
        auto* self = static_cast<Impl*>(opaque);
        const std::int64_t size = self->source.size();
        const auto flags = static_cast<unsigned>(whence);
        if ((flags & static_cast<unsigned>(AVSEEK_SIZE)) != 0U) {
            return size >= 0 ? size : AVERROR(ENOSYS);
        }
        std::int64_t target = offset;
        switch (static_cast<int>(flags & ~static_cast<unsigned>(AVSEEK_FORCE))) {
        case SEEK_SET: break;
        case SEEK_CUR: target += self->source.position(); break;
        case SEEK_END:
            if (size < 0) {
                return AVERROR(ENOSYS);
            }
            target += size;
            break;
        default: return AVERROR(EINVAL);
        }
        if (!self->source.seek(target)) {
            return AVERROR(EINVAL);
        }
        return target;
    }

    [[nodiscard]] Error failure(int code, const char* what) const
    {
        if (sourceError && (code == AVERROR_EXIT || code == AVERROR(EIO))) {
            return *sourceError;
        }
        return {ErrorCode::DecodeFailed, std::string(what) + ": " + ffmpegError(code)};
    }

    Result<void> openResampler(AudioFormat output)
    {
        AVChannelLayout in{};
        if (codec->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC || av_channel_layout_check(&codec->ch_layout) == 0) {
            av_channel_layout_default(&in, std::max(codec->ch_layout.nb_channels, 1));
        } else if (av_channel_layout_copy(&in, &codec->ch_layout) < 0) {
            return fail(ErrorCode::DecodeFailed, "channel layout");
        }
        AVChannelLayout out{};
        av_channel_layout_default(&out, output.channels);
        SwrContext* raw = nullptr;
        const int set = swr_alloc_set_opts2(&raw, &out, AV_SAMPLE_FMT_FLT, output.sampleRate, &in, codec->sample_fmt,
                                            codec->sample_rate, 0, nullptr);
        av_channel_layout_uninit(&in);
        av_channel_layout_uninit(&out);
        resampler.reset(raw);
        if (set < 0) {
            return std::unexpected(failure(set, "resampler options"));
        }
        if (const int init = swr_init(resampler.get()); init < 0) {
            return std::unexpected(failure(init, "resampler"));
        }
        return {};
    }
};

void installFfmpegLogging()
{
    static std::once_flag once;
    std::call_once(once, [] {
        av_log_set_level(AV_LOG_ERROR);
        av_log_set_callback(&ffmpegLog);
    });
}

Result<std::unique_ptr<Decoder>> Decoder::open(StreamSource& source, AudioFormat output, DecoderStart start)
{
    installFfmpegLogging();
    auto impl = std::make_unique<Impl>(source);
    if (!impl->packet || !impl->frame) {
        return fail(ErrorCode::DecodeFailed, "out of memory");
    }
    auto* buffer = static_cast<unsigned char*>(av_malloc(kIoBufferSize));
    if (buffer == nullptr) {
        return fail(ErrorCode::DecodeFailed, "out of memory");
    }
    AVIOContext* io = avio_alloc_context(buffer, kIoBufferSize, 0, impl.get(), &Impl::read, nullptr,
                                         source.seekable() ? &Impl::seek : nullptr);
    if (io == nullptr) {
        av_free(buffer);
        return fail(ErrorCode::DecodeFailed, "out of memory");
    }
    impl->io.reset(io);

    AVFormatContext* format = avformat_alloc_context();
    if (format == nullptr) {
        return fail(ErrorCode::DecodeFailed, "out of memory");
    }
    format->pb = io;
    format->flags =
        static_cast<int>(static_cast<unsigned>(format->flags) | static_cast<unsigned>(AVFMT_FLAG_CUSTOM_IO));
    // Keep the first audio quick: audio needs only a few packets to be understood.
    format->probesize = 256LL * 1024;
    format->max_analyze_duration = AV_TIME_BASE / 2;
    // avformat_open_input frees the context on failure.
    if (const int opened = avformat_open_input(&format, nullptr, nullptr, nullptr); opened < 0) {
        return std::unexpected(impl->failure(opened, "open"));
    }
    impl->format.reset(format);
    if (const int info = avformat_find_stream_info(format, nullptr); info < 0) {
        return std::unexpected(impl->failure(info, "stream info"));
    }
    const AVCodec* codec = nullptr;
    impl->stream = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (impl->stream < 0 || codec == nullptr) {
        return fail(ErrorCode::DecodeFailed, "no audio stream");
    }
    const AVStream* stream = format->streams[impl->stream];
    impl->codec.reset(avcodec_alloc_context3(codec));
    if (!impl->codec) {
        return fail(ErrorCode::DecodeFailed, "out of memory");
    }
    if (const int copied = avcodec_parameters_to_context(impl->codec.get(), stream->codecpar); copied < 0) {
        return std::unexpected(impl->failure(copied, "codec parameters"));
    }
    impl->codec->pkt_timebase = stream->time_base;
    if (const int opened = avcodec_open2(impl->codec.get(), codec, nullptr); opened < 0) {
        return std::unexpected(impl->failure(opened, "codec"));
    }
    if (auto resampler = impl->openResampler(output); !resampler) {
        return std::unexpected(resampler.error());
    }

    std::optional<std::int64_t> duration;
    if (format->duration > 0) {
        duration = av_rescale(format->duration, 1000, AV_TIME_BASE);
    }
    const bool segmented = !source.seekable();
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): the private constructor is only reachable from here
    std::unique_ptr<Decoder> decoder(new Decoder(std::move(impl), output, start.frame));
    decoder->m_trimTo = start.trimTo;
    decoder->m_usePts = !segmented;
    decoder->m_durationMs = duration;
    return decoder;
}

Decoder::Decoder(std::unique_ptr<Impl> impl, AudioFormat output, std::int64_t position)
    : m_impl(std::move(impl))
    , m_output(output)
    , m_position(position)
{
}

Decoder::~Decoder() = default;

void Decoder::append(std::vector<float>& out, const float* samples, std::int64_t frames, std::int64_t firstFrame)
{
    std::int64_t skip = 0;
    if (m_trimTo > firstFrame) {
        skip = std::min(frames, m_trimTo - firstFrame);
    }
    const auto channels = static_cast<std::size_t>(m_output.channels);
    if (frames > skip) {
        out.insert(out.end(), samples + (static_cast<std::size_t>(skip) * channels),
                   samples + (static_cast<std::size_t>(frames) * channels));
    }
    m_position = firstFrame + frames;
    if (m_position >= m_trimTo) {
        m_trimTo = 0;  // reached: everything from here on is delivered
    }
}

Result<bool> Decoder::decode(std::vector<float>& out)
{
    Impl& d = *m_impl;
    const std::size_t before = out.size();
    const auto channels = static_cast<std::size_t>(m_output.channels);
    while (out.size() == before) {
        const int received = avcodec_receive_frame(d.codec.get(), d.frame.get());
        if (received == 0) {
            const AVFrame& frame = *d.frame;
            std::int64_t first = m_position;
            if (m_usePts && frame.best_effort_timestamp != AV_NOPTS_VALUE && m_trimTo > 0) {
                const AVStream* stream = d.format->streams[d.stream];
                const std::int64_t origin = stream->start_time != AV_NOPTS_VALUE ? stream->start_time : 0;
                first = av_rescale_q(frame.best_effort_timestamp - origin, stream->time_base,
                                     AVRational{1, m_output.sampleRate});
            }
            const int capacity = swr_get_out_samples(d.resampler.get(), frame.nb_samples);
            if (capacity < 0) {
                return std::unexpected(d.failure(capacity, "resample"));
            }
            d.scratch.resize(static_cast<std::size_t>(capacity) * channels);
            auto* target = reinterpret_cast<std::uint8_t*>(d.scratch.data());
            const int converted =
                swr_convert(d.resampler.get(), &target, capacity, frame.extended_data, frame.nb_samples);
            av_frame_unref(d.frame.get());
            if (converted < 0) {
                return std::unexpected(d.failure(converted, "resample"));
            }
            append(out, d.scratch.data(), converted, first);
            d.badPackets = 0;
            continue;
        }
        if (received == AVERROR_EOF) {
            // Whatever the resampler still holds.
            const int capacity = std::max(swr_get_out_samples(d.resampler.get(), 0), 0);
            if (capacity > 0) {
                d.scratch.resize(static_cast<std::size_t>(capacity) * channels);
                auto* target = reinterpret_cast<std::uint8_t*>(d.scratch.data());
                const int converted = swr_convert(d.resampler.get(), &target, capacity, nullptr, 0);
                if (converted > 0) {
                    append(out, d.scratch.data(), converted, m_position);
                }
            }
            return out.size() > before;
        }
        if (received != AVERROR(EAGAIN)) {
            if (++d.badPackets > kMaxBadPackets) {
                return std::unexpected(d.failure(received, "decode"));
            }
            continue;
        }
        if (d.draining) {
            return out.size() > before;  // nothing more will come
        }
        const int read = av_read_frame(d.format.get(), d.packet.get());
        if (read == AVERROR_EOF || (read < 0 && read != AVERROR_EXIT && !d.sourceError)) {
            // The end of the stream (or a damaged tail): flush the codec.
            d.draining = true;
            (void)avcodec_send_packet(d.codec.get(), nullptr);
            continue;
        }
        if (read < 0) {
            return std::unexpected(d.failure(read, "read"));
        }
        if (d.packet->stream_index == d.stream) {
            const int sent = avcodec_send_packet(d.codec.get(), d.packet.get());
            if (sent < 0 && sent != AVERROR(EAGAIN) && ++d.badPackets > kMaxBadPackets) {
                av_packet_unref(d.packet.get());
                return std::unexpected(d.failure(sent, "decode"));
            }
        }
        av_packet_unref(d.packet.get());
    }
    return true;
}

Result<void> Decoder::seek(std::int64_t frame)
{
    Impl& d = *m_impl;
    if (!d.source.seekable()) {
        return fail(ErrorCode::Unsupported, "segmented source");
    }
    const AVStream* stream = d.format->streams[d.stream];
    const std::int64_t origin = stream->start_time != AV_NOPTS_VALUE ? stream->start_time : 0;
    const std::int64_t timestamp = origin + av_rescale_q(frame, AVRational{1, m_output.sampleRate}, stream->time_base);
    d.sourceError.reset();
    if (const int sought = av_seek_frame(d.format.get(), d.stream, timestamp, AVSEEK_FLAG_BACKWARD); sought < 0) {
        return std::unexpected(d.failure(sought, "seek"));
    }
    avcodec_flush_buffers(d.codec.get());
    // Drop what the resampler buffered from before the seek.
    if (auto resampler = d.openResampler(m_output); !resampler) {
        return resampler;
    }
    d.draining = false;
    d.badPackets = 0;
    m_position = frame;
    m_trimTo = frame;
    return {};
}

}  // namespace noctara::media
