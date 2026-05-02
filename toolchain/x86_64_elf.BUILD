load("@rules_cc//cc:cc_toolchain_config_lib.bzl", "tool_path")

package(default_visibility = ["//visibility:public"])

filegroup(name = "empty")

cc_toolchain_config(
    name = "x86_64_elf_config",
    cpu = "x86_64",
    compiler = "clang",
    toolchain_identifier = "x86_64-elf-clang",
    host_system_name = "x86_64-darwin",
    target_system_name = "x86_64-unknown-elf",
    target_libc = "unknown",
    abi_version = "elf",
    abi_libc_version = "unknown",
    target_cpu = "x86_64",
    builtin_sysroot = "/dev/null",
    tool_paths = [
        tool_path(name = "gcc", path = "clang"),
        tool_path(name = "g++", path = "clang++"),
        tool_path(name = "cpp", path = "clang-cpp"),
        tool_path(name = "ar", path = "llvm-ar"),
        tool_path(name = "nm", path = "llvm-nm"),
        tool_path(name = "ld", path = "ld.lld"),
        tool_path(name = "objcopy", path = "llvm-objcopy"),
        tool_path(name = "objdump", path = "llvm-objdump"),
        tool_path(name = "strip", path = "llvm-strip"),
    ],
    cxx_builtin_include_directories = [],
    compile_flags = [
        "-target",
        "x86_64-unknown-elf",
        "-ffreestanding",
        "-nostdlib",
        "-nostdinc",
        "-std=c++26",
        "-fno-exceptions",
        "-fno-rtti",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-mno-red-zone",
        "-mno-mmx",
        "-mno-sse",
        "-mno-sse2",
        "-mgeneral-regs-only",
        "-mcmodel=kernel",
    ],
    dbg_compile_flags = [
        "-g",
        "-O0",
    ],
    opt_compile_flags = [
        "-O2",
        "-DNDEBUG",
    ],
    cxx_flags = [
        "-fno-use-cxa-atexit",
    ],
    link_flags = [
        "-target",
        "x86_64-unknown-elf",
        "-nostdlib",
        "-nostartfiles",
        "-ffreestanding",
        "-Wl,-z,max-page-size=0x1000",
        "-Wl,-build-id=none",
    ],
    dbg_link_flags = [],
    opt_link_flags = [],
)
