#include "cache/Tagger.hpp"

#include "cache/AudioPayload.hpp"
#include "cache/FileOps.hpp"
#include "cache/TrackInfo.hpp"
#include "core/FileIo.hpp"

#include <QFile>

#include <cstdio>
#include <memory>
#include <taglib/attachedpictureframe.h>
#include <taglib/flacpicture.h>
#include <taglib/id3v2tag.h>
#include <taglib/mp4coverart.h>
#include <taglib/mp4file.h>
#include <taglib/mp4tag.h>
#include <taglib/mpegfile.h>
#include <taglib/opusfile.h>
#include <taglib/textidentificationframe.h>
#include <taglib/urllinkframe.h>
#include <taglib/vorbisfile.h>
#include <taglib/xiphcomment.h>

namespace noctara::cache {

namespace {

constexpr auto kMp4UrlItem = "----:com.apple.iTunes:WEBSITE";

TagLib::String toTag(const QString& text)
{
    return {text.toUtf8().constData(), TagLib::String::UTF8};
}

QString fromTag(const TagLib::String& text)
{
    return QString::fromStdString(text.to8Bit(true));
}

TagLib::ByteVector toBytes(const QByteArray& data)
{
    return {data.constData(), static_cast<unsigned int>(data.size())};
}

QByteArray fromBytes(const TagLib::ByteVector& data)
{
    return {data.data(), static_cast<qsizetype>(data.size())};
}

struct Properties {
    int lengthMs = 0;
    int sampleRate = 0;
    int channels = 0;
    friend bool operator==(const Properties&, const Properties&) = default;
};

// Opus and Vorbis share the Ogg container; the first packet tells them apart.
bool isOpus(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    return file.read(64).contains("OpusHead");
}

std::unique_ptr<TagLib::File> openFile(const QString& path, Container container)
{
    const QByteArray name = QFile::encodeName(path);
    switch (container) {
    case Container::Mpeg: return std::make_unique<TagLib::MPEG::File>(name.constData());
    case Container::Mp4: return std::make_unique<TagLib::MP4::File>(name.constData());
    case Container::Ogg:
        if (isOpus(path)) {
            return std::make_unique<TagLib::Ogg::Opus::File>(name.constData());
        }
        return std::make_unique<TagLib::Vorbis::File>(name.constData());
    case Container::Unknown: break;
    }
    return nullptr;
}

Properties propertiesOf(TagLib::File const& file)
{
    const TagLib::AudioProperties* audio = file.audioProperties();
    if (audio == nullptr) {
        return {};
    }
    return {audio->lengthInMilliseconds(), audio->sampleRate(), audio->channels()};
}

void setText(TagLib::ID3v2::Tag& tag, const char* id, const QString& value)
{
    tag.removeFrames(id);
    if (value.isEmpty()) {
        return;
    }
    auto frame = std::make_unique<TagLib::ID3v2::TextIdentificationFrame>(TagLib::ByteVector(id), TagLib::String::UTF8);
    frame->setText(toTag(value));
    tag.addFrame(frame.release());  // the tag takes ownership
}

void applyId3(TagLib::MPEG::File& file, const TagData& tags)
{
    TagLib::ID3v2::Tag& tag = *file.ID3v2Tag(true);
    setText(tag, "TIT2", tags.title);
    setText(tag, "TPE1", tags.artist);
    setText(tag, "TALB", tags.album);
    setText(tag, "TCON", tags.genre);
    setText(tag, "TDRC", tags.date);
    tag.removeFrames("WOAF");
    if (!tags.permalink.isEmpty()) {
        auto url = std::make_unique<TagLib::ID3v2::UrlLinkFrame>(TagLib::ByteVector("WOAF"));
        url->setUrl(toTag(tags.permalink.toString()));
        tag.addFrame(url.release());
    }
    tag.removeFrames("APIC");
    if (!tags.cover.isEmpty()) {
        auto picture = std::make_unique<TagLib::ID3v2::AttachedPictureFrame>();
        picture->setTextEncoding(TagLib::String::UTF8);
        picture->setMimeType(toTag(tags.coverMime));
        picture->setType(TagLib::ID3v2::AttachedPictureFrame::FrontCover);
        picture->setPicture(toBytes(tags.cover));
        tag.addFrame(picture.release());
    }
}

void setItem(TagLib::MP4::Tag& tag, const char* key, const QString& value)
{
    if (value.isEmpty()) {
        tag.removeItem(key);
    } else {
        tag.setItem(key, TagLib::StringList(toTag(value)));
    }
}

void applyMp4(TagLib::MP4::File const& file, const TagData& tags)
{
    TagLib::MP4::Tag& tag = *file.tag();
    setItem(tag, "\251nam", tags.title);
    setItem(tag, "\251ART", tags.artist);
    setItem(tag, "\251alb", tags.album);
    setItem(tag, "\251gen", tags.genre);
    setItem(tag, "\251day", tags.date);
    setItem(tag, kMp4UrlItem, tags.permalink.toString());
    if (tags.cover.isEmpty()) {
        tag.removeItem("covr");
    } else {
        const auto format = tags.coverMime == u"image/png" ? TagLib::MP4::CoverArt::PNG : TagLib::MP4::CoverArt::JPEG;
        TagLib::MP4::CoverArtList covers;
        covers.append(TagLib::MP4::CoverArt(format, toBytes(tags.cover)));
        tag.setItem("covr", covers);
    }
}

void setField(TagLib::Ogg::XiphComment& comment, const char* key, const QString& value)
{
    comment.removeFields(key);
    if (!value.isEmpty()) {
        comment.addField(key, toTag(value), true);
    }
}

void applyXiph(TagLib::Ogg::XiphComment& comment, const TagData& tags)
{
    setField(comment, "TITLE", tags.title);
    setField(comment, "ARTIST", tags.artist);
    setField(comment, "ALBUM", tags.album);
    setField(comment, "GENRE", tags.genre);
    setField(comment, "DATE", tags.date);
    setField(comment, "WEBSITE", tags.permalink.toString());
    comment.removeAllPictures();
    if (!tags.cover.isEmpty()) {
        auto picture = std::make_unique<TagLib::FLAC::Picture>();
        picture->setType(TagLib::FLAC::Picture::FrontCover);
        picture->setMimeType(toTag(tags.coverMime));
        picture->setData(toBytes(tags.cover));
        comment.addPicture(picture.release());  // the comment takes ownership
    }
}

Result<void> apply(TagLib::File& file, Container container, const TagData& tags)
{
    bool saved = false;
    if (container == Container::Mpeg) {
        auto& mpeg = dynamic_cast<TagLib::MPEG::File&>(file);
        applyId3(mpeg, tags);
        // Only ID3v2.4: an ID3v1 or APE tag (outside the payload) would just duplicate it.
        saved = mpeg.save(TagLib::MPEG::File::ID3v2, TagLib::File::StripOthers, TagLib::ID3v2::v4);
    } else if (container == Container::Mp4) {
        applyMp4(dynamic_cast<TagLib::MP4::File&>(file), tags);
        saved = file.save();
    } else if (const auto* opus = dynamic_cast<TagLib::Ogg::Opus::File*>(&file)) {
        applyXiph(*opus->tag(), tags);
        saved = file.save();
    } else if (const auto* vorbis = dynamic_cast<TagLib::Vorbis::File*>(&file)) {
        applyXiph(*vorbis->tag(), tags);
        saved = file.save();
    }
    if (!saved) {
        return fail(ErrorCode::IoError, "TagLib could not save the tags");
    }
    return {};
}

}  // namespace

QString sanitizeTagText(const QString& text)
{
    QString clean;
    clean.reserve(text.size());
    for (const QChar c : text) {
        if (c.category() != QChar::Other_Control) {
            clean.append(c);
        }
    }
    return clean.trimmed();
}

TagData tagDataFor(const TrackInfo& info, const QByteArray& cover, const QString& coverMime)
{
    const QDateTime& when = info.released.isValid() ? info.released : info.created;
    TagData tags;
    tags.title = sanitizeTagText(info.title);
    tags.artist = sanitizeTagText(info.artist.isEmpty() ? info.uploader : info.artist);
    tags.album = sanitizeTagText(info.album);
    tags.genre = sanitizeTagText(info.genre);
    tags.date = when.isValid() ? when.toUTC().date().toString(Qt::ISODate) : QString();
    tags.permalink = info.permalink;
    tags.cover = cover;
    tags.coverMime = cover.isEmpty() ? QString() : coverMime;
    return tags;
}

Result<void> writeTags(const QString& path, const TagData& tags, const QByteArray& expectedPayload, FileOps& ops)
{
    const Result<QByteArray> original = readWholeFile(path);
    if (!original) {
        return std::unexpected(original.error());
    }
    const Container container =
        detectContainer(QByteArrayView(*original).first(std::min<qsizetype>(16, original->size())));
    if (container == Container::Unknown) {
        return fail(ErrorCode::Unsupported, "container cannot be tagged without remuxing");
    }
    const Result<QByteArray> before = payloadHash(*original);
    if (!before) {
        return std::unexpected(before.error());
    }
    if (!expectedPayload.isEmpty() && *before != expectedPayload) {
        return fail(ErrorCode::Corrupt, "audio payload differs from the recorded hash");
    }

    const QString temp = path + QStringLiteral(".tagging");
    const auto discard = [&temp] {
        QFile::remove(temp);
    };
    if (auto copied = ops.copyFile(path, temp); !copied) {
        discard();
        return copied;
    }
    Properties propertiesBefore;
    {
        const std::unique_ptr<TagLib::File> source = openFile(path, container);
        if (!source || !source->isValid()) {
            discard();
            return fail(ErrorCode::Unsupported, "TagLib cannot read this file");
        }
        propertiesBefore = propertiesOf(*source);
    }
    {
        const std::unique_ptr<TagLib::File> target = openFile(temp, container);
        if (!target || !target->isValid()) {
            discard();
            return fail(ErrorCode::Unsupported, "TagLib cannot open the copy");
        }
        if (auto applied = apply(*target, container, tags); !applied) {
            discard();
            return applied;
        }
    }
    const Result<QByteArray> after = payloadHashOfFile(temp);
    if (!after || *after != *before) {
        discard();
        return fail(ErrorCode::Corrupt, "tagging changed the audio payload");
    }
    {
        const std::unique_ptr<TagLib::File> check = openFile(temp, container);
        if (!check || !check->isValid() || propertiesOf(*check) != propertiesBefore) {
            discard();
            return fail(ErrorCode::Corrupt, "tagging changed the audio properties");
        }
    }
    if (std::rename(QFile::encodeName(temp).constData(), QFile::encodeName(path).constData()) != 0) {
        discard();
        return fail(errorCodeFromErrno(errno), "rename after tagging failed");
    }
    return {};
}

Result<TagData> readTags(const QString& path)
{
    const Result<QByteArray> head = [&] -> Result<QByteArray> {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            return fail(ErrorCode::NotFound, "cannot open");
        }
        return file.read(16);
    }();
    if (!head) {
        return std::unexpected(head.error());
    }
    const Container container = detectContainer(*head);
    const std::unique_ptr<TagLib::File> file = openFile(path, container);
    if (!file || !file->isValid()) {
        return fail(ErrorCode::Unsupported, "TagLib cannot read this file");
    }
    TagData tags;
    if (auto* mpeg = dynamic_cast<TagLib::MPEG::File*>(file.get())) {
        const TagLib::ID3v2::Tag* tag = mpeg->ID3v2Tag(false);
        if (tag == nullptr) {
            return tags;
        }
        const auto text = [tag](const char* id) {
            const TagLib::ID3v2::FrameList& frames = tag->frameListMap()[id];
            return frames.isEmpty() ? QString() : fromTag(frames.front()->toString());
        };
        tags.title = text("TIT2");
        tags.artist = text("TPE1");
        tags.album = text("TALB");
        tags.genre = text("TCON");
        tags.date = text("TDRC");
        tags.permalink = QUrl(text("WOAF"));
        const TagLib::ID3v2::FrameList& pictures = tag->frameListMap()["APIC"];
        if (!pictures.isEmpty()) {
            const auto* picture = dynamic_cast<const TagLib::ID3v2::AttachedPictureFrame*>(pictures.front());
            if (picture != nullptr) {
                tags.cover = fromBytes(picture->picture());
                tags.coverMime = fromTag(picture->mimeType());
            }
        }
    } else if (const auto* mp4 = dynamic_cast<TagLib::MP4::File*>(file.get())) {
        const TagLib::MP4::Tag* tag = mp4->tag();
        const auto text = [tag](const char* key) {
            const TagLib::MP4::Item item = tag->item(key);
            return item.isValid() && !item.toStringList().isEmpty() ? fromTag(item.toStringList().front()) : QString();
        };
        tags.title = text("\251nam");
        tags.artist = text("\251ART");
        tags.album = text("\251alb");
        tags.genre = text("\251gen");
        tags.date = text("\251day");
        tags.permalink = QUrl(text(kMp4UrlItem));
        const TagLib::MP4::Item covers = tag->item("covr");
        if (covers.isValid() && !covers.toCoverArtList().isEmpty()) {
            const TagLib::MP4::CoverArt cover = covers.toCoverArtList().front();
            tags.cover = fromBytes(cover.data());
            tags.coverMime = cover.format() == TagLib::MP4::CoverArt::PNG ? QStringLiteral("image/png")
                                                                          : QStringLiteral("image/jpeg");
        }
    } else {
        TagLib::Ogg::XiphComment* comment = nullptr;
        if (const auto* opus = dynamic_cast<TagLib::Ogg::Opus::File*>(file.get())) {
            comment = opus->tag();
        } else if (const auto* vorbis = dynamic_cast<TagLib::Vorbis::File*>(file.get())) {
            comment = vorbis->tag();
        }
        if (comment == nullptr) {
            return tags;
        }
        const auto text = [comment](const char* key) {
            const TagLib::Ogg::FieldListMap& fields = comment->fieldListMap();
            const auto it = fields.find(key);
            return it == fields.end() || it->second.isEmpty() ? QString() : fromTag(it->second.front());
        };
        tags.title = text("TITLE");
        tags.artist = text("ARTIST");
        tags.album = text("ALBUM");
        tags.genre = text("GENRE");
        tags.date = text("DATE");
        tags.permalink = QUrl(text("WEBSITE"));
        const TagLib::List<TagLib::FLAC::Picture*> pictures = comment->pictureList();
        if (!pictures.isEmpty()) {
            tags.cover = fromBytes(pictures.front()->data());
            tags.coverMime = fromTag(pictures.front()->mimeType());
        }
    }
    return tags;
}

}  // namespace noctara::cache
