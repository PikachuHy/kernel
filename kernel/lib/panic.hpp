#pragma once

[[noreturn]] auto kpanic(const char* msg) -> void;
[[noreturn]] auto kpanic(const char* msg, const char* file, int line) -> void;
#define KPANIC(msg) kpanic(msg, __FILE__, __LINE__)
