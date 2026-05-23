"""Rule for embedding binary files as ELF objects via llvm-objcopy.

Produces a .o file with _binary_<basename>_bin_start/end/size symbols
that can be linked into the kernel for embedded ring-3 programs.
"""

def _embed_binary_impl(ctx):
    out = ctx.actions.declare_file(ctx.attr.name + ".o")
    bin_name = ctx.attr.basename + ".bin"

    ctx.actions.run_shell(
        inputs = [ctx.file.src],
        outputs = [out],
        tools = [ctx.executable._objcopy],
        command = 'out_abs="$PWD/{out}" && objcopy_abs="$PWD/{objcopy}" && cp "{src}" "{out_dir}/{bin}" && cd "{out_dir}" && "$objcopy_abs" -I binary -O elf64-x86-64 -B i386:x86-64 "{bin}" "$out_abs"'.format(
            src = ctx.file.src.path,
            out_dir = out.dirname,
            bin = bin_name,
            out = out.path,
            objcopy = ctx.executable._objcopy.path,
        ),
    )
    return [DefaultInfo(files = depset([out]))]

embed_binary = rule(
    implementation = _embed_binary_impl,
    attrs = {
        "basename": attr.string(mandatory = True),
        "src": attr.label(allow_single_file = True, mandatory = True),
        "_objcopy": attr.label(
            allow_single_file = True,
            executable = True,
            cfg = "exec",
            default = Label("//toolchain:objcopy.sh"),
        ),
    },
)
