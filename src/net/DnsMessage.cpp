#include "net/DnsMessage.hpp"

#include <QList>

#include <algorithm>
#include <format>

namespace noctara::net::dns {

namespace {

constexpr std::uint16_t kClassIn = 1;
constexpr std::uint16_t kFlagRecursionDesired = 0x0100;
constexpr std::uint16_t kFlagResponse = 0x8000;
constexpr std::uint16_t kFlagTruncated = 0x0200;
constexpr std::uint16_t kOptionPadding = 12;
constexpr qsizetype kHeaderSize = 12;
constexpr int kMaxCnameChain = 16;

void put16(QByteArray& out, std::uint16_t value)
{
    out.append(static_cast<char>(value >> 8U));
    out.append(static_cast<char>(value & 0xffU));
}

std::unexpected<Error> malformed(std::string detail)
{
    return fail(ErrorCode::ParseError, "DNS: " + std::move(detail));
}

// Bounds-checked big-endian reader over one message; name decompression jumps within the same message.
class Reader {
public:
    explicit Reader(QByteArrayView message)
        : m_data(message)
    {
    }

    [[nodiscard]] qsizetype offset() const noexcept { return m_offset; }
    [[nodiscard]] bool has(qsizetype bytes) const noexcept { return m_offset + bytes <= m_data.size(); }

    Result<std::uint16_t> u16()
    {
        if (!has(2)) {
            return malformed("truncated");
        }
        const auto hi = static_cast<std::uint8_t>(m_data[m_offset]);
        const auto lo = static_cast<std::uint8_t>(m_data[m_offset + 1]);
        m_offset += 2;
        return static_cast<std::uint16_t>((static_cast<unsigned>(hi) << 8U) | lo);
    }

    Result<std::uint32_t> u32()
    {
        auto hi = u16();
        if (!hi) {
            return std::unexpected(hi.error());
        }
        auto lo = u16();
        if (!lo) {
            return std::unexpected(lo.error());
        }
        return (static_cast<std::uint32_t>(*hi) << 16U) | *lo;
    }

    Result<QByteArrayView> bytes(qsizetype count)
    {
        if (count < 0 || !has(count)) {
            return malformed("truncated record data");
        }
        const QByteArrayView view = m_data.sliced(m_offset, count);
        m_offset += count;
        return view;
    }

    // Reads a (possibly compressed) name at the current offset, lower-cased, without trailing dot.
    Result<QByteArray> name() { return nameAt(m_offset, true); }

    // Decodes a name that starts at `at` without moving the reader (used inside record data).
    Result<QByteArray> nameFrom(qsizetype at)
    {
        qsizetype cursor = at;
        return nameAt(cursor, false);
    }

private:
    Result<QByteArray> nameAt(qsizetype& cursor, bool advance)
    {
        QByteArray out;
        qsizetype position = cursor;
        bool jumped = false;
        int jumps = 0;
        while (true) {
            if (position >= m_data.size()) {
                return malformed("name runs past the message");
            }
            const auto length = static_cast<std::uint8_t>(m_data[position]);
            if ((length & 0xc0U) == 0xc0U) {
                if (position + 1 >= m_data.size() || ++jumps > 64) {
                    return malformed("bad compression pointer");
                }
                const auto target =
                    static_cast<qsizetype>(((length & 0x3fU) << 8U) | static_cast<std::uint8_t>(m_data[position + 1]));
                if (!jumped && advance) {
                    cursor = position + 2;
                }
                jumped = true;
                position = target;
                continue;
            }
            if ((length & 0xc0U) != 0) {
                return malformed("unsupported label type");
            }
            if (length == 0) {
                if (!jumped && advance) {
                    cursor = position + 1;
                }
                break;
            }
            if (position + 1 + length > m_data.size() || out.size() + length + 1 > 255) {
                return malformed("name too long");
            }
            if (!out.isEmpty()) {
                out.append('.');
            }
            out.append(m_data.sliced(position + 1, length).toByteArray().toLower());
            position += 1 + length;
        }
        if (advance) {
            m_offset = cursor;
        }
        return out;
    }

    QByteArrayView m_data;
    qsizetype m_offset = 0;
};

struct RawRecord {
    QByteArray owner;
    std::uint16_t type = 0;
    std::uint32_t ttl = 0;
    qsizetype dataOffset = 0;
    QByteArrayView data;
};

Result<RawRecord> readRecord(Reader& reader)
{
    RawRecord record;
    auto owner = reader.name();
    auto type = owner ? reader.u16() : std::unexpected(owner.error());
    auto klass = type ? reader.u16() : std::unexpected(type.error());
    auto ttl = klass ? reader.u32() : std::unexpected(klass.error());
    auto length = ttl ? reader.u16() : std::unexpected(ttl.error());
    if (!length) {
        return std::unexpected(length.error());
    }
    record.dataOffset = reader.offset();
    auto data = reader.bytes(*length);
    if (!data) {
        return std::unexpected(data.error());
    }
    record.owner = std::move(*owner);
    record.type = *type;
    record.ttl = (*ttl & 0x80000000U) != 0 ? 0 : *ttl;  // RFC 2181 §8: values with the top bit set mean zero
    record.data = *data;
    return record;
}

}  // namespace

Result<QByteArray> encodeQuery(QByteArrayView name, RecordType type, std::uint16_t id, bool pad)
{
    if (name.isEmpty() || name.size() > 253) {
        return fail(ErrorCode::InvalidArgument, "DNS: invalid query name");
    }
    QByteArray out;
    out.reserve(kPaddingBlock);
    put16(out, id);
    put16(out, kFlagRecursionDesired);
    put16(out, 1);  // QDCOUNT
    put16(out, 0);
    put16(out, 0);
    put16(out, 1);  // ARCOUNT: the OPT record
    for (const QByteArrayView label : QByteArray(name.toByteArray()).split('.')) {
        if (label.isEmpty() || label.size() > 63) {
            return fail(ErrorCode::InvalidArgument, "DNS: invalid label in query name");
        }
        out.append(static_cast<char>(label.size()));
        out.append(label);
    }
    out.append('\0');
    put16(out, static_cast<std::uint16_t>(type));
    put16(out, kClassIn);

    // OPT pseudo-record: root owner, requestor's UDP payload size in CLASS, no flags, optional padding option.
    out.append('\0');
    put16(out, static_cast<std::uint16_t>(RecordType::Opt));
    put16(out, kUdpPayloadSize);
    put16(out, 0);
    put16(out, 0);
    if (!pad) {
        put16(out, 0);
        return out;
    }
    constexpr qsizetype kRdLengthAndOptionHeader = 2 + 4;
    const qsizetype unpadded = out.size() + kRdLengthAndOptionHeader;
    const qsizetype padding = (kPaddingBlock - unpadded % kPaddingBlock) % kPaddingBlock;
    put16(out, static_cast<std::uint16_t>(4 + padding));
    put16(out, kOptionPadding);
    put16(out, static_cast<std::uint16_t>(padding));
    out.append(padding, '\0');
    return out;
}

Result<Response> decodeResponse(QByteArrayView message, QByteArrayView name, RecordType type)
{
    if (message.size() < kHeaderSize) {
        return malformed("message shorter than its header");
    }
    Reader reader(message);
    Response response;
    response.id = *reader.u16();
    const std::uint16_t flags = *reader.u16();
    const std::uint16_t questions = *reader.u16();
    const std::uint16_t answers = *reader.u16();
    const std::uint16_t authorities = *reader.u16();
    (void)*reader.u16();  // additional records are not needed
    if ((flags & kFlagResponse) == 0 || ((static_cast<unsigned>(flags) >> 11U) & 0xfU) != 0) {
        return malformed("not a standard query response");
    }
    response.truncated = (flags & kFlagTruncated) != 0;
    response.rcode = static_cast<Rcode>(flags & 0xfU);
    if (questions != 1) {
        return malformed(std::format("{} questions instead of 1", questions));
    }
    auto qname = reader.name();
    auto qtype = qname ? reader.u16() : std::unexpected(qname.error());
    auto qclass = qtype ? reader.u16() : std::unexpected(qtype.error());
    if (!qclass) {
        return std::unexpected(qclass.error());
    }
    const QByteArray wanted = name.toByteArray().toLower();
    if (*qname != wanted || *qtype != static_cast<std::uint16_t>(type) || *qclass != kClassIn) {
        return malformed("answer is for a different question");
    }
    if (response.truncated) {
        return response;  // the caller retries over TCP; the partial content is not trusted
    }

    std::vector<RawRecord> records;
    records.reserve(answers);
    for (std::uint16_t i = 0; i < answers; ++i) {
        auto record = readRecord(reader);
        if (!record) {
            return std::unexpected(record.error());
        }
        records.push_back(std::move(*record));
    }
    for (std::uint16_t i = 0; i < authorities; ++i) {
        auto record = readRecord(reader);
        if (!record) {
            return std::unexpected(record.error());
        }
        if (record->type != static_cast<std::uint16_t>(RecordType::Soa)) {
            continue;
        }
        // SOA RDATA: MNAME, RNAME, then SERIAL REFRESH RETRY EXPIRE MINIMUM (5 × 32 bit); MINIMUM is last.
        if (record->data.size() < 20) {
            return malformed("short SOA record");
        }
        Reader tail(record->data.last(4));
        const std::uint32_t minimum = *tail.u32();
        response.negativeTtl = std::min(record->ttl, minimum);
    }

    // Follow the CNAME chain from the queried name; addresses must belong to its end.
    QByteArray current = wanted;
    std::uint32_t chainTtl = UINT32_MAX;
    for (int hop = 0; hop < kMaxCnameChain; ++hop) {
        const auto alias = std::ranges::find_if(records, [&](const RawRecord& r) {
            return r.owner == current && r.type == static_cast<std::uint16_t>(RecordType::Cname);
        });
        if (alias == records.end()) {
            break;
        }
        auto target = reader.nameFrom(alias->dataOffset);
        if (!target) {
            return std::unexpected(target.error());
        }
        chainTtl = std::min(chainTtl, alias->ttl);
        current = std::move(*target);
    }
    const qsizetype addressSize = type == RecordType::A ? 4 : 16;
    for (const RawRecord& record : records) {
        if (record.owner != current || record.type != static_cast<std::uint16_t>(type)) {
            continue;
        }
        if (record.data.size() != addressSize) {
            return malformed("address record of the wrong size");
        }
        const auto* raw = reinterpret_cast<const std::uint8_t*>(record.data.data());  // NOLINT: byte view
        const IpAddress address = type == RecordType::A ? IpAddress::fromV4(std::span<const std::uint8_t, 4>(raw, 4))
                                                        : IpAddress::fromV6(std::span<const std::uint8_t, 16>(raw, 16));
        response.addresses.push_back({address, std::min(record.ttl, chainTtl)});
    }
    return response;
}

}  // namespace noctara::net::dns
