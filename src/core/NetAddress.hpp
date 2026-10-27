#pragma once

#include <QString>
#include <QStringView>

#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <span>

namespace noctara {

enum class AddressFamily : std::uint8_t { V4, V6 };

// An IPv4 or IPv6 address, parsed strictly (no octal or shortened IPv4 forms).
class IpAddress {
public:
    // Accepts "192.0.2.1", "2001:db8::1" and "[2001:db8::1]".
    [[nodiscard]] static std::optional<IpAddress> parse(QStringView text);
    [[nodiscard]] static IpAddress fromV4(std::span<const std::uint8_t, 4> bytes) noexcept;
    [[nodiscard]] static IpAddress fromV6(std::span<const std::uint8_t, 16> bytes) noexcept;

    [[nodiscard]] AddressFamily family() const noexcept { return m_family; }
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept;
    [[nodiscard]] QString toString() const;   // canonical form, IPv6 without brackets
    [[nodiscard]] QString toUrlHost() const;  // IPv6 in brackets, as in URLs and "host:port" strings

    [[nodiscard]] bool isLoopback() const noexcept;
    [[nodiscard]] bool isPrivate() const noexcept;    // RFC 1918, RFC 6598 shared space, IPv6 ULA fc00::/7
    [[nodiscard]] bool isLinkLocal() const noexcept;  // 169.254.0.0/16, fe80::/10
    [[nodiscard]] bool isLocal() const noexcept { return isLoopback() || isPrivate() || isLinkLocal(); }

    friend auto operator<=>(const IpAddress&, const IpAddress&) = default;

private:
    [[nodiscard]] std::optional<IpAddress> mappedV4() const noexcept;  // ::ffff:a.b.c.d

    AddressFamily m_family = AddressFamily::V4;
    std::array<std::uint8_t, 16> m_bytes{};
};

// "10.0.0.0/8", "fd00::/8"; a bare address is a host prefix (/32 or /128).
struct Cidr {
    IpAddress network;
    int prefix = 0;

    [[nodiscard]] static std::optional<Cidr> parse(QStringView text);
    [[nodiscard]] bool contains(const IpAddress& address) const noexcept;
};

// "192.0.2.1", "192.0.2.1:5353", "2001:db8::1", "[2001:db8::1]:5353".
struct Endpoint {
    IpAddress address;
    std::uint16_t port = 0;

    [[nodiscard]] static std::optional<Endpoint> parse(QStringView text, std::uint16_t defaultPort);
    [[nodiscard]] QString toString() const;  // port omitted when it equals `defaultPort`
    [[nodiscard]] QString toString(std::uint16_t defaultPort) const;

    friend auto operator<=>(const Endpoint&, const Endpoint&) = default;
};

// A DNS host name in ASCII (IDNs must already be punycode): labels of 1-63 letters, digits, '-' or '_'
// that do not start or end with '-', at most 253 characters, optional trailing dot.
[[nodiscard]] bool isValidHostname(QStringView name) noexcept;

}  // namespace noctara
