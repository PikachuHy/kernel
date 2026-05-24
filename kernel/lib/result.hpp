// kernel/lib/result.hpp
#pragma once

namespace km {

template <typename T>
class Result {
    // Compile-time check using Clang builtin (no <type_traits> in freestanding).
    static_assert(__is_trivially_destructible(T),
                  "Result<T> supports only trivially-destructible types");

public:
    static auto Ok(T val) noexcept -> Result { return Result(val, true); }
    static auto Err(int error) noexcept -> Result { return Result(error); }

    Result(Result&& other) noexcept : ok_(other.ok_) {
        if (ok_) value_ = other.value_;
        else error_ = other.error_;
    }

    auto operator=(Result&& other) noexcept -> Result& {
        if (this != &other) {
            ok_ = other.ok_;
            if (ok_) value_ = other.value_;
            else error_ = other.error_;
        }
        return *this;
    }

    Result(const Result&) = delete;
    auto operator=(const Result&) = delete;

    explicit operator bool() const noexcept { return ok_; }
    auto value() noexcept -> T& { return value_; }
    auto value() const noexcept -> const T& { return value_; }
    auto take_value() noexcept -> T { return value_; }
    auto error() const noexcept -> int { return error_; }

private:
    union { T value_; int error_; };
    bool ok_;

    Result(T val, bool ok) noexcept : value_(val), ok_(ok) {}
    explicit Result(int err) noexcept : error_(err), ok_(false) {}
};

} // namespace km
