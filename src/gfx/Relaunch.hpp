#pragma once

#include <QStringList>

#include <optional>
#include <string>
#include <vector>

namespace noctara::gfx {

// Declared first in main() so it is destroyed last: if a restart was requested, the destructor
// replaces the (fully shut down) process image with a fresh instance via execve. No child process,
// so nothing can be orphaned. The new instance gets the environment as it was when the guard was
// created, not the variables the graphics bootstrap set since (DRI_PRIME, QT_VK_PHYSICAL_DEVICE_INDEX),
// which it would take for the user's own and keep despite a changed GPU preference.
class RelaunchGuard {
public:
    RelaunchGuard();
    ~RelaunchGuard();

    RelaunchGuard(const RelaunchGuard&) = delete;
    RelaunchGuard& operator=(const RelaunchGuard&) = delete;

    // `arguments` excludes the program name. One-shot flags (--safe-mode, --reset-config) are dropped.
    void request(const QString& program, const QStringList& arguments);
    [[nodiscard]] bool requested() const noexcept { return m_program.has_value(); }
    // The environment the relaunched process gets.
    [[nodiscard]] const std::vector<std::string>& environment() const noexcept { return m_environment; }

private:
    std::optional<std::string> m_program;
    std::vector<std::string> m_arguments;
    std::vector<std::string> m_environment;
};

}  // namespace noctara::gfx
