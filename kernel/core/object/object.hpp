#pragma once
#include <stdint.h>

class KernelObject {
public:
    enum class Type : uint8_t {
        Channel,
        Port,
        Process,
        Vmo,
    };

    auto type() const noexcept -> Type { return type_; }
    auto refcount() const noexcept -> uint32_t { return ref_count_; }

    auto AddRef() noexcept -> void { ref_count_++; }
    auto Release() -> void;

protected:
    explicit KernelObject(Type t) noexcept : type_(t), ref_count_(1) {}
    virtual ~KernelObject() = default;

private:
    Type type_;
    uint32_t ref_count_;
};
