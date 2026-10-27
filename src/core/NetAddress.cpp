#include "core/NetAddress.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <netinet/in.h>

namespace noctara {

namespace {

constexpr std::array<std::uint8_t, 12> kV4MappedPrefix{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};

QStringView stripBrackets(QStringView text)
{
    if (text.size() >= 2 && text.front() == u'[' && text.back() == u']') {
        return text.sliced(1, text.size() - 2);
    }
    return text;
}

bool prefixMatches(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b, int bits) noexcept
{
    const auto fullBytes = static_cast<std::size_t>(bits / 8);
    if (!std::equal(a.begin(), a.begin() + static_cast<std::ptrdiff_t>(fullBytes), b.begin())) {
        return false;
    }
    const int rest = bits % 8;
    if (rest == 0) {
        return true;
    }
    const auto mask = static_cast<std::uint8_t>(0xffU << static_cast<unsigned>(8 - rest));
    return (a[fullBytes] & mask) == (b[fullBytes] & mask);
}

std::optional<std::uint16_t> parsePort(QStringView text)
{
    bool ok = false;
    const uint value = text.toUInt(&ok);
    if (!ok || value == 0 || value > 65535) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(value);
}

}  // namespace

std::optional<IpAddress> IpAddress::parse(QStringView text)
{
    const QByteArray latin = stripBrackets(text.trimmed()).toLatin1();
    if (latin.isEmpty() || latin.size() > INET6_ADDRSTRLEN) {
        return std::nullopt;
    }
    IpAddress address;
    if (::inet_pton(AF_INET, latin.constData(), address.m_bytes.data()) == 1) {
        address.m_family = AddressFamily::V4;
        return address;
    }
    if (latin.contains(':') && ::inet_pton(AF_INET6, latin.constData(), address.m_bytes.data()) == 1) {
        address.m_family = AddressFamily::V6;
        return address;
    }
    return std::nullopt;
}

IpAddress IpAddress::fromV4(std::span<const std::uint8_t, 4> bytes) noexcept
{
    IpAddress address;
    address.m_family = AddressFamily::V4;
    std::ranges::copy(bytes, address.m_bytes.begin());
    return address;
}

IpAddress IpAddress::fromV6(std::span<const std::uint8_t, 16> bytes) noexcept
{
    IpAddress address;
    address.m_family = AddressFamily::V6;
    std::ranges::copy(bytes, address.m_bytes.begin());
    return address;
}

std::span<const std::uint8_t> IpAddress::bytes() const noexcept
{
    return std::span(m_bytes).first(m_family == AddressFamily::V4 ? 4 : 16);
}

QString IpAddress::toString() const
{
    std::array<char, INET6_ADDRSTRLEN> buffer{};
    const int af = m_family == AddressFamily::V4 ? AF_INET : AF_INET6;
    if (::inet_ntop(af, m_bytes.data(), buffer.data(), static_cast<socklen_t>(buffer.size())) == nullptr) {
        return {};
    }
    return QString::fromLatin1(buffer.data());
}

QString IpAddress::toUrlHost() const
{
    return m_family == AddressFamily::V6 ? u'[' + toString() + u']' : toString();
}

std::optional<IpAddress> IpAddress::mappedV4() const noexcept
{
    if (m_family != AddressFamily::V6 || !std::equal(kV4MappedPrefix.begin(), kV4MappedPrefix.end(), m_bytes.begin())) {
        return std::nullopt;
    }
    return fromV4(std::span(m_bytes).subspan<12, 4>());
}

bool IpAddress::isLoopback() const noexcept
{
    if (const auto v4 = mappedV4()) {
        return v4->isLoopback();
    }
    if (m_family == AddressFamily::V4) {
        return m_bytes[0] == 127;
    }
    constexpr std::array<std::uint8_t, 16> loopback{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    return m_bytes == loopback;
}

bool IpAddress::isPrivate() const noexcept
{
    if (const auto v4 = mappedV4()) {
        return v4->isPrivate();
    }
    if (m_family == AddressFamily::V4) {
        const std::uint8_t a = m_bytes[0];
        const std::uint8_t b = m_bytes[1];
        return a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) ||
               (a == 100 && b >= 64 && b <= 127);
    }
    return (m_bytes[0] & 0xfeU) == 0xfc;
}

bool IpAddress::isLinkLocal() const noexcept
{
    if (const auto v4 = mappedV4()) {
        return v4->isLinkLocal();
    }
    if (m_family == AddressFamily::V4) {
        return m_bytes[0] == 169 && m_bytes[1] == 254;
    }
    return m_bytes[0] == 0xfe && (m_bytes[1] & 0xc0U) == 0x80;
}

std::optional<Cidr> Cidr::parse(QStringView text)
{
    const QStringView trimmed = text.trimmed();
    const qsizetype slash = trimmed.indexOf(u'/');
    const auto address = IpAddress::parse(slash < 0 ? trimmed : trimmed.first(slash));
    if (!address) {
        return std::nullopt;
    }
    const int maxBits = address->family() == AddressFamily::V4 ? 32 : 128;
    if (slash < 0) {
        return Cidr{*address, maxBits};
    }
    bool ok = false;
    const int prefix = trimmed.sliced(slash + 1).toInt(&ok);
    if (!ok || prefix < 0 || prefix > maxBits) {
        return std::nullopt;
    }
    return Cidr{*address, prefix};
}

bool Cidr::contains(const IpAddress& address) const noexcept
{
    return address.family() == network.family() && prefixMatches(network.bytes(), address.bytes(), prefix);
}

std::optional<Endpoint> Endpoint::parse(QStringView text, std::uint16_t defaultPort)
{
    const QStringView trimmed = text.trimmed();
    if (trimmed.startsWith(u'[')) {
        const qsizetype close = trimmed.indexOf(u']');
        if (close < 0) {
            return std::nullopt;
        }
        const auto address = IpAddress::parse(trimmed.first(close + 1));
        const QStringView rest = trimmed.sliced(close + 1);
        if (!address || address->family() != AddressFamily::V6) {
            return std::nullopt;
        }
        if (rest.isEmpty()) {
            return Endpoint{*address, defaultPort};
        }
        const auto port = rest.startsWith(u':') ? parsePort(rest.sliced(1)) : std::nullopt;
        return port ? std::optional(Endpoint{*address, *port}) : std::nullopt;
    }
    if (trimmed.count(u':') == 1) {
        const qsizetype colon = trimmed.indexOf(u':');
        const auto address = IpAddress::parse(trimmed.first(colon));
        const auto port = parsePort(trimmed.sliced(colon + 1));
        if (!address || !port || address->family() != AddressFamily::V4) {
            return std::nullopt;
        }
        return Endpoint{*address, *port};
    }
    const auto address = IpAddress::parse(trimmed);
    return address ? std::optional(Endpoint{*address, defaultPort}) : std::nullopt;
}

QString Endpoint::toString() const
{
    return address.toUrlHost() + u':' + QString::number(port);
}

QString Endpoint::toString(std::uint16_t defaultPort) const
{
    return port == defaultPort ? address.toString() : toString();
}

bool isValidHostname(QStringView name) noexcept
{
    if (name.endsWith(u'.')) {
        name.chop(1);
    }
    if (name.isEmpty() || name.size() > 253) {
        return false;
    }
    qsizetype labelStart = 0;
    for (qsizetype i = 0; i <= name.size(); ++i) {
        if (i == name.size() || name[i] == u'.') {
            const qsizetype length = i - labelStart;
            if (length < 1 || length > 63 || name[labelStart] == u'-' || name[i - 1] == u'-') {
                return false;
            }
            labelStart = i + 1;
            continue;
        }
        const char16_t c = name[i].unicode();
        const bool allowed =
            (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || (c >= u'0' && c <= u'9') || c == u'-' || c == u'_';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

}  // namespace noctara
