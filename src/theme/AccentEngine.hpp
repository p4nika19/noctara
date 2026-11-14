#pragma once

#include "theme/ColorScience.hpp"
#include "theme/Palette.hpp"

#include <QFutureWatcher>
#include <QImage>
#include <QObject>
#include <QString>
#include <QVariantList>

#include <cstdint>
#include <list>
#include <mutex>
#include <optional>
#include <vector>

namespace noctara::settings {
class SettingsStore;
}

namespace noctara::theme {

// "Palette extraction" (appearance.accent.extraction): how saturated the seed taken from artwork is.
enum class Extraction : std::uint8_t { Vivid, Balanced, Soft };
[[nodiscard]] Extraction parseExtraction(const QString& text) noexcept;

struct ArtworkAnalysis {
    // Palette seed, or nullopt when the artwork has no usable colour (grayscale, nearly black or white).
    std::optional<color::OkLch> seed;
    // The artwork is black and white, or nearly: too few coloured pixels, or no colour covering enough of it.
    // Such artwork gets neutralSeed() rather than a hue read from noise or a speck.
    bool achromatic = false;
    // Up to four dominant colours, most populous first (as found in the artwork, not adjusted).
    std::vector<color::OkLch> swatches;
};

// Picks the palette seed of an artwork. Pure and deterministic: the image is reduced to at most 64x64 and
// converted to OKLab. Only coloured pixels take part (chroma above a threshold that rises for dark pixels, where
// chroma is mostly compression noise); they are clustered with k-means (fixed, farthest-point initialisation) and
// the clusters that cover enough of the artwork are scored by population x chroma, with penalties for
// near-black, near-white and dull colours. A muted colour stays muted: the modes lift its chroma only by a
// bounded factor.
[[nodiscard]] ArtworkAnalysis analyseArtwork(const QImage& image, Extraction mode);
// The accent seed for black-and-white artwork: achromatic (the palette then uses its neutral hue).
[[nodiscard]] color::OkLch neutralSeed() noexcept;

// The artwork as a heavily blurred backdrop for one scheme: blurred at low resolution, its lightness remapped
// into the scheme's legible backdrop range (SchemeTones::backdropMinL..MaxL) and its chroma reduced (strongly
// for Frost: a pale wash). Returned at `size` x `size`, ready to be stretched over the window.
[[nodiscard]] QImage blurredBackdrop(const QImage& image, Scheme scheme, int size = 256);

// Derives accent seeds and the pre-blurred cover backdrop from the current artwork, off the GUI thread.
// Results are cached per artwork key (URL or content hash) in a small LRU and delivered with queued signals;
// stale results (the track changed meanwhile) are dropped. Exposed to QML as "Artwork".
class AccentEngine : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool hasArtwork READ hasArtwork NOTIFY artworkChanged)
    Q_PROPERTY(int revision READ revision NOTIFY artworkChanged)  // changes with every new backdrop image
    Q_PROPERTY(QVariantList swatches READ swatches NOTIFY artworkChanged)

public:
    explicit AccentEngine(settings::SettingsStore& settings, QObject* parent = nullptr);
    ~AccentEngine() override;

    // A null image clears the artwork.
    void setArtwork(const QString& key, const QImage& image);
    void clearArtwork();

    [[nodiscard]] bool hasArtwork() const noexcept { return m_hasArtwork; }
    [[nodiscard]] int revision() const noexcept { return m_revision; }
    [[nodiscard]] QVariantList swatches() const;
    // Thread-safe: called by the image provider on Qt Quick's loader threads.
    [[nodiscard]] QImage backdrop(Scheme scheme) const;

    static constexpr std::size_t kCacheEntries = 16;

Q_SIGNALS:
    // nullopt: no artwork (the theme falls back to the static accent). Black-and-white artwork sends
    // neutralSeed().
    void seedChanged(std::optional<noctara::color::OkLch> seed);
    void artworkChanged();

private:
    struct Result {
        std::uint64_t generation = 0;
        QString key;
        Extraction mode = Extraction::Vivid;
        ArtworkAnalysis analysis;
        QImage dark;
        QImage light;
    };

    void start();
    void apply(const Result& result);

    settings::SettingsStore& m_settings;
    QString m_key;
    QImage m_source;  // downscaled current artwork, kept for re-extraction when the mode changes
    std::uint64_t m_generation = 0;
    QFutureWatcher<Result> m_watcher;
    std::list<Result> m_cache;  // most recently used first

    bool m_hasArtwork = false;
    int m_revision = 0;
    std::vector<color::OkLch> m_swatches;
    mutable std::mutex m_imageMutex;
    QImage m_backdropDark;   // guarded by m_imageMutex
    QImage m_backdropLight;  // guarded by m_imageMutex
};

}  // namespace noctara::theme
