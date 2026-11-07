#include "theme/AccentEngine.hpp"

#include "core/Log.hpp"
#include "core/Trace.hpp"
#include "settings/Keys.hpp"
#include "settings/SettingsStore.hpp"

#include <QColor>
#include <QElapsedTimer>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace noctara::theme {

namespace {

namespace keys = settings::keys;

constexpr int kAnalysisSize = 64;
constexpr int kBlurSize = 32;
constexpr int kBackdropSize = 256;  // blurredBackdrop's default size
constexpr int kSourceSize = 128;
constexpr int kClusters = 6;
constexpr int kIterations = 10;
constexpr double kMinSeedChroma = 0.035;

std::size_t pixelIndex(int x, int y, int width) noexcept
{
    return (static_cast<std::size_t>(y) * static_cast<std::size_t>(width)) + static_cast<std::size_t>(x);
}

// sRGB byte -> linear, shared by every pixel loop.
const std::array<double, 256>& linearTable()
{
    static const std::array<double, 256> table = [] {
        std::array<double, 256> values{};
        for (std::size_t i = 0; i < values.size(); ++i) {
            values[i] = color::srgbToLinear(static_cast<double>(i) / 255.0);
        }
        return values;
    }();
    return table;
}

std::vector<color::OkLab> labPixels(const QImage& image)
{
    const QImage argb = image.convertToFormat(QImage::Format_ARGB32);
    const auto& lut = linearTable();
    std::vector<color::OkLab> pixels;
    pixels.reserve(static_cast<std::size_t>(argb.width()) * static_cast<std::size_t>(argb.height()));
    for (int y = 0; y < argb.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(argb.constScanLine(y));  // NOLINT: Qt scan line
        for (int x = 0; x < argb.width(); ++x) {
            const QRgb px = line[x];  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            if (qAlpha(px) < 128) {
                continue;
            }
            pixels.push_back(color::toOkLab(color::LinearRgb{lut[static_cast<std::size_t>(qRed(px))],
                                                             lut[static_cast<std::size_t>(qGreen(px))],
                                                             lut[static_cast<std::size_t>(qBlue(px))]}));
        }
    }
    return pixels;
}

double distance2(const color::OkLab& p, const color::OkLab& q) noexcept
{
    const double dl = p.L - q.L;
    const double da = p.a - q.a;
    const double db = p.b - q.b;
    return dl * dl + da * da + db * db;
}

struct Cluster {
    color::OkLab centre;
    std::size_t count = 0;
};

// k-means with a deterministic farthest-point initialisation (no randomness: same image, same seed).
std::vector<Cluster> cluster(const std::vector<color::OkLab>& pixels)
{
    color::OkLab mean;
    for (const auto& p : pixels) {
        mean.L += p.L;
        mean.a += p.a;
        mean.b += p.b;
    }
    const auto n = static_cast<double>(pixels.size());
    mean = {mean.L / n, mean.a / n, mean.b / n};

    std::vector<Cluster> clusters{{mean, 0}};
    std::vector<double> nearest(pixels.size(), std::numeric_limits<double>::max());
    while (clusters.size() < static_cast<std::size_t>(kClusters)) {
        std::size_t farthest = 0;
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            nearest[i] = std::min(nearest[i], distance2(pixels[i], clusters.back().centre));
            if (nearest[i] > nearest[farthest]) {
                farthest = i;
            }
        }
        if (nearest[farthest] < 1e-6) {
            break;  // fewer distinct colours than clusters
        }
        clusters.push_back({pixels[farthest], 0});
    }

    std::vector<std::size_t> assignment(pixels.size(), 0);
    for (int iteration = 0; iteration < kIterations; ++iteration) {
        // Once no pixel changes cluster, the centres are a fixed point: later iterations would repeat this one.
        bool changed = iteration == 0;
        std::vector<color::OkLab> sums(clusters.size());
        for (auto& c : clusters) {
            c.count = 0;
        }
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            std::size_t best = 0;
            double bestDistance = std::numeric_limits<double>::max();
            for (std::size_t k = 0; k < clusters.size(); ++k) {
                const double d = distance2(pixels[i], clusters[k].centre);
                if (d < bestDistance) {
                    bestDistance = d;
                    best = k;
                }
            }
            changed = changed || assignment[i] != best;
            assignment[i] = best;
            ++clusters[best].count;
            sums[best].L += pixels[i].L;
            sums[best].a += pixels[i].a;
            sums[best].b += pixels[i].b;
        }
        for (std::size_t k = 0; k < clusters.size(); ++k) {
            if (clusters[k].count > 0) {
                const auto count = static_cast<double>(clusters[k].count);
                clusters[k].centre = {sums[k].L / count, sums[k].a / count, sums[k].b / count};
            }
        }
        if (!changed) {
            break;
        }
    }
    return clusters;
}

double lightnessPenalty(double lightness) noexcept
{
    if (lightness < 0.2 || lightness > 0.93) {
        return 0.15;
    }
    if (lightness < 0.3 || lightness > 0.86) {
        return 0.5;
    }
    return 1.0;
}

double dullnessPenalty(double chroma) noexcept
{
    if (chroma < 0.03) {
        return 0.05;
    }
    return chroma < 0.06 ? 0.4 : 1.0;
}

double seedChroma(double chroma, Extraction mode) noexcept
{
    switch (mode) {
    case Extraction::Vivid: return std::clamp(chroma * 1.3, 0.13, 0.26);
    case Extraction::Balanced: return std::clamp(chroma, 0.07, 0.15);
    case Extraction::Soft: return std::clamp(chroma * 0.55, 0.04, 0.085);
    }
    return chroma;
}

// Three box-blur passes (close to a Gaussian) over one channel, edges clamped.
void boxBlur(std::vector<double>& channel, int width, int height, int radius)
{
    std::vector<double> scratch(channel.size());
    const auto at = [width](int x, int y) {
        return pixelIndex(x, y, width);
    };
    const double scale = 1.0 / (2 * radius + 1);
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                double sum = 0.0;
                for (int d = -radius; d <= radius; ++d) {
                    sum += channel[at(std::clamp(x + d, 0, width - 1), y)];
                }
                scratch[at(x, y)] = sum * scale;
            }
        }
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                double sum = 0.0;
                for (int d = -radius; d <= radius; ++d) {
                    sum += scratch[at(x, std::clamp(y + d, 0, height - 1))];
                }
                channel[at(x, y)] = sum * scale;
            }
        }
    }
}

}  // namespace

Extraction parseExtraction(const QString& text) noexcept
{
    if (text == u"balanced") {
        return Extraction::Balanced;
    }
    return text == u"soft" ? Extraction::Soft : Extraction::Vivid;
}

ArtworkAnalysis analyseArtwork(const QImage& image, Extraction mode)
{
    NOCTARA_TRACE_SCOPE("accent: analyse artwork");
    ArtworkAnalysis analysis;
    if (image.isNull()) {
        return analysis;
    }
    const QImage small = image.width() > kAnalysisSize || image.height() > kAnalysisSize
                             ? image.scaled(kAnalysisSize, kAnalysisSize, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                             : image;
    const std::vector<color::OkLab> pixels = labPixels(small);
    if (pixels.empty()) {
        return analysis;
    }
    std::vector<Cluster> clusters = cluster(pixels);
    std::ranges::stable_sort(clusters, [](const Cluster& a, const Cluster& b) { return a.count > b.count; });

    const auto total = static_cast<double>(pixels.size());
    const Cluster* best = nullptr;
    double bestScore = -1.0;
    for (const Cluster& c : clusters) {
        if (c.count == 0) {
            continue;
        }
        const color::OkLch lch = color::toOkLch(c.centre);
        // A sub-linear population term lets a vivid minority colour beat a large dull area.
        const double score = std::pow(static_cast<double>(c.count) / total, 0.75) * (lch.C + 0.01) *
                             lightnessPenalty(lch.L) * dullnessPenalty(lch.C);
        if (score > bestScore) {
            bestScore = score;
            best = &c;
        }
        if (analysis.swatches.size() < 4) {
            analysis.swatches.push_back(lch);
        }
    }
    if (best == nullptr) {
        return analysis;
    }
    const color::OkLch chosen = color::toOkLch(best->centre);
    if (chosen.C < kMinSeedChroma) {
        return analysis;  // grayscale or nearly so: no meaningful hue
    }
    analysis.seed = color::OkLch{std::clamp(chosen.L, 0.5, 0.78), seedChroma(chosen.C, mode), chosen.h};
    return analysis;
}

namespace {

// The artwork in OKLab, blurred at very low resolution: shared by the two schemes' backdrops.
struct BlurredLab {
    std::vector<double> lightness;
    std::vector<double> a;
    std::vector<double> b;
};

BlurredLab blurLab(const QImage& image)
{
    // Cover the square (crop to the centre), then blur at very low resolution: the backdrop is soft anyway.
    const QImage covered = image.scaled(kBlurSize, kBlurSize, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    const QImage square =
        covered.copy((covered.width() - kBlurSize) / 2, (covered.height() - kBlurSize) / 2, kBlurSize, kBlurSize)
            .convertToFormat(QImage::Format_ARGB32);
    const auto& lut = linearTable();
    const std::size_t count = pixelIndex(0, kBlurSize, kBlurSize);
    std::vector<double> lightness(count);
    std::vector<double> a(count);
    std::vector<double> b(count);
    for (int y = 0; y < kBlurSize; ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(square.constScanLine(y));  // NOLINT: Qt scan line
        for (int x = 0; x < kBlurSize; ++x) {
            const QRgb px = line[x];  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            const color::OkLab lab = color::toOkLab(color::LinearRgb{lut[static_cast<std::size_t>(qRed(px))],
                                                                     lut[static_cast<std::size_t>(qGreen(px))],
                                                                     lut[static_cast<std::size_t>(qBlue(px))]});
            const std::size_t i = pixelIndex(x, y, kBlurSize);
            lightness[i] = lab.L;
            a[i] = lab.a;
            b[i] = lab.b;
        }
    }
    for (auto* channel : {&lightness, &a, &b}) {
        boxBlur(*channel, kBlurSize, kBlurSize, 3);
    }
    return {std::move(lightness), std::move(a), std::move(b)};
}

QImage remapBackdrop(const BlurredLab& lab, Scheme scheme, int size)
{
    // Remap into the scheme's legible backdrop range: contrast enforcement assumes exactly these bounds.
    const SchemeTones& tones = defaultTones(scheme);
    const double chromaScale = scheme == Scheme::Dark ? 0.85 : 0.3;
    QImage out(kBlurSize, kBlurSize, QImage::Format_RGB32);
    for (int y = 0; y < kBlurSize; ++y) {
        auto* line = reinterpret_cast<QRgb*>(out.scanLine(y));  // NOLINT: Qt scan line
        for (int x = 0; x < kBlurSize; ++x) {
            const std::size_t i = pixelIndex(x, y, kBlurSize);
            const double mapped =
                tones.backdropMinL + std::clamp(lab.lightness[i], 0.0, 1.0) * (tones.backdropMaxL - tones.backdropMinL);
            const color::OkLch lch =
                color::toOkLch(color::OkLab{mapped, lab.a[i] * chromaScale, lab.b[i] * chromaScale});
            const color::Srgb rgb = color::toSrgbGamutMapped(lch);
            line[x] = qRgb(static_cast<int>(std::lround(rgb.r * 255.0)),  // NOLINT: Qt scan line
                           static_cast<int>(std::lround(rgb.g * 255.0)), static_cast<int>(std::lround(rgb.b * 255.0)));
        }
    }
    return out.scaled(size, size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

}  // namespace

QImage blurredBackdrop(const QImage& image, Scheme scheme, int size)
{
    NOCTARA_TRACE_SCOPE("accent: blurred backdrop");
    if (image.isNull() || size <= 0) {
        return {};
    }
    return remapBackdrop(blurLab(image), scheme, size);
}

AccentEngine::AccentEngine(settings::SettingsStore& settings, QObject* parent)
    : QObject(parent)
    , m_settings(settings)
{
    connect(&m_watcher, &QFutureWatcher<Result>::finished, this, [this] {
        if (m_watcher.future().resultCount() > 0) {
            apply(m_watcher.result());
        }
    });
    connect(&m_settings, &settings::SettingsStore::valueChanged, this, [this](const QString& key) {
        if (key == QLatin1StringView(keys::AccentExtraction.data(),
                                     static_cast<qsizetype>(keys::AccentExtraction.size())) &&
            !m_source.isNull()) {
            start();
        }
    });
}

AccentEngine::~AccentEngine()
{
    // The worker only holds copies of its inputs, but finishing it keeps shutdown deterministic.
    m_watcher.waitForFinished();
}

void AccentEngine::setArtwork(const QString& key, const QImage& image)
{
    if (image.isNull()) {
        clearArtwork();
        return;
    }
    if (key == m_key && !m_source.isNull()) {
        return;
    }
    m_key = key;
    m_source = image.width() > kSourceSize || image.height() > kSourceSize
                   ? image.scaled(kSourceSize, kSourceSize, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                   : image;
    start();
}

void AccentEngine::clearArtwork()
{
    ++m_generation;
    m_key.clear();
    m_source = {};
    if (!m_hasArtwork) {
        return;
    }
    m_hasArtwork = false;
    m_swatches.clear();
    {
        const std::scoped_lock lock(m_imageMutex);
        m_backdropDark = {};
        m_backdropLight = {};
    }
    ++m_revision;
    Q_EMIT artworkChanged();
    Q_EMIT seedChanged(std::nullopt);
}

void AccentEngine::start()
{
    const Extraction mode = parseExtraction(m_settings.stringValue(keys::AccentExtraction));
    const std::uint64_t generation = ++m_generation;
    const auto cached =
        std::ranges::find_if(m_cache, [&](const Result& r) { return r.key == m_key && r.mode == mode; });
    if (cached != m_cache.end()) {
        m_cache.splice(m_cache.begin(), m_cache, cached);
        Result hit = m_cache.front();
        hit.generation = generation;
        apply(hit);
        return;
    }
    m_watcher.setFuture(QtConcurrent::run([generation, key = m_key, source = m_source, mode] {
        QElapsedTimer timer;
        timer.start();
        Result result;
        result.generation = generation;
        result.key = key;
        result.mode = mode;
        result.analysis = analyseArtwork(source, mode);
        if (!source.isNull()) {
            NOCTARA_TRACE_SCOPE("accent: blurred backdrop");
            const BlurredLab lab = blurLab(source);
            result.dark = remapBackdrop(lab, Scheme::Dark, kBackdropSize);
            result.light = remapBackdrop(lab, Scheme::Light, kBackdropSize);
        }
        qCDebug(lcTheme) << "artwork analysed in" << timer.nsecsElapsed() / 1000 << "us";
        return result;
    }));
}

void AccentEngine::apply(const Result& result)
{
    if (result.generation != m_generation) {
        return;  // the artwork or mode changed while this was computed
    }
    const auto existing =
        std::ranges::find_if(m_cache, [&](const Result& r) { return r.key == result.key && r.mode == result.mode; });
    if (existing == m_cache.end()) {
        m_cache.push_front(result);
        if (m_cache.size() > kCacheEntries) {
            m_cache.pop_back();
        }
    }
    {
        const std::scoped_lock lock(m_imageMutex);
        m_backdropDark = result.dark;
        m_backdropLight = result.light;
    }
    m_hasArtwork = true;
    m_swatches = result.analysis.swatches;
    ++m_revision;
    Q_EMIT artworkChanged();
    if (result.analysis.seed) {
        Q_EMIT seedChanged(result.analysis.seed);
    }
}

QVariantList AccentEngine::swatches() const
{
    QVariantList list;
    for (const color::OkLch& swatch : m_swatches) {
        const color::Srgb rgb = color::toSrgbGamutMapped(swatch);
        list.append(QColor::fromRgbF(static_cast<float>(rgb.r), static_cast<float>(rgb.g), static_cast<float>(rgb.b)));
    }
    return list;
}

QImage AccentEngine::backdrop(Scheme scheme) const
{
    const std::scoped_lock lock(m_imageMutex);
    return scheme == Scheme::Dark ? m_backdropDark : m_backdropLight;
}

}  // namespace noctara::theme
