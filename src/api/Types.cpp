#include "api/Types.hpp"

#include <QRegularExpression>

#include <algorithm>
#include <array>
#include <utility>

namespace noctara::api {

QString Transcoding::cacheName() const
{
    return protocolName + u'/' + preset + u'/' + quality;
}

bool Track::drm() const
{
    const auto plainFull = [](const Transcoding& t) {
        return !t.encrypted && !t.snipped;
    };
    return std::ranges::any_of(transcodings, &Transcoding::encrypted) && std::ranges::none_of(transcodings, plainFull);
}

bool Track::previewOnly() const
{
    if (policy == Policy::Snip) {
        return true;
    }
    return !transcodings.empty() && std::ranges::all_of(transcodings, &Transcoding::snipped);
}

QString Playlist::artwork() const
{
    if (!artworkUrl.isEmpty()) {
        return artworkUrl;
    }
    for (const Track& track : tracks) {
        if (!track.stub && !track.artworkUrl.isEmpty()) {
            return track.artworkUrl;
        }
    }
    return user.avatarUrl;
}

Id itemId(const Item& item) noexcept
{
    return std::visit([](const auto& value) { return value.id; }, item);
}

QString sizedArtwork(const QString& url, int pixels)
{
    static const QRegularExpression suffix(
        QStringLiteral(R"(-(?:large|original|crop|mini|tiny|small|badge|t\d+x\d+)(\.(?:jpe?g|png))$)"));
    static constexpr std::array<std::pair<int, const char*>, 4> kSizes{{
        {67, "t67x67"},
        {100, "large"},
        {300, "t300x300"},
        {500, "t500x500"},
    }};
    const QRegularExpressionMatch match = suffix.match(url);
    if (!match.hasMatch()) {
        return url;
    }
    const auto* const size =
        std::ranges::find_if(kSizes, [pixels](const auto& entry) { return entry.first >= pixels; });
    const char* const name = size == kSizes.end() ? kSizes.back().second : size->second;
    QString sized = url;
    sized.replace(match.capturedStart(), match.capturedLength(), u'-' + QLatin1StringView(name) + match.captured(1));
    return sized;
}

}  // namespace noctara::api
