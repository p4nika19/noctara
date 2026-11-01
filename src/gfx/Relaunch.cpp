#include "gfx/Relaunch.hpp"

#include <QFile>

#include <cerrno>
#include <cstdio>
#include <print>
#include <system_error>
#include <unistd.h>

namespace noctara::gfx {

void RelaunchGuard::request(const QString& program, const QStringList& arguments)
{
    m_program = QFile::encodeName(program).toStdString();
    m_arguments.clear();
    for (const QString& argument : arguments) {
        if (argument == u"--safe-mode" || argument == u"--reset-config") {
            continue;
        }
        m_arguments.push_back(QFile::encodeName(argument).toStdString());
    }
}

RelaunchGuard::RelaunchGuard()
{
    for (char* const* entry = environ; *entry != nullptr; ++entry) {
        m_environment.emplace_back(*entry);
    }
}

RelaunchGuard::~RelaunchGuard()
{
    if (!m_program) {
        return;
    }
    std::vector<char*> argv;
    argv.reserve(m_arguments.size() + 2);
    argv.push_back(m_program->data());
    for (std::string& argument : m_arguments) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);
    std::vector<char*> envp;
    envp.reserve(m_environment.size() + 1);
    for (std::string& entry : m_environment) {
        envp.push_back(entry.data());
    }
    envp.push_back(nullptr);
    (void)std::fflush(nullptr);
    ::execve(m_program->c_str(), argv.data(), envp.data());
    // Only reached on failure; the process exits normally afterwards.
    std::println(stderr, "relaunch failed: {}", std::generic_category().message(errno));
}

}  // namespace noctara::gfx
