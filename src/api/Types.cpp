#include "api/Types.hpp"

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
    return {};
}

bool Playlist::coverUnknown() const
{
    return artworkUrl.isEmpty() && trackCount > 0 &&
           std::ranges::none_of(tracks, [](const Track& track) { return !track.stub; });
}

Id itemId(const Item& item) noexcept
{
    return std::visit([](const auto& value) { return value.id; }, item);
}

QStringList splitTags(const QString& tagList)
{
    QStringList tags;
    QString current;
    bool quoted = false;
    const auto finish = [&] {
        if (const QString tag = current.trimmed(); !tag.isEmpty() && !tags.contains(tag)) {
            tags.append(tag);
        }
        current.clear();
    };
    for (const QChar c : tagList) {
        if (c == u'"') {
            finish();
            quoted = !quoted;
        } else if (c.isSpace() && !quoted) {
            finish();
        } else {
            current.append(c);
        }
    }
    finish();
    return tags;
}

namespace {

// "large", "t500x500" … : the size names SoundCloud uses before the artwork's extension.
bool isSizeToken(QStringView token)
{
    static constexpr std::array<std::u16string_view, 7> kNames{u"large", u"original", u"crop", u"mini",
                                                               u"tiny",  u"small",    u"badge"};
    if (std::ranges::find(kNames, std::u16string_view(token.utf16(), static_cast<std::size_t>(token.size()))) !=
        kNames.end()) {
        return true;
    }
    // t<digits>x<digits>, ASCII digits only.
    const auto digits = [](QStringView text) {
        return !text.isEmpty() && std::ranges::all_of(text, [](QChar c) { return c >= u'0' && c <= u'9'; });
    };
    if (token.size() < 4 || token.front() != u't') {
        return false;
    }
    const qsizetype x = token.indexOf(u'x', 1);
    return x > 0 && digits(token.sliced(1, x - 1)) && digits(token.sliced(x + 1));
}

}  // namespace

QString sizedArtwork(const QString& url, int pixels)
{
    static constexpr std::array<std::pair<int, const char*>, 4> kSizes{{
        {67, "t67x67"},
        {100, "large"},
        {300, "t300x300"},
        {500, "t500x500"},
    }};
    // The suffix -(large|original|crop|mini|tiny|small|badge|t<n>x<n>)(.jpg|.jpeg|.png) at the end, matched by hand:
    // this runs for every list row's artwork roles. Like the regular expression it replaced, the end may be followed
    // by one final newline, and invalid UTF-16 never matches.
    const QStringView view(url);
    const qsizetype end = view.endsWith(u'\n') ? view.size() - 1 : view.size();
    const QStringView body = view.first(end);
    qsizetype dot = -1;
    for (const QStringView extension : {QStringView(u".jpg"), QStringView(u".jpeg"), QStringView(u".png")}) {
        if (body.endsWith(extension)) {
            dot = end - extension.size();
            break;
        }
    }
    if (dot <= 0) {
        return url;
    }
    const qsizetype dash = body.first(dot).lastIndexOf(u'-');
    if (dash < 0 || !isSizeToken(body.sliced(dash + 1, dot - dash - 1)) || !view.isValidUtf16()) {
        return url;
    }
    const auto* const size =
        std::ranges::find_if(kSizes, [pixels](const auto& entry) { return entry.first >= pixels; });
    const char* const name = size == kSizes.end() ? kSizes.back().second : size->second;
    return url.first(dash) + u'-' + QLatin1StringView(name) + url.sliced(dot);
}

}  // namespace noctara::api
