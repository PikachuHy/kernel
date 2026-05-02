#pragma once

[[noreturn]] void kpanic(const char* msg);
[[noreturn]] void kpanic(const char* msg, const char* file, int line);
#define KPANIC(msg) kpanic(msg, __FILE__, __LINE__)
