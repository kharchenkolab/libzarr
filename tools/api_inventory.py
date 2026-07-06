#!/usr/bin/env python3
"""Generate docs/API.md: the canonical public API surface of libzarr.

The public API is every symbol in `namespace zarr` (including the `zarr::v2`
and `zarr::v3` sub-namespaces) that is NOT inside a namespace whose name starts
with `detail` (`detail`, `detail_shard`, `detail_zip`, `detail_v2`,
`detail_v3`). Those detail namespaces are internal even though they live in
public headers.

The extractor is a self-contained textual parser: it reads only the header text
and depends on no compiler, libclang, or system headers. That makes its output a
pure function of the sources, so CI can regenerate docs/API.md and
`git diff --exit-code` it — the drift guard is byte-stable on every machine, the
way tools/update_vendored.sh guards the vendored headers. It relies on the
repo's enforced clang-format style (one declaration per statement, braces where
clang-format puts them); it is a documentation aid, not a C++ front end.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Header order: dependency/topic, not alphabetical, so the document reads top-down.
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


def strip_comments_and_literals(text):
    """Remove comments and blank out string/char literal contents so their
    braces, semicolons, and quotes never confuse the scanner."""
    text = text.replace("\\\n", "")  # splice line continuations
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                i += 1
            i += 2
        elif c in "\"'":
            quote = c
            out.append(quote)
            i += 1
            while i < n and text[i] != quote:
                i += 2 if text[i] == "\\" else 1
            out.append(quote)
            i += 1
        else:
            out.append(c)
            i += 1
    # Drop preprocessor lines (the public macro contract is documented by hand).
    kept = [ln for ln in "".join(out).split("\n") if not ln.lstrip().startswith("#")]
    return "\n".join(kept)


def norm(s):
    s = " ".join(s.split())
    s = re.sub(r"\binline\b\s*", "", s)  # 'inline' is not part of the surface
    return s.strip()


def clean_sig(sig):
    sig = norm(sig)
    sig = re.sub(r"\)\s*:\s.*$", ")", sig)  # drop a constructor member-init list
    sig = sig.replace("( ", "(").replace(" )", ")").replace(" ,", ",")
    return sig.strip()


def is_boring_member(decl):
    """Deleted/defaulted special members, destructors: not real surface."""
    return ("= delete" in decl or "= default" in decl
            or re.search(r"~\w+\s*\(", decl) is not None)


def match_brace(text, open_idx):
    """Index just past the '}' matching the '{' at open_idx."""
    depth = 0
    i = open_idx
    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return len(text)


def enum_record(head, body):
    m = re.match(r"enum(\s+class)?\s+(\w+)\s*(:\s*([\w:\s]+))?$", head.strip())
    name = m.group(2) if m else "?"
    underlying = norm(m.group(4)) if (m and m.group(4)) else ""
    vals = [p.split("=")[0].strip() for p in body.split(",") if p.split("=")[0].strip()]
    kind = "enum class" if (m and m.group(1)) else "enum"
    head_txt = f"{kind} {name}" + (f" : {underlying}" if underlying else "")
    return f"{head_txt} {{ {', '.join(vals)} }}"


def emitting(stack):
    ns_segs = [s for f in stack if f[0] == "ns" for s in f[1].split("::")]
    if "zarr" not in ns_segs or any(s.startswith("detail") for s in ns_segs):
        return False
    for f in stack:
        if f[0] == "agg" and f[3] not in ("public", "protected"):
            return False
    return True


def classify_member(decl):
    """A class-body declaration -> ('kind', rendered) or None to skip."""
    decl = norm(decl)
    if not decl or decl in ("public", "private", "protected") or is_boring_member(decl):
        return None
    if re.match(r"^(class|struct)\s+\w+$", decl):
        return None  # forward declaration
    if decl.startswith("using "):
        return ("alias", decl)
    if "(" in decl:  # method (declaration or the signature of an inline def)
        return ("method", clean_sig(decl))
    return ("field", norm(decl.split("=")[0]))  # data member: drop initializer


def parse_header(path):
    text = strip_comments_and_literals(open(path).read())
    # Scope frames: ['ns', name, is_detail] or ['agg', kind, (name, bases), access, members]
    stack = []
    symbols = []  # (ns_path, kind, rendered, members|None) in source order
    buf = []
    paren = 0
    i, n = 0, len(text)

    def ns_path():
        return "::".join(f[1] for f in stack if f[0] == "ns" and f[1])

    def record_member(rendered_kind_pair):
        top = stack[-1]
        tag = "" if top[3] == "public" else "[protected] "
        top[4].append(tag + rendered_kind_pair[1])

    def flush_decl():
        decl = norm("".join(buf))
        del buf[:]
        if not decl or not emitting(stack):
            return
        if stack and stack[-1][0] == "agg":
            m = classify_member(decl)
            if m:
                record_member(m)
            return
        if re.match(r"^(class|struct)\s+\w+$", decl) or is_boring_member(decl):
            return
        if decl.startswith("using "):
            symbols.append((ns_path(), "alias", decl, None))
        elif "(" in decl:
            name = decl.split("(")[0].split()[-1] if decl.split("(")[0].split() else ""
            if "::" not in name:  # skip out-of-line member definitions
                symbols.append((ns_path(), "fn", clean_sig(decl), None))
        elif re.search(r"\b(constexpr|const)\b", decl):
            symbols.append((ns_path(), "const", norm(decl.split("=")[0]), None))

    while i < n:
        c = text[i]
        if c == "(":
            paren += 1
            buf.append(c)
            i += 1
            continue
        if c == ")":
            paren = max(0, paren - 1)
            buf.append(c)
            i += 1
            continue
        if paren > 0:  # inside a parameter list: braces/colons/semis are literal
            buf.append(c)
            i += 1
            continue
        if c == ":" and not (text[i - 1: i] == ":" or text[i + 1: i + 2] == ":"):
            if norm("".join(buf)) in ("public", "private", "protected") \
                    and stack and stack[-1][0] == "agg":
                stack[-1][3] = norm("".join(buf))
                del buf[:]
                i += 1
                continue
            buf.append(c)
            i += 1
            continue
        if c == "{":
            head = norm("".join(buf))
            del buf[:]
            magg = re.match(r"^(class|struct)\s+(\w+)\b(.*)$", head)
            if head.startswith("namespace"):
                # One frame per `namespace` statement (matching its single closing
                # brace), even for a concatenated `namespace a::b {`.
                m = re.match(r"^namespace\s+([\w:]+)$", head)
                name = m.group(1) if m else ""  # anonymous namespace -> ""
                is_detail = any(s.startswith("detail") for s in name.split("::"))
                stack.append(["ns", name, is_detail])
            elif magg and "(" not in head:
                rest = re.sub(r"^final\b", "", magg.group(3).strip()).strip()
                bases = ""
                if rest.startswith(":"):
                    bases = norm(re.sub(r"\b(public|private|protected|virtual)\b", "", rest[1:]))
                access = "public" if magg.group(1) == "struct" else "private"
                stack.append(["agg", magg.group(1), (magg.group(2), bases), access, []])
            elif head.startswith("enum"):
                end = match_brace(text, i)
                rendered = enum_record(head, text[i + 1: end - 1])
                if emitting(stack):
                    if stack and stack[-1][0] == "agg":
                        record_member(("enum", rendered))
                    else:
                        symbols.append((ns_path(), "enum", rendered, None))
                i = end
                continue
            elif "(" in head:  # function / constructor body
                if emitting(stack) and not is_boring_member(head):
                    if stack and stack[-1][0] == "agg":
                        m = classify_member(head)
                        if m:
                            record_member(m)
                    else:
                        name = head.split("(")[0].split()[-1]
                        if "::" not in name:
                            symbols.append((ns_path(), "fn", clean_sig(head), None))
                i = match_brace(text, i)
                continue
            else:  # data-member / variable brace initializer: keep the decl, skip braces
                i = match_brace(text, i)
                buf.extend(head)
                continue
            i += 1
            continue
        if c == "}":
            del buf[:]
            if stack:
                frame = stack.pop()
                if frame[0] == "agg" and emitting(stack):
                    name, bases = frame[2]
                    head = f"{frame[1]} {name}" + (f" : {bases}" if bases else "")
                    ns = "::".join(f[1] for f in stack if f[0] == "ns" and f[1])
                    symbols.append((ns, "type", head, frame[4]))
            i += 1
            continue
        if c == ";":
            flush_decl()
            i += 1
            continue
        buf.append(c)
        i += 1
    return symbols


ORDER = {"enum": 0, "alias": 1, "const": 2, "type": 3, "fn": 4}


def render(header, symbols):
    lines = [f"## `{header}`", ""]
    by_ns = {}
    for ns, kind, rendered, members in symbols:
        by_ns.setdefault(ns, []).append((kind, rendered, members))
    for ns in sorted(by_ns):
        lines += [f"### `namespace {ns}`", ""]
        seen = set()
        for kind, rendered, members in sorted(by_ns[ns], key=lambda s: (ORDER[s[0]], s[1])):
            if (kind, rendered) in seen:  # a decl and its out-of-header-order twin
                continue
            seen.add((kind, rendered))
            lines.append(f"- `{rendered}`")
            for m in members or []:
                lines.append(f"  - `{m}`")
        lines.append("")
    return lines


def main():
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
        "- `LIBZARR_ZSTD_DECODE_ONLY` — with zstd, omit the compress side (reader-only build;",
        "  encoding a zstd chunk throws). Lets a consumer link zstd's decompress-only build.",
        "- `LIBZARR_DEPRECATED(msg)` — marks a symbol deprecated (see COMPATIBILITY.md).",
        "",
    ]
    for rel in HEADERS:
        doc += render(rel, parse_header(os.path.join(ROOT, "include", "libzarr", rel)))
    with open(os.path.join(ROOT, "docs", "API.md"), "w") as f:
        f.write("\n".join(doc).rstrip() + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
