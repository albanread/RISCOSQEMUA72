#!/usr/bin/env python3
"""Generate HostFS's extension->filetype table from ROOL's allocation list.

ROOL publish every allocated RISC OS filetype at

    https://www.riscosopen.org/wiki/documentation/show/File%20Types

with a status, a short name, a description, and — the useful part — a
column of the filename extensions and MIME types the rest of the world
uses for the same data.  That column is the mapping HostFS needs, already
curated by the people who allocate the numbers, so it is generated rather
than transcribed (riscos-pi4/FSDESIGN-V1.md Sec 6.3.1).

    mktypemap.py --fetch -o typemap.txt
    mktypemap.py filetypes.html -o typemap.txt

Output is `ext<TAB>type<TAB>name<TAB>description`, one line per
extension, sorted; comments carry the provenance.  Point the device at
it with -global bcm2838-peripherals.vmchannel-typemap=<file>.
"""
import argparse
import html
import re
import sys
from collections import defaultdict

URL = "https://www.riscosopen.org/wiki/documentation/show/File%20Types"

# Types whose double-click action is "run this".  An extension is a guess,
# and a guess must never tell the desktop that an arbitrary host file is
# code: .mod is a RISC OS module to us and a ProTracker song to everyone
# else.  Only an explicit ,xxx suffix or a stored override may yield one
# of these.  See FSDESIGN-V1.md Sec 6.2.
EXECUTABLE = {"FFA", "FF8", "FFC", "FEB", "FFB", "FFE"}

# Status glyphs, best first.  Used to settle an extension claimed twice.
STATUS_RANK = {"✔": 0,    # OS-defined
               "✓": 1,    # Registered
               "": 2,          # unknown
               "✗": 3,    # Unregistered
               "✘": 9}    # Do not use - never emitted


# ROOL allocate types, not extensions, so the list has no row for a C
# source file — it is simply Text.  These are the everyday development
# extensions that carry no type of their own; all of them are &FFF, which
# is both correct and safe.  Marked separately in the output so it stays
# obvious which lines came from ROOL and which are ours.
LOCAL = {
    "c": "C source", "h": "C header", "cpp": "C++ source",
    "cc": "C++ source", "hpp": "C++ header", "s": "Assembler source",
    "py": "Python source", "rs": "Rust source", "go": "Go source",
    "js": "JavaScript source", "ts": "TypeScript source",
    "sh": "Shell script", "mk": "Makefile fragment",
    "json": "JSON data", "yaml": "YAML data", "yml": "YAML data",
    "toml": "TOML data", "ini": "Configuration", "cfg": "Configuration",
    "md": "Markdown text", "rst": "reStructuredText", "log": "Log file",
    "diff": "Patch", "patch": "Patch", "csv": "Comma-separated values",
}


def parse(text):
    rows = re.findall(r"<tr[^>]*>(.*?)</tr>", text, re.S | re.I)
    out = []
    for r in rows:
        cells = [html.unescape(re.sub(r"<[^>]*>", " ", c)).strip()
                 for c in re.findall(r"<t[dh][^>]*>(.*?)</t[dh]>", r, re.S | re.I)]
        cells = [re.sub(r"\s+", " ", c) for c in cells]
        if len(cells) >= 6 and re.fullmatch(r"[0-9A-Fa-f]{3}", cells[1]):
            out.append({"status": cells[0], "type": cells[1].upper(),
                        "name": cells[2], "desc": cells[3],
                        "app": cells[4], "mime": cells[5]})
    return out


def extensions(mime_field):
    """The column mixes bare extensions with MIME types; keep the former."""
    for tok in mime_field.split():
        tok = tok.strip().lstrip(".").lower()
        if not tok or "/" in tok:          # image/png and friends
            continue
        if re.fullmatch(r"[a-z0-9][a-z0-9+_-]{0,9}", tok):
            yield tok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source", nargs="?", help="saved wiki HTML")
    ap.add_argument("--fetch", action="store_true", help="download it instead")
    ap.add_argument("-o", "--out", default="-")
    args = ap.parse_args()

    if args.fetch:
        import urllib.request
        with urllib.request.urlopen(URL, timeout=60) as f:
            text = f.read().decode("utf-8", "replace")
    elif args.source:
        text = open(args.source, encoding="utf-8", errors="replace").read()
    else:
        ap.error("give a saved HTML file or --fetch")

    rows = parse(text)
    if not rows:
        sys.exit("no filetype rows found: has the wiki's table markup changed?")

    claims = defaultdict(list)
    for row in rows:
        if STATUS_RANK.get(row["status"], 2) >= 9:      # do not use
            continue
        if row["type"] in EXECUTABLE:
            continue
        for ext in extensions(row["mime"]):
            claims[ext].append(row)

    chosen, contested = {}, []
    for ext, rs in sorted(claims.items()):
        if len(rs) > 1:
            # Prefer the better status, then a type whose short name is
            # the extension itself (zip -> Zip, not Archive).
            rs = sorted(rs, key=lambda r: (STATUS_RANK.get(r["status"], 2),
                                           r["name"].lower() != ext))
            contested.append((ext, rs))
        chosen[ext] = rs[0]

    lines = [
        "# HostFS extension -> RISC OS filetype.",
        "#",
        "# Generated by riscos-pi4/tools/mktypemap.py from ROOL's filetype",
        "# allocation list: " + URL,
        "# Edit freely — this file is read at startup, and a hand-made entry",
        "# is as good as a generated one.  Regenerate to pick up new",
        "# allocations; your edits are not preserved, so keep them below in",
        "# a block of their own if you regenerate often.",
        "#",
        "# Executable types (&FFA Module, &FF8 Absolute, &FFC Utility,",
        "# &FEB Obey, &FFB BASIC, &FFE Command) are deliberately absent: an",
        "# extension is a guess, and a guess must not tell the desktop that",
        "# a host file is code.  Only a ,xxx suffix may say that.",
        "#",
        "# ext\ttype\tname\tdescription",
    ]
    for ext, row in sorted(chosen.items()):
        lines.append("%s\t%s\t%s\t%s" % (ext, row["type"], row["name"],
                                         row["desc"] or row["name"]))

    lines += ["",
              "# Not from ROOL: development extensions that have no type of",
              "# their own because they are simply text.",
              "# ext\ttype\tname\tdescription"]
    for ext, desc in sorted(LOCAL.items()):
        if ext in chosen:
            continue                    # ROOL's answer wins
        lines.append("%s\tFFF\tText\t%s" % (ext, desc))

    text_out = "\n".join(lines) + "\n"
    if args.out == "-":
        sys.stdout.write(text_out)
    else:
        open(args.out, "w", encoding="utf-8").write(text_out)

    print("%d extensions from %d allocated types" % (len(chosen), len(rows)),
          file=sys.stderr)
    if contested:
        print("%d extensions were claimed more than once; chosen first:"
              % len(contested), file=sys.stderr)
        for ext, rs in contested[:20]:
            print("  .%-6s %s" % (ext, "  ".join(
                "%s(%s)" % (r["type"], r["name"]) for r in rs)), file=sys.stderr)


if __name__ == "__main__":
    main()
