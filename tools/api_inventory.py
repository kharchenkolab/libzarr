#!/usr/bin/env python3
"""Generate docs/API.md: the canonical public API surface of libzarr.

The public API is every symbol in `namespace zarr` (including the `zarr::v2`
and `zarr::v3` sub-namespaces) that is NOT inside a `detail`, `detail_shard`,
or `detail_zip` namespace. Those detail namespaces are internal even though
they live in public headers.

Output is deterministic (sorted), so CI can regenerate it and `git diff
--exit-code`: any change to the public surface — a removed symbol, a changed
signature, a dropped [[nodiscard]] — shows up as a diff and fails the build,
exactly the way tools/update_vendored.sh guards the vendored headers.

Requires libclang (`pip install libclang`). Clang builtin headers and the
libstdc++ search path are discovered automatically.
"""
import glob
import os
import subprocess
import sys
import tempfile

import clang.cindex as cx

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Any namespace named detail* is internal (detail, detail_shard, detail_zip,
# detail_v2, detail_v3), even when it sits inside a public header.
def is_detail_ns(name):
    return name.startswith("detail")
# Order public headers by dependency/topic rather than alphabetically so the
# document reads top-down.
# codecs_{gzip,blosc,zstd}.hpp are excluded: their contents live entirely in
# namespace zarr::detail (internal machinery pulled in by codecs.hpp behind the
# feature flags), and they #error without an external codec library present.
# The public codec surface is the factories in metadata.hpp plus codecs.hpp.
HEADERS = [
    "types.hpp",
    "store.hpp",
    "metadata.hpp",
    "codecs.hpp",
    "array.hpp",
    "group.hpp",
    "sharding.hpp",
    "v2.hpp",
    "v3.hpp",
    "zip.hpp",
    "adapters/filesystem_store.hpp",
]


# detail/common.hpp includes <nmmintrin.h> for the hardware CRC on x86. The
# bundled libclang has no resource dir, so it would fall through to GCC's SSE
# intrinsic headers, which it cannot parse — and that failure silently derails
# std:: type resolution (std::vector<...> decays to int). Shadowing the header
# with a stub of just the two intrinsics common.hpp uses keeps a native 64-bit
# parse clean, so signatures resolve exactly.
_NMMINTRIN_STUB = """#pragma once
extern "C" {
unsigned int _mm_crc32_u8(unsigned int, unsigned char);
unsigned int _mm_crc32_u16(unsigned int, unsigned short);
unsigned int _mm_crc32_u32(unsigned int, unsigned int);
unsigned long long _mm_crc32_u64(unsigned long long, unsigned long long);
}
"""


def clang_args():
    stub_dir = tempfile.mkdtemp(prefix="libzarr_api_")
    with open(os.path.join(stub_dir, "nmmintrin.h"), "w") as f:
        f.write(_NMMINTRIN_STUB)
    args = ["-std=c++17", "-x", "c++",
            "-I" + stub_dir,
            "-I" + os.path.join(ROOT, "include"),
            "-I" + os.path.join(ROOT, "third_party")]
    for d in sorted(glob.glob("/usr/lib/llvm-*/lib/clang/*/include")) + \
            sorted(glob.glob("/usr/lib/clang/*/include")):
        args += ["-isystem", d]
    try:
        v = subprocess.run(["g++", "-E", "-x", "c++", "-v", "/dev/null"],
                           capture_output=True, text=True).stderr
        cap = False
        for ln in v.splitlines():
            if "#include <...> search starts here" in ln:
                cap = True
                continue
            if "End of search list" in ln:
                break
            if cap:
                args += ["-isystem", ln.strip()]
    except FileNotFoundError:
        pass
    return args


def has_nodiscard(cursor):
    # [[nodiscard]] surfaces as a WARN_UNUSED_RESULT_ATTR child cursor (its
    # tokens sit before the declaration's own extent, so a token scan misses it).
    return any(ch.kind == cx.CursorKind.WARN_UNUSED_RESULT_ATTR
               for ch in cursor.get_children())


def norm(sig):
    return " ".join(sig.split())


def method_sig(c):
    parts = []
    if c.is_static_method():
        parts.append("static")
    if c.is_pure_virtual_method():
        parts.append("virtual")
    elif c.is_virtual_method():
        parts.append("virtual")
    if has_nodiscard(c):
        parts.insert(0, "[[nodiscard]]")
    ret = c.result_type.spelling
    if ret:
        parts.append(ret)
    args = ", ".join(a.type.spelling for a in c.get_arguments())
    tail = ""
    if c.is_const_method():
        tail += " const"
    if c.is_pure_virtual_method():
        tail += " = 0"
    return norm(" ".join(parts) + " " + c.spelling + "(" + args + ")" + tail)


def free_fn_sig(c):
    prefix = "[[nodiscard]] " if has_nodiscard(c) else ""
    args = ", ".join(a.type.spelling for a in c.get_arguments())
    return norm(prefix + c.result_type.spelling + " " + c.spelling + "(" + args + ")")


def record_class(c):
    kind = "struct" if c.kind == cx.CursorKind.STRUCT_DECL else "class"
    bases = [b.type.spelling for b in c.get_children()
             if b.kind == cx.CursorKind.CXX_BASE_SPECIFIER]
    head = f"{kind} {c.spelling}"
    if bases:
        head += " : " + ", ".join(bases)
    members = set()
    for m in c.get_children():
        if m.access_specifier not in (cx.AccessSpecifier.PUBLIC,
                                      cx.AccessSpecifier.PROTECTED):
            continue
        tag = "" if m.access_specifier == cx.AccessSpecifier.PUBLIC else "[protected] "
        if m.kind == cx.CursorKind.CXX_METHOD:
            if m.is_deleted_method() or m.is_default_method():
                continue  # deleted/defaulted special members are not real surface
            members.add(tag + method_sig(m))
        elif m.kind == cx.CursorKind.CONSTRUCTOR:
            # Skip compiler-ish special members; keep user-facing constructors.
            if (m.is_deleted_method() or m.is_default_method()
                    or m.is_copy_constructor() or m.is_move_constructor()):
                continue
            args = ", ".join(a.type.spelling for a in m.get_arguments())
            members.add(tag + norm(f"{c.spelling}({args})"))
        elif m.kind == cx.CursorKind.FIELD_DECL:
            members.add(tag + norm(f"{m.type.spelling} {m.spelling}"))
        elif m.kind in (cx.CursorKind.TYPEDEF_DECL, cx.CursorKind.TYPE_ALIAS_DECL):
            members.add(tag + norm(f"using {m.spelling} = {m.underlying_typedef_type.spelling}"))
        elif m.kind == cx.CursorKind.ENUM_DECL and m.spelling:
            members.add(tag + record_enum(m))
    return head, sorted(members)


def record_enum(c):
    underlying = c.enum_type.spelling
    vals = [e.spelling for e in c.get_children() if e.kind == cx.CursorKind.ENUM_CONSTANT_DECL]
    name = c.spelling or "<anonymous>"
    return norm(f"enum class {name} : {underlying} {{ {', '.join(vals)} }}")


def walk(cursor, header_path, ns_stack, out):
    for c in cursor.get_children():
        if c.location.file is None or os.path.abspath(c.location.file.name) != header_path:
            # Only symbols *defined in this header*, not pulled in via includes.
            if c.kind != cx.CursorKind.NAMESPACE:
                continue
        if c.kind == cx.CursorKind.NAMESPACE:
            if is_detail_ns(c.spelling):
                continue
            walk(c, header_path, ns_stack + [c.spelling], out)
            continue
        ns = "::".join(ns_stack)
        if c.kind in (cx.CursorKind.CLASS_DECL, cx.CursorKind.STRUCT_DECL) and c.is_definition():
            head, members = record_class(c)
            out.append((ns, "type", c.spelling, head, members))
        elif c.kind == cx.CursorKind.ENUM_DECL and c.is_definition() and c.spelling:
            out.append((ns, "enum", c.spelling, record_enum(c), []))
        elif c.kind == cx.CursorKind.FUNCTION_DECL:
            out.append((ns, "fn", c.spelling, free_fn_sig(c), []))
        elif c.kind in (cx.CursorKind.TYPEDEF_DECL, cx.CursorKind.TYPE_ALIAS_DECL):
            out.append((ns, "alias", c.spelling,
                        norm(f"using {c.spelling} = {c.underlying_typedef_type.spelling}"), []))
        elif c.kind == cx.CursorKind.VAR_DECL and (
                "constexpr" in [t.spelling for t in c.get_tokens()]):
            out.append((ns, "const", c.spelling, norm(f"{c.type.spelling} {c.spelling}"), []))


def render(header, symbols):
    lines = [f"## `{header}`", ""]
    by_ns = {}
    for ns, kind, name, sig, members in symbols:
        by_ns.setdefault(ns, []).append((kind, name, sig, members))
    for ns in sorted(by_ns):
        lines.append(f"### `namespace {ns}`")
        lines.append("")
        order = {"enum": 0, "alias": 1, "const": 2, "type": 3, "fn": 4}
        seen = set()
        for kind, name, sig, members in sorted(by_ns[ns], key=lambda s: (order[s[0]], s[1], s[2])):
            if sig in seen:  # a free function's declaration and definition both appear
                continue
            seen.add(sig)
            lines.append(f"- `{sig}`")
            for m in members:
                lines.append(f"  - `{m}`")
        lines.append("")
    return lines


def main():
    args = clang_args()
    index = cx.Index.create()
    doc = [
        "# Public API surface",
        "",
        "> Generated by `tools/api_inventory.py` — do not edit by hand.",
        "> Lists every symbol in `namespace zarr` (and `zarr::v2` / `zarr::v3`)",
        "> that is not inside a `detail*` namespace. This is the frozen 1.0",
        "> contract; a change here is an API change (see [COMPATIBILITY.md](COMPATIBILITY.md)).",
        "",
        "## Version and feature macros",
        "",
        "- `LIBZARR_VERSION_MAJOR`, `LIBZARR_VERSION_MINOR`, `LIBZARR_VERSION_PATCH`",
        "- `LIBZARR_HAS_ZLIB`, `LIBZARR_HAS_BLOSC`, `LIBZARR_HAS_ZSTD` — set when the",
        "  matching codec is compiled in; a codec header `#error`s without its flag.",
        "- `LIBZARR_EXTERNAL_JSON` — use an external nlohmann/json instead of the vendored copy.",
        "",
    ]
    had_error = False
    for rel in HEADERS:
        path = os.path.join(ROOT, "include", "libzarr", rel)
        tu = index.parse(path, args=args)
        # Only errors originating in our own headers matter. Parsing GCC's SSE
        # intrinsic headers (pulled by detail/common.hpp's hardware CRC) under
        # libclang emits errors inside <emmintrin.h>; those are system-header
        # noise that does not touch any public declaration we extract.
        errs = [d for d in tu.diagnostics
                if d.severity >= cx.Diagnostic.Error and d.location.file
                and "include/libzarr" in os.path.abspath(d.location.file.name)]
        if errs:
            had_error = True
            sys.stderr.write(f"{rel}: {len(errs)} parse error(s):\n")
            for d in errs[:5]:
                sys.stderr.write(f"  {d.spelling}\n")
        symbols = []
        walk(tu.cursor, os.path.abspath(path), [], symbols)
        doc += render(rel, symbols)
    if had_error:
        sys.stderr.write("api_inventory: refusing to write with parse errors\n")
        return 1
    with open(os.path.join(ROOT, "docs", "API.md"), "w") as f:
        f.write("\n".join(doc).rstrip() + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
