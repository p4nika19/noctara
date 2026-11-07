#include "api/Parse.hpp"

#include "core/Trace.hpp"

#include <QTimeZone>

#include <format>
#include <optional>
#include <simdjson.h>
#include <string_view>

namespace noctara::api::parse {

namespace {

using simdjson::dom::element;
using simdjson::dom::element_type;

simdjson::dom::parser& parser()
{
    thread_local simdjson::dom::parser instance;
    return instance;
}

// The element is valid only until the thread's next parse.
Result<element> document(const QByteArray& json)
{
    // simdjson reads up to SIMDJSON_PADDING bytes past the end; a buffer with that much spare capacity is
    // parsed in place, anything else is copied once.
    const auto spare = static_cast<std::size_t>(json.capacity() - json.size());
    element root;
    if (const auto error =
            parser()
                .parse(json.constData(), static_cast<std::size_t>(json.size()), spare < simdjson::SIMDJSON_PADDING)
                .get(root);
        error != simdjson::SUCCESS) {
        return fail(ErrorCode::InvalidResponse, std::format("JSON: {}", simdjson::error_message(error)));
    }
    return root;
}

std::optional<element> field(element object, std::string_view key)
{
    element value;
    if (object[key].get(value) != simdjson::SUCCESS || value.is_null()) {
        return std::nullopt;
    }
    return value;
}

QString stringOf(element value)
{
    std::string_view string;
    if (value.get_string().get(string) == simdjson::SUCCESS) {
        return QString::fromUtf8(string.data(), static_cast<qsizetype>(string.size()));
    }
    return {};
}

QString text(element object, std::string_view key)
{
    const auto found = field(object, key);
    return found ? stringOf(*found) : QString();
}

std::optional<std::int64_t> integer(element value)
{
    switch (value.type()) {
    case element_type::INT64: return value.get_int64().value_unsafe();
    case element_type::UINT64: return static_cast<std::int64_t>(value.get_uint64().value_unsafe());
    case element_type::DOUBLE: return static_cast<std::int64_t>(value.get_double().value_unsafe());
    case element_type::STRING: {
        // Ids occasionally arrive as strings.
        bool ok = false;
        const std::string_view digits = value.get_string().value_unsafe();
        const qint64 parsed = QByteArrayView(digits.data(), static_cast<qsizetype>(digits.size())).toLongLong(&ok);
        return ok ? std::optional(parsed) : std::nullopt;
    }
    default: return std::nullopt;
    }
}

std::int64_t number(element object, std::string_view key, std::int64_t fallback = -1)
{
    const auto found = field(object, key);
    return found ? integer(*found).value_or(fallback) : fallback;
}

bool flag(element object, std::string_view key)
{
    bool value = false;
    const auto found = field(object, key);
    return found && found->get_bool().get(value) == simdjson::SUCCESS && value;
}

QString link(element object, std::string_view key)
{
    return text(object, key);
}

// SoundCloud's usual "2021-03-04T05:06:07Z" (optionally with milliseconds), without QDateTime's general parser,
// which dominated the cost of parsing a page.
std::optional<QDateTime> isoUtc(std::string_view text)
{
    const auto digits = [&](std::size_t at, std::size_t count) -> int {
        int value = 0;
        for (std::size_t i = at; i < at + count; ++i) {
            if (i >= text.size() || text[i] < '0' || text[i] > '9') {
                return -1;
            }
            value = value * 10 + (text[i] - '0');
        }
        return value;
    };
    if (text.size() < 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' || text[16] != ':' ||
        text.back() != 'Z') {
        return std::nullopt;
    }
    int millis = 0;
    if (text.size() == 24 && text[19] == '.') {
        millis = digits(20, 3);
    } else if (text.size() != 20) {
        return std::nullopt;
    }
    const QDate date(digits(0, 4), digits(5, 2), digits(8, 2));
    const QTime time(digits(11, 2), digits(14, 2), digits(17, 2), millis);
    if (!date.isValid() || !time.isValid()) {
        return std::nullopt;
    }
    return QDateTime(date, time, QTimeZone::UTC);
}

QDateTime timestamp(element object, std::string_view key)
{
    const auto found = field(object, key);
    if (!found) {
        return {};
    }
    if (found->type() != element_type::STRING) {
        // play-history uses milliseconds since the epoch
        const auto millis = integer(*found);
        return millis ? QDateTime::fromMSecsSinceEpoch(*millis, QTimeZone::UTC) : QDateTime();
    }
    std::string_view raw;
    (void)found->get_string().get(raw);
    if (auto fast = isoUtc(raw)) {
        return *fast;
    }
    const QString value = QString::fromUtf8(raw.data(), static_cast<qsizetype>(raw.size()));
    QDateTime parsed = QDateTime::fromString(value, Qt::ISODateWithMs);
    if (!parsed.isValid()) {
        // older objects: "2014/05/12 09:34:12 +0000"
        parsed = QDateTime::fromString(value.left(19), QStringLiteral("yyyy/MM/dd HH:mm:ss"));
        parsed.setTimeZone(QTimeZone::UTC);
    }
    return parsed;
}

std::string_view kind(element object)
{
    std::string_view value;
    if (const auto found = field(object, "kind"); found && found->get_string().get(value) == simdjson::SUCCESS) {
        return value;
    }
    return {};
}

Result<Id> requiredId(element object)
{
    if (const auto found = field(object, "id")) {
        if (const auto id = integer(*found); id && *id > 0) {
            return *id;
        }
    }
    return fail(ErrorCode::InvalidResponse, "object without a numeric id");
}

UserRef userRef(element object)
{
    UserRef user;
    user.id = number(object, "id", 0);
    user.username = text(object, "username");
    user.permalink = text(object, "permalink");
    user.avatarUrl = link(object, "avatar_url");
    user.verified = flag(object, "verified");
    if (const auto badges = field(object, "badges")) {
        user.verified = user.verified || flag(*badges, "verified");
    }
    return user;
}

Policy policy(const QString& value)
{
    if (value == u"SNIP") {
        return Policy::Snip;
    }
    if (value == u"BLOCK") {
        return Policy::Block;
    }
    if (value == u"MONETIZE") {
        return Policy::Monetize;
    }
    return Policy::Allow;
}

std::optional<Transcoding> transcoding(element object)
{
    Transcoding result;
    result.url = link(object, "url");
    result.preset = text(object, "preset");
    result.quality = text(object, "quality");
    result.durationMs = number(object, "duration", 0);
    result.snipped = flag(object, "snipped");
    if (const auto format = field(object, "format")) {
        result.protocolName = text(*format, "protocol");
        result.mimeType = text(*format, "mime_type");
    }
    if (result.url.isEmpty() || result.protocolName.isEmpty()) {
        return std::nullopt;
    }
    result.encrypted = result.protocolName.contains(u"encrypted");
    result.protocol =
        result.protocolName == u"progressive" ? cache::StreamProtocol::Progressive : cache::StreamProtocol::Hls;
    return result;
}

Result<Track> trackFrom(element object)
{
    Track track;
    Result<Id> id = requiredId(object);
    if (!id) {
        return std::unexpected(id.error());
    }
    track.id = *id;
    track.policy = policy(text(object, "policy"));
    const auto title = field(object, "title");
    if (!title) {
        track.stub = true;  // playlists list later tracks as {id, kind, policy, monetization_model}
        return track;
    }
    track.title = stringOf(*title);
    if (const auto user = field(object, "user")) {
        track.user = userRef(*user);
    }
    track.genre = text(object, "genre");
    track.description = text(object, "description");
    track.durationMs = number(object, "duration", 0);
    track.fullDurationMs = number(object, "full_duration", track.durationMs);
    track.createdAt = timestamp(object, "created_at");
    track.releasedAt = timestamp(object, "release_date");
    if (!track.releasedAt.isValid()) {
        track.releasedAt = timestamp(object, "display_date");
    }
    track.lastModified = text(object, "last_modified");
    track.permalinkUrl = link(object, "permalink_url");
    track.artworkUrl = link(object, "artwork_url");
    track.waveformUrl = link(object, "waveform_url");
    track.playbackCount = number(object, "playback_count");
    track.likesCount = number(object, "likes_count");
    track.repostsCount = number(object, "reposts_count");
    track.commentCount = number(object, "comment_count");
    bool streamable = false;
    const auto streamableField = field(object, "streamable");
    track.streamable =
        !streamableField || (streamableField->get_bool().get(streamable) == simdjson::SUCCESS && streamable);
    track.trackAuthorization = text(object, "track_authorization");
    if (const auto publisher = field(object, "publisher_metadata")) {
        track.publisherArtist = text(*publisher, "artist");
        track.publisherAlbum = text(*publisher, "album_title");
    }
    simdjson::dom::array list;
    if (const auto media = field(object, "media");
        media && (*media)["transcodings"].get_array().get(list) == simdjson::SUCCESS) {
        track.transcodings.reserve(list.size());
        for (const element entry : list) {
            if (auto parsed = transcoding(entry)) {
                track.transcodings.push_back(std::move(*parsed));
            }
        }
    }
    return track;
}

Result<User> userFrom(element object)
{
    User user;
    Result<Id> id = requiredId(object);
    if (!id) {
        return std::unexpected(id.error());
    }
    const UserRef ref = userRef(object);
    user.id = *id;
    user.username = ref.username;
    user.permalink = ref.permalink;
    user.avatarUrl = ref.avatarUrl;
    user.verified = ref.verified;
    user.fullName = text(object, "full_name");
    user.permalinkUrl = link(object, "permalink_url");
    user.city = text(object, "city");
    user.countryCode = text(object, "country_code");
    user.description = text(object, "description");
    user.followersCount = number(object, "followers_count");
    user.followingsCount = number(object, "followings_count");
    user.trackCount = number(object, "track_count");
    user.playlistCount = number(object, "playlist_count");
    user.likesCount = number(object, "likes_count");
    return user;
}

Result<Playlist> playlistFrom(element object)
{
    Playlist playlist;
    Result<Id> id = requiredId(object);  // system playlists have string URNs and are skipped by collections
    if (!id) {
        return std::unexpected(id.error());
    }
    playlist.id = *id;
    playlist.title = text(object, "title");
    if (const auto user = field(object, "user")) {
        playlist.user = userRef(*user);
    }
    playlist.genre = text(object, "genre");
    playlist.setType = text(object, "set_type");
    playlist.album = flag(object, "is_album") || (!playlist.setType.isEmpty() && playlist.setType != u"playlist");
    playlist.permalinkUrl = link(object, "permalink_url");
    playlist.artworkUrl = link(object, "artwork_url");
    playlist.durationMs = number(object, "duration", 0);
    playlist.trackCount = number(object, "track_count", 0);
    playlist.likesCount = number(object, "likes_count");
    playlist.repostsCount = number(object, "reposts_count");
    playlist.createdAt = timestamp(object, "created_at");
    playlist.publishedAt = timestamp(object, "published_at");
    playlist.lastModified = text(object, "last_modified");
    simdjson::dom::array list;
    if (object["tracks"].get_array().get(list) == simdjson::SUCCESS) {
        playlist.tracks.reserve(list.size());
        for (const element entry : list) {
            if (Result<Track> track = trackFrom(entry)) {
                playlist.tracks.push_back(std::move(*track));
            }
        }
    }
    return playlist;
}

std::optional<Item> itemFrom(element object)
{
    const std::string_view type = kind(object);
    if (type == "track") {
        if (Result<Track> track = trackFrom(object)) {
            return Item(std::move(*track));
        }
    } else if (type == "playlist") {
        if (Result<Playlist> playlist = playlistFrom(object)) {
            return Item(std::move(*playlist));
        }
    } else if (type == "user") {
        if (Result<User> user = userFrom(object)) {
            return Item(std::move(*user));
        }
    }
    return std::nullopt;
}

// A collection element: a bare object, or a wrapper such as {"track": …, "created_at": …} (likes),
// {"track": …, "played_at": …} (history), {"type": "track-repost", "user": …, "track": …} (feed),
// {"track": …, "score": …} (charts).
std::optional<Entry> entryFrom(element object)
{
    std::optional<Item> item = itemFrom(object);
    if (item) {
        return Entry{.item = std::move(*item), .at = {}, .repostedBy = {}};
    }
    for (const std::string_view key : {"track", "playlist", "user"}) {
        if (const auto inner = field(object, key)) {
            item = itemFrom(*inner);
            break;
        }
    }
    if (!item) {
        return std::nullopt;
    }
    Entry entry{.item = std::move(*item), .at = {}, .repostedBy = {}};
    entry.at = timestamp(object, "played_at");
    if (!entry.at.isValid()) {
        entry.at = timestamp(object, "created_at");
    }
    if (text(object, "type").endsWith(u"-repost")) {
        if (const auto reposter = field(object, "user")) {
            entry.repostedBy = userRef(*reposter);
        }
    }
    return entry;
}

template <class T> Result<T> single(const QByteArray& json, Result<T> (*from)(element))
{
    Result<element> root = document(json);
    if (!root) {
        return std::unexpected(root.error());
    }
    if (root->type() != element_type::OBJECT) {
        return fail(ErrorCode::InvalidResponse, "expected an object");
    }
    return from(*root);
}

}  // namespace

Result<Track> track(const QByteArray& json)
{
    return single<Track>(json, trackFrom);
}

Result<Track> trackDetail(const QByteArray& json)
{
    return single<Track>(json, [](element object) -> Result<Track> {
        Result<Track> track = trackFrom(object);
        if (track) {
            track->tagList = text(object, "tag_list");
            track->isPrivate = text(object, "sharing") == u"private";
        }
        return track;
    });
}

Result<Page<Comment>> comments(const QByteArray& json)
{
    Result<element> root = document(json);
    if (!root) {
        return std::unexpected(root.error());
    }
    simdjson::dom::array list;
    if (root->type() != element_type::OBJECT || (*root)["collection"].get_array().get(list) != simdjson::SUCCESS) {
        return fail(ErrorCode::InvalidResponse, "expected a collection of comments");
    }
    Page<Comment> page;
    page.next = link(*root, "next_href");
    page.items.reserve(list.size());
    for (const element entry : list) {
        Result<Id> id = requiredId(entry);
        if (!id) {
            continue;
        }
        Comment comment;
        comment.id = *id;
        comment.body = text(entry, "body");
        if (const auto user = field(entry, "user")) {
            comment.user = userRef(*user);
        }
        comment.createdAt = timestamp(entry, "created_at");
        comment.timestampMs = number(entry, "timestamp");
        page.items.push_back(std::move(comment));
    }
    return page;
}

Result<Done> nothing(const QByteArray& /*json*/)
{
    return Done{};
}

Result<User> user(const QByteArray& json)
{
    return single<User>(json, userFrom);
}

Result<Playlist> playlist(const QByteArray& json)
{
    return single<Playlist>(json, playlistFrom);
}

Result<Item> item(const QByteArray& json)
{
    Result<element> root = document(json);
    if (!root) {
        return std::unexpected(root.error());
    }
    if (auto parsed = itemFrom(*root)) {
        return std::move(*parsed);
    }
    return fail(ErrorCode::InvalidResponse, "not a track, playlist or user");
}

Result<std::vector<Track>> tracks(const QByteArray& json)
{
    Result<element> root = document(json);
    if (!root) {
        return std::unexpected(root.error());
    }
    simdjson::dom::array list;
    if (root->get_array().get(list) != simdjson::SUCCESS) {
        return fail(ErrorCode::InvalidResponse, "expected an array of tracks");
    }
    std::vector<Track> result;
    result.reserve(list.size());
    for (const element entry : list) {
        if (Result<Track> parsed = trackFrom(entry)) {
            result.push_back(std::move(*parsed));
        }
    }
    return result;
}

Result<Page<Entry>> playlistTracks(const QByteArray& json)
{
    Result<element> root = document(json);
    if (!root) {
        return std::unexpected(root.error());
    }
    simdjson::dom::array list;
    if ((*root)["tracks"].get_array().get(list) != simdjson::SUCCESS) {
        return fail(ErrorCode::InvalidResponse, "expected a playlist with tracks");
    }
    Page<Entry> page;
    page.items.reserve(list.size());
    for (const element entry : list) {
        if (Result<Track> track = trackFrom(entry)) {
            page.items.push_back({.item = std::move(*track), .at = {}, .repostedBy = std::nullopt});
        }
    }
    return page;
}

namespace {

Result<Page<Entry>> collectionFrom(element root)
{
    simdjson::dom::array list;
    Page<Entry> page;
    if (root.type() == element_type::OBJECT) {
        if (root["collection"].get_array().get(list) != simdjson::SUCCESS) {
            return fail(ErrorCode::InvalidResponse, "object without a collection");
        }
        page.next = link(root, "next_href");
    } else if (root.get_array().get(list) != simdjson::SUCCESS) {
        return fail(ErrorCode::InvalidResponse, "expected a collection");
    }
    page.items.reserve(list.size());
    for (const element entry : list) {
        if (auto parsed = entryFrom(entry)) {
            page.items.push_back(std::move(*parsed));
        }
    }
    return page;
}

}  // namespace

Result<Page<Entry>> collection(const QByteArray& json)
{
    NOCTARA_TRACE_SCOPE("api: parse page");
    Result<element> root = document(json);
    if (!root) {
        return std::unexpected(root.error());
    }
    return collectionFrom(*root);
}

Result<SnapshotFields> snapshot(const QByteArray& json)
{
    NOCTARA_TRACE_SCOPE("api: parse snapshot");
    Result<element> root = document(json);
    if (!root) {
        return std::unexpected(root.error());
    }
    if (root->type() != element_type::OBJECT) {
        return fail(ErrorCode::Corrupt, "a snapshot is an object");
    }
    Result<Page<Entry>> page = collectionFrom(*root);
    if (!page) {
        return std::unexpected(page.error());
    }
    SnapshotFields fields{.page = std::move(*page), .tag = text(*root, "noctara_tag"), .complete = true};
    if (field(*root, "noctara_complete")) {
        fields.complete = flag(*root, "noctara_complete");
    }
    return fields;
}

Result<Waveform> waveform(const QByteArray& json)
{
    Result<element> root = document(json);
    if (!root) {
        return std::unexpected(root.error());
    }
    simdjson::dom::array list;
    if (root->type() != element_type::OBJECT || (*root)["samples"].get_array().get(list) != simdjson::SUCCESS) {
        return fail(ErrorCode::InvalidResponse, "waveform without samples");
    }
    Waveform result;
    result.samples.reserve(list.size());
    std::int64_t peak = number(*root, "height", 0);
    for (const element sample : list) {
        const std::int64_t value = integer(sample).value_or(0);
        peak = std::max(peak, value);
        result.samples.push_back(static_cast<float>(value));
    }
    if (peak > 0) {
        const float scale = 1.0F / static_cast<float>(peak);
        for (float& sample : result.samples) {
            sample = std::clamp(sample * scale, 0.0F, 1.0F);
        }
    }
    return result;
}

Result<QUrl> streamUrl(const QByteArray& json)
{
    Result<element> root = document(json);
    if (!root) {
        return std::unexpected(root.error());
    }
    QUrl url(root->type() == element_type::OBJECT ? link(*root, "url") : QString());
    if (!url.isValid() || url.scheme() != u"https") {
        return fail(ErrorCode::InvalidResponse, "stream resolution without an https URL");
    }
    return url;
}

Result<ProtectedStream> protectedStream(const QByteArray& json)
{
    Result<element> root = document(json);
    if (!root) {
        return std::unexpected(root.error());
    }
    if (root->type() != element_type::OBJECT) {
        return fail(ErrorCode::InvalidResponse, "protected stream resolution is not an object");
    }
    ProtectedStream stream{.url = QUrl(link(*root, "url")), .licenseToken = text(*root, "licenseAuthToken")};
    if (!stream.url.isValid() || stream.url.scheme() != u"https" || stream.licenseToken.isEmpty()) {
        return fail(ErrorCode::InvalidResponse, "protected stream without an https URL or a license token");
    }
    return stream;
}

}  // namespace noctara::api::parse
