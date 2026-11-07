#include "cache/LruLedger.hpp"

#include <algorithm>
#include <tuple>

namespace noctara::cache {

void LruLedger::upsert(const QByteArray& name, std::int64_t bytes, std::int64_t lastAccess)
{
    Item& item = m_items[name];
    m_total += bytes - item.bytes;
    item.bytes = bytes;
    item.lastAccess = std::max(item.lastAccess, lastAccess);
}

void LruLedger::addBytes(const QByteArray& name, std::int64_t delta)
{
    const auto it = m_items.find(name);
    if (it != m_items.end()) {
        it->bytes += delta;
        m_total += delta;
    }
}

void LruLedger::touch(const QByteArray& name, std::int64_t now)
{
    const auto it = m_items.find(name);
    if (it != m_items.end()) {
        it->lastAccess = std::max(it->lastAccess, now);
    }
}

void LruLedger::pin(const QByteArray& name)
{
    ++m_items[name].pins;
}

void LruLedger::unpin(const QByteArray& name)
{
    const auto it = m_items.find(name);
    if (it != m_items.end() && it->pins > 0) {
        --it->pins;
    }
}

void LruLedger::remove(const QByteArray& name)
{
    const auto it = m_items.find(name);
    if (it != m_items.end()) {
        m_total -= it->bytes;
        m_items.erase(it);
    }
}

void LruLedger::clear()
{
    m_items.clear();
    m_total = 0;
}

std::vector<QByteArray> LruLedger::victims(std::int64_t bytesToFree, const QByteArray& keep) const
{
    struct Candidate {
        std::int64_t lastAccess;
        QByteArray name;
        std::int64_t bytes;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(static_cast<std::size_t>(m_items.size()));
    for (auto it = m_items.cbegin(); it != m_items.cend(); ++it) {
        if (it->pins == 0 && it.key() != keep) {
            candidates.push_back({it->lastAccess, it.key(), it->bytes});
        }
    }
    // Oldest first; ties broken by name for determinism. A heap pops only the victims actually taken instead of
    // sorting every entry of a full tier (names are unique, so the order is total and matches a full sort).
    const auto later = [](const Candidate& a, const Candidate& b) {
        return std::tie(a.lastAccess, a.name) > std::tie(b.lastAccess, b.name);
    };
    std::ranges::make_heap(candidates, later);
    std::vector<QByteArray> chosen;
    std::int64_t freed = 0;
    while (!candidates.empty()) {
        if (bytesToFree >= 0 && freed >= bytesToFree) {
            break;
        }
        std::ranges::pop_heap(candidates, later);
        freed += candidates.back().bytes;
        chosen.push_back(std::move(candidates.back().name));
        candidates.pop_back();
    }
    return chosen;
}

std::vector<QByteArray> LruLedger::overBudget(std::int64_t budget, const QByteArray& keep) const
{
    if (m_total <= budget) {
        return {};
    }
    return victims(m_total - budget, keep);
}

}  // namespace noctara::cache
