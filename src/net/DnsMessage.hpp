#pragma once

#include "core/Error.hpp"
#include "core/NetAddress.hpp"

#include <QByteArray>
#include <QByteArrayView>

#include <cstdint>
#include <optional>
#include <vector>

// DNS wire format (RFC 1035) for the A/AAAA lookups the resolver needs, with EDNS(0) (RFC 6891) and
// block-length padding (RFC 7830, RFC 8467). EDNS Client Subnet is never sent.
namespace noctara::net::dns {

enum class RecordType : std::uint16_t { A = 1, Cname = 5, Soa = 6, Opt = 41, Aaaa = 28 };

enum class Rcode : std::uint8_t { NoError = 0, FormErr = 1, ServFail = 2, NxDomain = 3, NotImp = 4, Refused = 5 };

inline constexpr std::uint16_t kUdpPayloadSize = 1232;  // avoids IP fragmentation (DNS flag day 2020)
inline constexpr qsizetype kPaddingBlock = 128;         // RFC 8467 recommendation for queries

[[nodiscard]] RecordType recordTypeFor(AddressFamily family) noexcept;

// `name` is an ASCII host name without trailing dot. `pad` pads the message to a multiple of 128 bytes.
[[nodiscard]] Result<QByteArray> encodeQuery(QByteArrayView name, RecordType type, std::uint16_t id, bool pad);

struct AddressRecord {
    IpAddress address;
    std::uint32_t ttl = 0;
};

struct Response {
    std::uint16_t id = 0;
    Rcode rcode = Rcode::NoError;
    bool truncated = false;
    // Addresses of the requested type for the queried name, following CNAMEs; TTLs include the chain's.
    std::vector<AddressRecord> addresses;
    // For NXDOMAIN / NODATA: min(SOA TTL, SOA MINIMUM) from the authority section (RFC 2308).
    std::optional<std::uint32_t> negativeTtl;
};

// Validates that `message` answers the question (name, type); rejects anything malformed or unrelated.
[[nodiscard]] Result<Response> decodeResponse(QByteArrayView message, QByteArrayView name, RecordType type);

}  // namespace noctara::net::dns
