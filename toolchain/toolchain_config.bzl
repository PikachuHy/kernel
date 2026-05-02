"""Custom cc_toolchain_config rule for x86_64-elf cross-compilation."""

load("@rules_cc//cc:action_names.bzl", "ALL_CC_COMPILE_ACTION_NAMES", "ALL_CC_LINK_ACTION_NAMES")
load("@rules_cc//cc:cc_toolchain_config_lib.bzl", "feature", "flag_group", "flag_set", "tool_path")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/toolchains:cc_toolchain_config_info.bzl", "CcToolchainConfigInfo")

def _impl(ctx):
    features = []

    # Compile flags (default_compile_flags feature)
    compile_flags = ctx.attr.compile_flags + ctx.attr.cxx_flags
    if compile_flags:
        features.append(feature(
            name = "default_compile_flags",
            enabled = True,
            flag_sets = [
                flag_set(
                    actions = ALL_CC_COMPILE_ACTION_NAMES,
                    flag_groups = [flag_group(flags = compile_flags)],
                ),
            ],
        ))

    # Debug compile flags
    dbg_compile_flags = ctx.attr.dbg_compile_flags
    if dbg_compile_flags:
        features.append(feature(
            name = "dbg_compile_flags",
            flag_sets = [
                flag_set(
                    actions = ALL_CC_COMPILE_ACTION_NAMES,
                    flag_groups = [flag_group(flags = dbg_compile_flags)],
                ),
            ],
        ))

    # Opt compile flags
    opt_compile_flags = ctx.attr.opt_compile_flags
    if opt_compile_flags:
        features.append(feature(
            name = "opt_compile_flags",
            flag_sets = [
                flag_set(
                    actions = ALL_CC_COMPILE_ACTION_NAMES,
                    flag_groups = [flag_group(flags = opt_compile_flags)],
                ),
            ],
        ))

    # Link flags (default_link_flags feature)
    link_flags = ctx.attr.link_flags
    if link_flags:
        features.append(feature(
            name = "default_link_flags",
            enabled = True,
            flag_sets = [
                flag_set(
                    actions = ALL_CC_LINK_ACTION_NAMES,
                    flag_groups = [flag_group(flags = link_flags)],
                ),
            ],
        ))

    return cc_common.create_cc_toolchain_config_info(
        ctx = ctx,
        toolchain_identifier = ctx.attr.toolchain_identifier,
        host_system_name = ctx.attr.host_system_name,
        target_system_name = ctx.attr.target_system_name,
        target_cpu = ctx.attr.target_cpu,
        target_libc = ctx.attr.target_libc,
        compiler = ctx.attr.compiler,
        abi_version = ctx.attr.abi_version,
        abi_libc_version = ctx.attr.abi_libc_version,
        tool_paths = [tool_path(name = k, path = v) for k, v in ctx.attr.tool_paths.items()],
        cxx_builtin_include_directories = ctx.attr.cxx_builtin_include_directories,
        builtin_sysroot = ctx.attr.builtin_sysroot,
        features = features,
    )

cc_toolchain_config = rule(
    implementation = _impl,
    attrs = {
        "toolchain_identifier": attr.string(mandatory = True),
        "host_system_name": attr.string(mandatory = True),
        "target_system_name": attr.string(mandatory = True),
        "target_cpu": attr.string(mandatory = True),
        "target_libc": attr.string(mandatory = True),
        "compiler": attr.string(mandatory = True),
        "abi_version": attr.string(mandatory = True),
        "abi_libc_version": attr.string(mandatory = True),
        "tool_paths": attr.string_dict(
            mandatory = True,
            doc = "Dictionary mapping tool name to tool path",
        ),
        "cxx_builtin_include_directories": attr.string_list(),
        "builtin_sysroot": attr.string(),
        "compile_flags": attr.string_list(),
        "dbg_compile_flags": attr.string_list(),
        "opt_compile_flags": attr.string_list(),
        "cxx_flags": attr.string_list(),
        "link_flags": attr.string_list(),
        "dbg_link_flags": attr.string_list(),
        "opt_link_flags": attr.string_list(),
    },
    provides = [CcToolchainConfigInfo],
)
