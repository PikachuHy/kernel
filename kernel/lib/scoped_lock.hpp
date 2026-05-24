// kernel/lib/scoped_lock.hpp
#pragma once

namespace km {

template <typename Lock>
class ScopedLock {
    Lock& lock_;
public:
    explicit ScopedLock(Lock& l) noexcept : lock_(l) { lock_.lock(); }
    ~ScopedLock() { lock_.unlock(); }
    ScopedLock(const ScopedLock&) = delete;
    auto operator=(const ScopedLock&) = delete;
};

} // namespace km
