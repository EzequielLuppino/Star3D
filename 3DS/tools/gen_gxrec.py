#!/usr/bin/env python3
"""Generate the GX recorder wrappers from gx/gxrec.list (MULTICORE_PLAN.md step 2).

    gen_gxrec.py gx/gxrec.list out_dir
    gen_gxrec.py --names gx/gxrec.list   (prints the wrapped names, for -Wl,--wrap)

Writes out_dir/gxrec_gen.h (opcodes, encoder prototypes) and
out_dir/gxrec_gen.c (the --wrap wrappers for cmd/sync entries, the encoders
gxrec_put_<name>, the replay switch and the opcode names). The list format is
described at the top of gxrec.list; the runtime (ring buffer, render thread,
hand-written wrappers) is gx/gx_rec.c.
"""

import os
import re
import sys

# By-value structs: size in 32-bit words
STRUCTS = {"GXColor": 1, "GXColorS10": 2}
FLOATS = {"f32", "float"}
KINDS = {"cmd", "sync", "manual", "wrap"}


class Arg:
    def __init__(self, text):
        self.blob = None  # Size expression: copy the pointed-to bytes at record time
        m = re.match(r"^(.*?)@(\{.*\}|\d+)\s*$", text)
        if m:
            text = m.group(1)
            self.blob = m.group(2).strip("{}")
        m = re.match(r"^(.*?)([A-Za-z_]\w*)\s*$", text.strip())
        if not m:
            raise ValueError("cannot parse argument %r" % text)
        self.type = " ".join(m.group(1).split()).replace(" *", "*")
        self.name = m.group(2)
        base = self.type.replace("const ", "").strip()
        if self.blob is not None:
            self.kind = "blob"
            if "*" not in self.type:
                raise ValueError("@size on a non-pointer argument %r" % text)
        elif "*" in self.type:
            self.kind = "ptr"
        elif base in FLOATS:
            self.kind = "float"
        elif base in STRUCTS:
            self.kind = "struct"
            self.words = STRUCTS[base]
        else:
            self.kind = "int"

    def decl(self):
        return "%s %s" % (self.type, self.name)


class Entry:
    def __init__(self, kind, proto, line):
        m = re.match(r"^(.*?)\b([A-Za-z_]\w*)\s*\((.*)\)\s*$", proto)
        if not m:
            raise ValueError("line %d: cannot parse prototype %r" % (line, proto))
        self.kind = kind
        self.ret = " ".join(m.group(1).split())
        self.name = m.group(2)
        argtext = m.group(3).strip()
        self.args = [] if argtext in ("", "void") else [Arg(a) for a in split_args(argtext)]
        self.op = "GXREC_OP_" + self.name

    def params(self):
        return ", ".join(a.decl() for a in self.args) or "void"

    def call_args(self):
        return ", ".join(a.name for a in self.args)

    def has_command(self):
        return self.kind in ("cmd", "manual")


def split_args(text):
    out, depth, cur = [], 0, ""
    for ch in text:
        if ch in "({[":
            depth += 1
        elif ch in ")}]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    out.append(cur)
    return [a.strip() for a in out]


def parse(path):
    entries = []
    with open(path, encoding="utf-8") as f:
        for n, raw in enumerate(f, 1):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            kind, _, proto = line.partition(" ")
            if kind not in KINDS:
                raise ValueError("line %d: unknown kind %r" % (n, kind))
            entries.append(Entry(kind, proto.strip(), n))
    names = [e.name for e in entries]
    dup = {x for x in names if names.count(x) > 1}
    if dup:
        raise ValueError("listed twice: " + ", ".join(sorted(dup)))
    return entries


def encoder_body(e, indent):
    """Code that records e's arguments as one command (uses the parameter names)."""
    i = indent
    out = []
    words = []
    for k, a in enumerate(e.args):
        if a.kind == "blob":
            out.append("%su32 n%d = ((u32)(%s) + 3) / 4;" % (i, k, a.blob))
            words.append("1 + n%d" % k)
        elif a.kind == "struct":
            words.append(str(a.words))
        else:
            words.append("1")
    out.append("%su32* p = gxrec_begin(%s, %s);" % (i, e.op, " + ".join(words) or "0"))
    for k, a in enumerate(e.args):
        if a.kind == "int":
            out.append("%s*p++ = (u32)%s;" % (i, a.name))
        elif a.kind == "ptr":
            out.append("%s*p++ = (u32)(uintptr_t)%s;" % (i, a.name))
        elif a.kind == "float":
            out.append("%smemcpy(p++, &%s, 4);" % (i, a.name))
        elif a.kind == "struct":
            out.append("%smemcpy(p, &%s, sizeof(%s));" % (i, a.name, a.name))
            out.append("%sp += %d;" % (i, a.words))
        else:
            out.append("%s*p++ = n%d;" % (i, k))
            out.append("%smemcpy(p, %s, n%d * 4);" % (i, a.name, k))
            out.append("%sp += n%d;" % (i, k))
    out.append("%sgxrec_end(p);" % i)
    return out


def replay_case(e):
    out = ["    case %s: {" % e.op]
    for a in e.args:
        if a.kind == "int":
            out.append("        %s %s = (%s)*p++;" % (a.type, a.name, a.type))
        elif a.kind == "ptr":
            out.append("        %s %s = (%s)(uintptr_t)*p++;" % (a.type, a.name, a.type))
        elif a.kind == "float":
            out.append("        %s %s;" % (a.type, a.name))
            out.append("        memcpy(&%s, p++, 4);" % a.name)
        elif a.kind == "struct":
            out.append("        %s %s;" % (a.type, a.name))
            out.append("        memcpy(&%s, p, sizeof(%s));" % (a.name, a.name))
            out.append("        p += %d;" % a.words)
        else:
            out.append("        u32 n_%s = *p++;" % a.name)
            out.append("        %s %s = (%s)(uintptr_t)p;" % (a.type, a.name, a.type))
            out.append("        p += n_%s;" % a.name)
    out.append("        (void)p;")
    out.append("        __real_%s(%s);" % (e.name, e.call_args()))
    out.append("        return true;")
    out.append("    }")
    return out


def generate(entries):
    cmds = [e for e in entries if e.has_command()]
    h = []
    h.append("// Generated by tools/gen_gxrec.py from gx/gxrec.list. Do not edit.")
    h.append("#ifndef GXREC_GEN_H")
    h.append("#define GXREC_GEN_H")
    h.append("")
    h.append("enum {")
    h.append("    GXREC_OP_FIRST_GENERATED = GXREC_OP_RESERVED,")
    for k, e in enumerate(cmds):
        h.append("    %s%s," % (e.op, " = GXREC_OP_FIRST_GENERATED" if k == 0 else ""))
    h.append("    GXREC_OP_COUNT")
    h.append("};")
    h.append("")
    for e in entries:
        h.append("%s __real_%s(%s);" % (e.ret, e.name, e.params()))
    h.append("")
    for e in entries:
        if e.kind == "manual":
            h.append("void gxrec_put_%s(%s);" % (e.name, e.params()))
    h.append("")
    h.append("bool gxrec_replay_generated(u32 op, const u32* p);")
    h.append("extern const char* const gxrec_op_names[GXREC_OP_COUNT];")
    h.append("")
    h.append("#endif")

    c = []
    c.append("// Generated by tools/gen_gxrec.py from gx/gxrec.list. Do not edit.")
    c.append("#include <stdint.h>")
    c.append("#include <string.h>")
    c.append('#include "gx_internal.h"')
    c.append('#include "gx_rec.h"')
    c.append('#include "gxrec_gen.h"')
    c.append('#include "ptag.h"')
    c.append("")
    c.append("// Same prototypes as the definitions: a mismatch with a header is a compile error")
    for e in entries:
        c.append("%s %s(%s);" % (e.ret, e.name, e.params()))
    c.append("")
    for e in entries:
        if e.kind == "cmd":
            c.append("void __wrap_%s(%s) {" % (e.name, e.params()))
            c.append("    PTAG_LEAF(PL_GXCMD);")
            c.append("    if (!gxrec_recording()) {")
            c.append("        __real_%s(%s);" % (e.name, e.call_args()))
            c.append("        return;")
            c.append("    }")
            c.extend(encoder_body(e, "    "))
            c.append("}")
            c.append("")
        elif e.kind == "manual":
            c.append("void gxrec_put_%s(%s) {" % (e.name, e.params()))
            c.extend(encoder_body(e, "    "))
            c.append("}")
            c.append("")
        elif e.kind == "sync":
            void = e.ret == "void"
            c.append("%s __wrap_%s(%s) {" % (e.ret, e.name, e.params()))
            c.append("    PTAG_LEAF(PL_GXCMD);")
            c.append("    if (!gxrec_recording()) %s__real_%s(%s);" % ("" if void else "return ", e.name, e.call_args()))
            if void:
                c[-1] = c[-1].replace(");", ");")
                c.append("    else {")
                c.append('        static int sync_id = -1;')
                c.append('        gxrec_sync_direct_begin("rt.sync.%s", &sync_id);' % e.name)
                c.append("        __real_%s(%s);" % (e.name, e.call_args()))
                c.append("        gxrec_sync_direct_end();")
                c.append("    }")
            else:
                c.append('    static int sync_id = -1;')
                c.append('    gxrec_sync_direct_begin("rt.sync.%s", &sync_id);' % e.name)
                c.append("    %s r = __real_%s(%s);" % (e.ret, e.name, e.call_args()))
                c.append("    gxrec_sync_direct_end();")
                c.append("    return r;")
            c.append("}")
            c.append("")
    c.append("bool gxrec_replay_generated(u32 op, const u32* p) {")
    c.append("    switch (op) {")
    for e in cmds:
        c.extend(replay_case(e))
    c.append("    default:")
    c.append("        return false;")
    c.append("    }")
    c.append("}")
    c.append("")
    c.append("const char* const gxrec_op_names[GXREC_OP_COUNT] = {")
    c.append("    GXREC_OP_RESERVED_NAMES")
    for e in cmds:
        c.append('    [%s] = "%s",' % (e.op, e.name))
    c.append("};")
    return "\n".join(h) + "\n", "\n".join(c) + "\n"


def write_if_changed(path, text):
    try:
        with open(path, encoding="utf-8") as f:
            if f.read() == text:
                return
    except OSError:
        pass
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--names":
        print(" ".join(e.name for e in parse(sys.argv[2])))
        return 0
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    entries = parse(sys.argv[1])
    h, c = generate(entries)
    os.makedirs(sys.argv[2], exist_ok=True)
    write_if_changed(os.path.join(sys.argv[2], "gxrec_gen.h"), h)
    write_if_changed(os.path.join(sys.argv[2], "gxrec_gen.c"), c)
    return 0


if __name__ == "__main__":
    sys.exit(main())
