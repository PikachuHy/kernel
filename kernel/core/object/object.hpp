#pragma once
#include <stdint.h>

class KernelObject {
public:
    enum class Type : uint8_t {
        Channel,
        Port,
        // Future: Process, Thread, VMO, Interrupt, Resource
    };

    Type type() const { return type_; }
    uint32_t refcount() const { return ref_count_; }

    void AddRef() { ref_count_++; }
    void Release();

protected:
    explicit KernelObject(Type t) : type_(t), ref_count_(1) {}
    virtual ~KernelObject() = default;

private:
    Type type_;
    uint32_t ref_count_;
};
