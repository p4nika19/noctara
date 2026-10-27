#pragma once

#include <QStringList>

#include <optional>
#include <string>
#include <vector>

namespace noctara::gfx {

// Declared first in main() so it is destroyed last: if a restart was requested, the destructor
// replaces the (fully shut down) process image with a fresh instance via execv. No child process,
// so nothing can be orphaned.
class RelaunchGuard {
public:
    RelaunchGuard() = default;
    ~RelaunchGuard();

    RelaunchGuard(const RelaunchGuard&) = delete;
    RelaunchGuard& operator=(const RelaunchGuard&) = delete;

    // `arguments` excludes the program name. One-shot flags (--safe-mode, --reset-config) are dropped.
    void request(const QString& program, const QStringList& arguments);
    [[nodiscard]] bool requested() const noexcept { return m_program.has_value(); }

private:
    std::optional<std::string> m_program;
    std::vector<std::string> m_arguments;
};

}  // namespace noctara::gfx
