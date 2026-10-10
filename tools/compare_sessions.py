#!/usr/bin/env python3
"""Diff two USB sessions in the capture-corpus JSONL schema (an OEM capture
and a pakon-cli capture, both via tools/pcap_to_jsonl.py, or a corpus file).

What is compared:
  1. the command sequence (what reached the scanner: `cmd_mod` replaces the
     preceding `cmd`), aligned with difflib. Status polls and service reads
     (030110, READ 0x02, ack 0x06, READ 0x83/0x84/0x88/0x90) depend on timing,
     so they are dropped unless --keep-polls;
  2. vendor control requests (EEPROM reads), as (req, wValue, wIndex, dir);
  3. image windows: EP6 bytes between each FPGA acquire-on and acquire-off
     (PICM 0x82 sub0 bit0), with duration and rate;
  4. registers written by one session and never by the other.

Exit status: 0 = no differences after masking, 1 = differences, 2 = usage.

Usage: compare_sessions.py oem.jsonl ours.jsonl [--keep-polls] [--context N]
"""

import argparse
import difflib
import json
import sys

POLL_PREFIXES = ("030110", "030140", "030144")


def load(path):
    events = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                events.append(json.loads(line))
    return [e for e in events if e.get("d") != "meta"]


def is_poll(h):
    if h.startswith(POLL_PREFIXES):
        return True
    # READ 01 03 <addr> <n> <reg> of the service/status registers
    if h.startswith("0103") and len(h) == 10 and h[8:10] in ("02", "83", "84", "88", "90"):
        return True
    # interrupt ack: WRITE 02 05 <addr> 02 06 00 <status>
    return h.startswith("0205") and h[6:10] == "0206"


def commands(events, keep_polls):
    out = []
    for e in events:
        if e["d"] == "cmd":
            out.append([e["t"], e["hex"]])
        elif e["d"] == "cmd_mod" and out:
            out[-1][1] = e["hex"]
    return [(t, h) for t, h in out if keep_polls or not is_poll(h)]


def acquire_state(h):
    """True/False when `h` writes PICM 0x82 sub0 (FPGA control), else None."""
    if h.startswith("0206440382") and len(h) == 16 and h[10:12] == "00":
        return bool(int(h[12:14], 16) & 0x01)
    return None


def windows(events):
    result, start, nbytes = [], None, 0
    for e in events:
        if e["d"] in ("cmd", "cmd_mod"):
            state = acquire_state(e["hex"])
            if state is True and start is None:
                start, nbytes = e["t"], 0
            elif state is False and start is not None:
                result.append((start, e["t"], nbytes))
                start = None
        elif e["d"] == "ep6" and start is not None:
            nbytes += e["n"]
    if start is not None:
        result.append((start, None, nbytes))
    return result


def written_registers(cmds):
    regs = set()
    for _, h in cmds:
        if h.startswith("02") and len(h) >= 10:
            reg = h[8:10]
            # PICM 0x82 (FPGA) and 0x84 (A/D) are banked by a sub-register
            sub = (h[10:12] if h[4:6] in ("44", "24") and reg in ("82", "84") and len(h) >= 12
                   else "")
            regs.add("WRITE %s reg 0x%s%s" % (h[4:6], reg, " sub " + sub if sub else ""))
        elif h.startswith("04") and len(h) >= 8:
            regs.add("CMD %s 0x%s" % (h[4:6], h[6:8]))
    return regs


def compare(a_events, b_events, keep_polls=False, context=3, out=sys.stdout):
    differences = 0
    a_cmds, b_cmds = commands(a_events, keep_polls), commands(b_events, keep_polls)
    a_hex, b_hex = [h for _, h in a_cmds], [h for _, h in b_cmds]
    print("commands: A %d, B %d%s" % (len(a_hex), len(b_hex),
                                      "" if keep_polls else " (polls/service reads masked)"),
          file=out)
    matcher = difflib.SequenceMatcher(a=a_hex, b=b_hex, autojunk=False)
    for tag, i1, i2, j1, j2 in matcher.get_opcodes():
        if tag == "equal":
            continue
        differences += 1
        ta = "%.3f" % a_cmds[i1][0] if i1 < len(a_cmds) else "end"
        tb = "%.3f" % b_cmds[j1][0] if j1 < len(b_cmds) else "end"
        print("  %s A[%d:%d] (t=%s) vs B[%d:%d] (t=%s)" % (tag, i1, i2, ta, j1, j2, tb), file=out)
        for h in a_hex[max(i1 - context, 0):i1]:
            print("      = %s" % h, file=out)
        for h in a_hex[i1:i2]:
            print("    A - %s" % h, file=out)
        for h in b_hex[j1:j2]:
            print("    B + %s" % h, file=out)

    def ep0(events):
        return [(e["req"], e["wValue"], e["wIndex"], e["dir"]) for e in events if e["d"] == "ep0"]

    a0, b0 = ep0(a_events), ep0(b_events)
    print("control requests: A %d, B %d" % (len(a0), len(b0)), file=out)
    if a0 != b0:
        differences += 1
        for r in sorted(set(a0) ^ set(b0)):
            side = "A only" if r in set(a0) else "B only"
            print("  %s: req 0x%02x wValue 0x%04x wIndex 0x%04x %s" % ((side,) + r), file=out)
        if set(a0) == set(b0):
            print("  same requests, different order or count", file=out)

    aw, bw = windows(a_events), windows(b_events)
    print("image windows: A %d, B %d" % (len(aw), len(bw)), file=out)
    for label, ws in (("A", aw), ("B", bw)):
        for k, (t0, t1, n) in enumerate(ws):
            if t1 is None:
                print("  %s#%d: %d B, never ended (acquire still on)" % (label, k, n), file=out)
            else:
                d = t1 - t0
                print("  %s#%d: %d B in %.3f s (%.2f MB/s)" % (label, k, n, d,
                                                               n / d / 1e6 if d > 0 else 0),
                      file=out)
    if len(aw) != len(bw):
        differences += 1

    ra, rb = written_registers(a_cmds), written_registers(b_cmds)
    for side, regs in (("A only", ra - rb), ("B only", rb - ra)):
        if regs:
            differences += 1
            print("registers written by %s: %s" % (side, ", ".join(sorted(regs))), file=out)
    print("differences: %d" % differences, file=out)
    return differences


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("a", help="reference session (e.g. the OEM capture)")
    ap.add_argument("b", help="session under test (e.g. pakon-cli)")
    ap.add_argument("--keep-polls", action="store_true")
    ap.add_argument("--context", type=int, default=3)
    args = ap.parse_args(argv)
    n = compare(load(args.a), load(args.b), args.keep_polls, args.context)
    return 1 if n else 0


if __name__ == "__main__":
    sys.exit(main())
