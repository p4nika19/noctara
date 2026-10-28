#pragma once

#include "core/Error.hpp"
#include "media/Decoder.hpp"

#include <QByteArray>
#include <QObject>

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

class QAudioSink;
class QMediaDevices;

namespace noctara::media {

// Fills the device buffer with interleaved float frames; called on the audio thread.
using RenderFunction = std::function<void(std::span<float>)>;

// The audio device. The render function runs on the implementation's audio thread.
class IAudioOutput {
public:
    IAudioOutput() = default;
    virtual ~IAudioOutput() = default;
    IAudioOutput(const IAudioOutput&) = delete;
    IAudioOutput& operator=(const IAudioOutput&) = delete;
    IAudioOutput(IAudioOutput&&) = delete;
    IAudioOutput& operator=(IAudioOutput&&) = delete;

    // The format the device would be opened with now; fails when there is no device.
    [[nodiscard]] virtual Result<AudioFormat> preferredFormat() = 0;
    // Opens the device with `format` (from preferredFormat()) and starts calling `render`.
    [[nodiscard]] virtual Result<void> start(AudioFormat format, RenderFunction render) = 0;
    virtual void suspend() = 0;
    virtual void resume() = 0;
    virtual void stop() = 0;
    [[nodiscard]] virtual bool running() const = 0;
    // Frames between what render() produced and what is heard.
    [[nodiscard]] virtual std::int64_t latencyFrames() const = 0;
    // Runs on the owner's thread when the device failed or the system's default device changed; the owner then
    // stops and starts again (possibly with another format).
    void setLostHandler(std::function<void()> handler) { m_lost = std::move(handler); }

protected:
    void lost() const
    {
        if (m_lost) {
            m_lost();
        }
    }

private:
    std::function<void()> m_lost;
};

// The system's default output through Qt Multimedia's pull callback. Follows the default device.
class QtAudioOutput : public QObject, public IAudioOutput {
    Q_OBJECT

public:
    explicit QtAudioOutput(QObject* parent = nullptr);
    ~QtAudioOutput() override;
    QtAudioOutput(const QtAudioOutput&) = delete;
    QtAudioOutput& operator=(const QtAudioOutput&) = delete;
    QtAudioOutput(QtAudioOutput&&) = delete;
    QtAudioOutput& operator=(QtAudioOutput&&) = delete;

    [[nodiscard]] Result<AudioFormat> preferredFormat() override;
    [[nodiscard]] Result<void> start(AudioFormat format, RenderFunction render) override;
    void suspend() override;
    void resume() override;
    void stop() override;
    [[nodiscard]] bool running() const override;
    [[nodiscard]] std::int64_t latencyFrames() const override;

    static constexpr int kBufferMs = 100;

private:
    void onDevicesChanged();

    std::unique_ptr<QMediaDevices> m_devices;  // created on first use: it talks to the sound server
    std::unique_ptr<QAudioSink> m_sink;
    QByteArray m_deviceId;
    AudioFormat m_format;
};

}  // namespace noctara::media
