// kernel/lib/result.hpp
#pragma once
#include <utility>     // std::move
#include <type_traits> // std::is_trivially_destructible_v

namespace km {

template <typename T>
class Result {
    static_assert(std::is_trivially_destructible_v<T>,
                  "Result<T> supports only trivially-destructible types (no heap cleanup)");

public:
    static auto Ok(T val) noexcept -> Result { return Result(std::move(val), true); }
    static auto Err(int error) noexcept -> Result { return Result(error); }

    Result(Result&& other) noexcept : ok_(other.ok_) {
        if (ok_) new (&value_) T(std::move(other.value_));
        else error_ = other.error_;
    }

    auto operator=(Result&& other) noexcept -> Result& {
        if (this != &other) {
            if (ok_) value_.~T();
            ok_ = other.ok_;
            if (ok_) new (&value_) T(std::move(other.value_));
            else error_ = other.error_;
        }
        return *this;
    }

    Result(const Result&) = delete;
    auto operator=(const Result&) = delete;

    ~Result() { if (ok_) value_.~T(); }

    explicit operator bool() const noexcept { return ok_; }
    auto value() noexcept -> T& { return value_; }
    auto value() const noexcept -> const T& { return value_; }
    auto take_value() noexcept -> T&& { return std::move(value_); }
    auto error() const noexcept -> int { return error_; }

private:
    union { T value_; int error_; };
    bool ok_;

    Result(T val, bool ok) noexcept : value_(std::move(val)), ok_(ok) {}
    explicit Result(int err) noexcept : error_(err), ok_(false) {}
};

} // namespace km
