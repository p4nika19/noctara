#include "media/AudioOutput.hpp"

#include "core/Log.hpp"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QMediaDevices>

#include <algorithm>
#include <limits>

namespace noctara::media {

namespace {

QAudioFormat toQt(AudioFormat format, QAudioFormat::SampleFormat sample)
{
    QAudioFormat qt;
    qt.setSampleRate(format.sampleRate);
    qt.setChannelCount(format.channels);
    qt.setSampleFormat(sample);
    return qt;
}

}  // namespace

QtAudioOutput::QtAudioOutput(QObject* parent)
    : QObject(parent)
{
}

QtAudioOutput::~QtAudioOutput()
{
    stop();
}

Result<AudioFormat> QtAudioOutput::preferredFormat()
{
    if (!m_devices) {
        m_devices = std::make_unique<QMediaDevices>();
        connect(m_devices.get(), &QMediaDevices::audioOutputsChanged, this, &QtAudioOutput::onDevicesChanged);
    }
    const QAudioDevice device = QMediaDevices::defaultAudioOutput();
    if (device.isNull()) {
        return fail(ErrorCode::AudioOutputFailed, "no audio output device");
    }
    const QAudioFormat preferred = device.preferredFormat();
    AudioFormat format;
    format.sampleRate = preferred.sampleRate() > 0 ? preferred.sampleRate() : 48000;
    // Stereo at most: SoundCloud's streams are stereo, and the sound server mixes up to the speakers.
    format.channels = std::clamp(preferred.channelCount(), 1, 2);
    return format;
}

Result<void> QtAudioOutput::start(AudioFormat format, RenderFunction render)
{
    stop();
    const QAudioDevice device = QMediaDevices::defaultAudioOutput();
    if (device.isNull()) {
        return fail(ErrorCode::AudioOutputFailed, "no audio output device");
    }
    const bool floatSupported = device.isFormatSupported(toQt(format, QAudioFormat::Float));
    const QAudioFormat::SampleFormat sample = floatSupported ? QAudioFormat::Float : QAudioFormat::Int16;
    m_sink = std::make_unique<QAudioSink>(device, toQt(format, sample));
    m_sink->setBufferFrameCount(static_cast<qsizetype>(format.sampleRate) * kBufferMs / 1000);
    if (floatSupported) {
        m_sink->start(
            [render](QSpan<float> out) { render(std::span(out.data(), static_cast<std::size_t>(out.size()))); });
    } else {
        // Rendered as float, converted here; the scratch buffer only grows on the first callbacks.
        m_sink->start([render, scratch = std::vector<float>()](QSpan<std::int16_t> out) mutable {
            scratch.resize(static_cast<std::size_t>(out.size()));
            render(scratch);
            constexpr float scale = std::numeric_limits<std::int16_t>::max();
            for (qsizetype i = 0; i < out.size(); ++i) {
                out[i] =
                    static_cast<std::int16_t>(std::clamp(scratch[static_cast<std::size_t>(i)], -1.0F, 1.0F) * scale);
            }
        });
    }
    if (m_sink->error() != QtAudio::NoError) {
        const int error = m_sink->error();
        m_sink.reset();
        return fail(ErrorCode::AudioOutputFailed, "audio sink error " + std::to_string(error));
    }
    connect(m_sink.get(), &QAudioSink::stateChanged, this, [this] {
        if (m_sink && (m_sink->error() == QtAudio::IOError || m_sink->error() == QtAudio::FatalError)) {
            qCWarning(lcMedia) << "audio device failed:" << m_sink->error();
            lost();
        }
    });
    m_deviceId = device.id();
    m_format = format;
    qCDebug(lcMedia) << "audio output" << device.description() << format.sampleRate << "Hz" << format.channels
                     << "channels" << (floatSupported ? "float" : "int16");
    return {};
}

void QtAudioOutput::suspend()
{
    if (m_sink && m_sink->state() == QtAudio::ActiveState) {
        m_sink->suspend();
    }
}

void QtAudioOutput::resume()
{
    if (m_sink && m_sink->state() == QtAudio::SuspendedState) {
        m_sink->resume();
    }
}

void QtAudioOutput::stop()
{
    if (m_sink) {
        disconnect(m_sink.get(), nullptr, this, nullptr);
        m_sink->stop();
        m_sink.reset();
    }
}

bool QtAudioOutput::running() const
{
    return m_sink != nullptr;
}

std::int64_t QtAudioOutput::latencyFrames() const
{
    if (!m_sink) {
        return 0;
    }
    const qsizetype frames = m_sink->bufferFrameCount();
    return frames > 0 ? frames : static_cast<std::int64_t>(m_format.sampleRate) * kBufferMs / 1000;
}

void QtAudioOutput::onDevicesChanged()
{
    if (m_sink && QMediaDevices::defaultAudioOutput().id() != m_deviceId) {
        qCInfo(lcMedia) << "default audio output changed";
        lost();
    }
}

}  // namespace noctara::media
