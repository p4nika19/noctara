#pragma once

#include <unistd.h>
#include <utility>

namespace noctara {

// Owning POSIX file descriptor.
class UniqueFd {
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept
        : m_fd(fd)
    {
    }
    ~UniqueFd() { reset(); }

    UniqueFd(UniqueFd&& other) noexcept
        : m_fd(std::exchange(other.m_fd, -1))
    {
    }
    UniqueFd& operator=(UniqueFd&& other) noexcept
    {
        if (this != &other) {
            reset(std::exchange(other.m_fd, -1));
        }
        return *this;
    }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    [[nodiscard]] int get() const noexcept { return m_fd; }
    [[nodiscard]] bool valid() const noexcept { return m_fd >= 0; }
    explicit operator bool() const noexcept { return valid(); }

    [[nodiscard]] int release() noexcept { return std::exchange(m_fd, -1); }

    void reset(int fd = -1) noexcept
    {
        if (m_fd >= 0) {
            ::close(m_fd);
        }
        m_fd = fd;
    }

private:
    int m_fd = -1;
};

}  // namespace noctara
