#include "cache/TrackInfo.hpp"

#include "cache/Hash.hpp"

#include <QJsonDocument>
#include <QJsonObject>

namespace noctara::cache {

namespace {

QString dateText(const QDateTime& when)
{
    return when.isValid() ? when.toUTC().toString(Qt::ISODateWithMs) : QString();
}

QDateTime parseDate(const QJsonValue& value)
{
    return QDateTime::fromString(value.toString(), Qt::ISODateWithMs);
}

QString protocolName(StreamProtocol protocol)
{
    return protocol == StreamProtocol::Hls ? QStringLiteral("hls") : QStringLiteral("progressive");
}

}  // namespace

QByteArray TrackKey::name() const
{
    return nameFor(QByteArray(QByteArray("audio/v1\n") + trackId.toUtf8() + '\n' + preset.toUtf8()));
}

std::uint64_t TrackKey::id() const
{
    return name().first(16).toULongLong(nullptr, 16);
}

QByteArray TrackInfo::toJson() const
{
    const QJsonObject json{
        {QStringLiteral("version"), kVersion},
        {QStringLiteral("track_id"), key.trackId},
        {QStringLiteral("preset"), key.preset},
        {QStringLiteral("title"), title},
        {QStringLiteral("artist"), artist},
        {QStringLiteral("uploader"), uploader},
        {QStringLiteral("album"), album},
        {QStringLiteral("context"), context},
        {QStringLiteral("genre"), genre},
        {QStringLiteral("duration_ms"), static_cast<qint64>(durationMs)},
        {QStringLiteral("created"), dateText(created)},
        {QStringLiteral("released"), dateText(released)},
        {QStringLiteral("permalink"), permalink.toString()},
        {QStringLiteral("artwork_url"), artworkUrl.toString()},
        {QStringLiteral("artwork_thumb_url"), artworkThumbUrl.toString()},
        {QStringLiteral("waveform_url"), waveformUrl.toString()},
        {QStringLiteral("protocol"), protocolName(protocol)},
        {QStringLiteral("mime_type"), mimeType},
        {QStringLiteral("codec"), codec},
        {QStringLiteral("bitrate_kbps"), bitrateKbps},
        {QStringLiteral("quality"), quality},
        {QStringLiteral("source_updated_at"), sourceUpdatedAt},
        {QStringLiteral("media_etag"), QString::fromLatin1(mediaEtag)},
        {QStringLiteral("content_length"), static_cast<qint64>(contentLength)},
        {QStringLiteral("cached_at"), dateText(cachedAt)},
        {QStringLiteral("complete"), complete},
        {QStringLiteral("cover"), QString::fromLatin1(coverHash)},
        {QStringLiteral("thumbnail"), QString::fromLatin1(thumbHash)},
        {QStringLiteral("cover_mime"), coverMime},
        {QStringLiteral("payload_hash"), QString::fromLatin1(payloadHash)},
        {QStringLiteral("tagged_for"), taggedFor},
    };
    return QJsonDocument(json).toJson(QJsonDocument::Indented);
}

Result<TrackInfo> TrackInfo::fromJson(const QByteArray& json)
{
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return fail(ErrorCode::Corrupt, "sidecar is not a JSON object");
    }
    const QJsonObject o = document.object();
    const int version = o.value(QStringLiteral("version")).toInt();
    if (version < 1 || version > kVersion) {
        return fail(ErrorCode::Corrupt, "unsupported sidecar version");
    }
    TrackInfo info;
    info.key.trackId = o.value(QStringLiteral("track_id")).toString();
    info.key.preset = o.value(QStringLiteral("preset")).toString();
    if (info.key.trackId.isEmpty()) {
        return fail(ErrorCode::Corrupt, "sidecar without a track id");
    }
    info.title = o.value(QStringLiteral("title")).toString();
    info.artist = o.value(QStringLiteral("artist")).toString();
    info.uploader = o.value(QStringLiteral("uploader")).toString();
    info.album = o.value(QStringLiteral("album")).toString();
    info.context = o.value(QStringLiteral("context")).toString();
    info.genre = o.value(QStringLiteral("genre")).toString();
    info.durationMs = o.value(QStringLiteral("duration_ms")).toInteger();
    info.created = parseDate(o.value(QStringLiteral("created")));
    info.released = parseDate(o.value(QStringLiteral("released")));
    info.permalink = QUrl(o.value(QStringLiteral("permalink")).toString());
    info.artworkUrl = QUrl(o.value(QStringLiteral("artwork_url")).toString());
    info.artworkThumbUrl = QUrl(o.value(QStringLiteral("artwork_thumb_url")).toString());
    info.waveformUrl = QUrl(o.value(QStringLiteral("waveform_url")).toString());
    info.protocol =
        o.value(QStringLiteral("protocol")).toString() == u"hls" ? StreamProtocol::Hls : StreamProtocol::Progressive;
    info.mimeType = o.value(QStringLiteral("mime_type")).toString();
    info.codec = o.value(QStringLiteral("codec")).toString();
    info.bitrateKbps = o.value(QStringLiteral("bitrate_kbps")).toInt();
    info.quality = o.value(QStringLiteral("quality")).toString();
    info.sourceUpdatedAt = o.value(QStringLiteral("source_updated_at")).toString();
    info.mediaEtag = o.value(QStringLiteral("media_etag")).toString().toLatin1();
    info.contentLength = o.value(QStringLiteral("content_length")).toInteger(-1);
    info.cachedAt = parseDate(o.value(QStringLiteral("cached_at")));
    info.complete = o.value(QStringLiteral("complete")).toBool();
    info.coverHash = o.value(QStringLiteral("cover")).toString().toLatin1();
    info.thumbHash = o.value(QStringLiteral("thumbnail")).toString().toLatin1();
    info.coverMime = o.value(QStringLiteral("cover_mime")).toString();
    info.payloadHash = o.value(QStringLiteral("payload_hash")).toString().toLatin1();
    info.taggedFor = o.value(QStringLiteral("tagged_for")).toString();
    return info;
}

void TrackInfo::refreshFrom(const TrackInfo& fresh)
{
    title = fresh.title;
    artist = fresh.artist;
    uploader = fresh.uploader;
    album = fresh.album;
    context = fresh.context.isEmpty() ? context : fresh.context;
    genre = fresh.genre;
    durationMs = fresh.durationMs;
    created = fresh.created;
    released = fresh.released;
    permalink = fresh.permalink;
    if (fresh.artworkUrl != artworkUrl) {
        coverHash.clear();  // a new cover is fetched and linked
        thumbHash.clear();
        coverMime.clear();
    }
    artworkUrl = fresh.artworkUrl;
    artworkThumbUrl = fresh.artworkThumbUrl;
    waveformUrl = fresh.waveformUrl;
    mimeType = fresh.mimeType.isEmpty() ? mimeType : fresh.mimeType;
    codec = fresh.codec.isEmpty() ? codec : fresh.codec;
    bitrateKbps = fresh.bitrateKbps != 0 ? fresh.bitrateKbps : bitrateKbps;
    quality = fresh.quality.isEmpty() ? quality : fresh.quality;
    sourceUpdatedAt = fresh.sourceUpdatedAt;
}

bool TrackInfo::sameMetadata(const TrackInfo& other) const
{
    return title == other.title && artist == other.artist && uploader == other.uploader && album == other.album &&
           genre == other.genre && durationMs == other.durationMs && created == other.created &&
           released == other.released && permalink == other.permalink && artworkUrl == other.artworkUrl &&
           artworkThumbUrl == other.artworkThumbUrl && waveformUrl == other.waveformUrl &&
           sourceUpdatedAt == other.sourceUpdatedAt;
}

}  // namespace noctara::cache
