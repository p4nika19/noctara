#include "cache/LruLedger.hpp"

#include <algorithm>

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

const LruLedger::Item* LruLedger::find(const QByteArray& name) const
{
    const auto it = m_items.constFind(name);
    return it == m_items.cend() ? nullptr : &*it;
}

std::vector<QByteArray> LruLedger::victims(std::int64_t bytesToFree, const QByteArray& keep) const
{
    std::vector<std::pair<std::int64_t, QByteArray>> candidates;
    candidates.reserve(static_cast<std::size_t>(m_items.size()));
    for (auto it = m_items.cbegin(); it != m_items.cend(); ++it) {
        if (it->pins == 0 && it.key() != keep) {
            candidates.emplace_back(it->lastAccess, it.key());
        }
    }
    std::ranges::sort(candidates);  // oldest first; ties broken by name for determinism
    std::vector<QByteArray> chosen;
    std::int64_t freed = 0;
    for (const auto& [lastAccess, name] : candidates) {
        if (bytesToFree >= 0 && freed >= bytesToFree) {
            break;
        }
        freed += m_items.value(name).bytes;
        chosen.push_back(name);
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
