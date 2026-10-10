#!/usr/bin/env python3
"""Build stand-in DLLs for OEM image-processing dependencies that are not
part of the scanner path (ekjpegi.dll, KODAKCMS.dll, xerces-c_2_2_0.dll,
imported by PakonIMAu.dll), so TLB.dll's initialisation can load
PakonIMAu.dll under Wine for the dynamic RE runs (docs/OEM_RE.md §11).

Every symbol the importer takes from a stubbed DLL is exported by name; each
export prints "STUB <dll>!<name>" to stderr and returns 0 if it is ever
called, so a run that reaches one is visible in the client's stderr.

Usage: make_stubs.py IMPORTER.dll OUTDIR dll1.dll [dll2.dll ...]
Needs i686-w64-mingw32-objdump/-gcc (MINGW_BIN to override their directory).
"""

import os
import re
import subprocess
import sys

BIN = os.environ.get("MINGW_BIN", "/home/linuxbrew/.linuxbrew/bin")


def imports(importer):
    out = subprocess.run([os.path.join(BIN, "i686-w64-mingw32-objdump"), "-p", importer],
                         check=True, capture_output=True, text=True).stdout
    table, current = {}, None
    for line in out.splitlines():
        m = re.match(r"\s*DLL Name: (\S+)", line)
        if m:
            current = m.group(1).lower()
            table.setdefault(current, [])
            continue
        m = re.match(r"\s*[0-9a-f]+\s+\S+\s+[0-9a-f]+\s+(\S+)$", line)
        if current and m and not re.fullmatch(r"[0-9a-f]+", m.group(1)):
            table[current].append(m.group(1))
    return table


def build(dll, names, outdir):
    base = os.path.splitext(dll)[0]
    c = ['#include <stdio.h>']
    d = ["LIBRARY %s" % dll, "EXPORTS"]
    for i, name in enumerate(names):
        c.append('int stub_%d(void) { fprintf(stderr, "STUB %s!%s\\n"); return 0; }'
                 % (i, dll, name.replace("\\", "\\\\").replace("%", "%%")))
        d.append('    "%s" = stub_%d' % (name, i))
    src = os.path.join(outdir, base + ".c")
    deff = os.path.join(outdir, base + ".def")
    with open(src, "w") as f:
        f.write("\n".join(c) + "\n")
    with open(deff, "w") as f:
        f.write("\n".join(d) + "\n")
    subprocess.run([os.path.join(BIN, "i686-w64-mingw32-gcc"), "-shared", "-O1", "-o",
                    os.path.join(outdir, dll), src, deff], check=True)
    print("%s: %d exports" % (dll, len(names)))


def main(argv):
    if len(argv) < 4:
        print(__doc__)
        return 2
    importer, outdir, wanted = argv[1], argv[2], argv[3:]
    os.makedirs(outdir, exist_ok=True)
    table = imports(importer)
    for dll in wanted:
        names = table.get(dll.lower())
        if names is None:
            print("%s: not imported by %s" % (dll, importer))
            return 1
        build(dll, names, outdir)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
