#!/usr/bin/env python3
"""Convert a USBPcap capture (.pcap or .pcapng) into the capture-corpus JSONL
schema (pakon-captures/FORMAT.md), so a Windows capture of either stack - the
OEM TLX stack or pakon-cli - can be diffed with tools/compare_sessions.py.

Emitted events, one JSON object per line after a `meta` line:
  cmd  {hex}                     bulk OUT 0x01 (PPB command), as submitted
  rsp  {hex}                     bulk IN 0x81 completion (PPB reply)
  ep6  {n}                       bulk IN 0x86 completion: byte count only
  ep0  {req,wValue,wIndex,dir,n} vendor control request; payload omitted

Image pixels and EEPROM payloads are never written (same policy as the
corpus). Only USBPcap's link type (DLT_USBPCAP, 249) is understood.

Usage: pcap_to_jsonl.py capture.pcapng -o session.jsonl [--label oem-base4]
       [--device N]   (USB device address; default: the one that uses 0x01)
"""

import argparse
import json
import struct
import sys

DLT_USBPCAP = 249
TRANSFER_CONTROL, TRANSFER_BULK = 2, 3
CMD_OUT, RSP_IN, IMAGE_IN = 0x01, 0x81, 0x86


def read_pcap(data):
    """Yield (timestamp, linktype, packet) from classic pcap or pcapng."""
    if len(data) < 24:
        raise ValueError("file too short for a capture")
    magic = data[:4]
    if magic in (b"\xd4\xc3\xb2\xa1", b"\xa1\xb2\xc3\xd4", b"\x4d\x3c\xb2\xa1", b"\xa1\xb2\x3c\x4d"):
        endian = "<" if magic in (b"\xd4\xc3\xb2\xa1", b"\x4d\x3c\xb2\xa1") else ">"
        nano = magic in (b"\x4d\x3c\xb2\xa1", b"\xa1\xb2\x3c\x4d")
        linktype = struct.unpack(endian + "I", data[20:24])[0]
        off = 24
        while off + 16 <= len(data):
            sec, frac, incl, _orig = struct.unpack(endian + "IIII", data[off:off + 16])
            off += 16
            yield sec + frac / (1e9 if nano else 1e6), linktype, data[off:off + incl]
            off += incl
        return
    if magic != b"\x0a\x0d\x0d\x0a":
        raise ValueError("not a pcap or pcapng file")
    off = 0
    endian = "<"
    interfaces = []  # (linktype, ticks per second)
    while off + 12 <= len(data):
        if data[off:off + 4] == b"\x0a\x0d\x0d\x0a":
            endian = "<" if data[off + 8:off + 12] == b"\x4d\x3c\x2b\x1a" else ">"
            interfaces = []
        btype, blen = struct.unpack(endian + "II", data[off:off + 8])
        if blen < 12 or off + blen > len(data):
            break
        body = data[off + 8:off + blen - 4]
        if btype == 1:  # Interface Description Block
            linktype = struct.unpack(endian + "H", body[0:2])[0]
            resol = 1e6
            opt = 8
            while opt + 4 <= len(body):
                code, olen = struct.unpack(endian + "HH", body[opt:opt + 4])
                if code == 0:
                    break
                if code == 9 and olen >= 1:  # if_tsresol
                    v = body[opt + 4]
                    resol = float(2 ** (v & 0x7F)) if v & 0x80 else float(10 ** v)
                opt += 4 + ((olen + 3) & ~3)
            interfaces.append((linktype, resol))
        elif btype == 6:  # Enhanced Packet Block
            iface, hi, lo, cap, _orig = struct.unpack(endian + "IIIII", body[0:20])
            linktype, resol = interfaces[iface] if iface < len(interfaces) else (None, 1e6)
            yield ((hi << 32) | lo) / resol, linktype, body[20:20 + cap]
        elif btype == 3:  # Simple Packet Block: no timestamp
            linktype, _ = interfaces[0] if interfaces else (None, 1e6)
            yield 0.0, linktype, body[4:]
        off += blen


def parse_usbpcap(pkt):
    """USBPCAP_BUFFER_PACKET_HEADER (packed, little-endian)."""
    if len(pkt) < 27:
        return None
    hlen, _irp, status, _func, info, _bus, device, endpoint, transfer, dlen = struct.unpack(
        "<HQIHBHHBBI", pkt[:27])
    return {
        "completion": bool(info & 0x01),  # PDO -> FDO
        "status": status,
        "device": device,
        "endpoint": endpoint,
        "transfer": transfer,
        "data": pkt[hlen:hlen + dlen],
        "stage": pkt[27] if transfer == TRANSFER_CONTROL and hlen >= 28 else None,
    }


def convert(data, label, device=None):
    records = []
    for ts, linktype, pkt in read_pcap(data):
        if linktype != DLT_USBPCAP:
            continue
        rec = parse_usbpcap(pkt)
        if rec is not None:
            records.append((ts, rec))
    if device is None:
        users = {r["device"] for _, r in records
                 if r["transfer"] == TRANSFER_BULK and r["endpoint"] == CMD_OUT}
        if len(users) > 1:
            raise ValueError("several devices use endpoint 0x01 (%s); pass --device"
                             % sorted(users))
        device = users.pop() if users else None
    events = [{"d": "meta", "t": records[0][0] if records else 0.0, "label": label,
               "bridge": "tools/pcap_to_jsonl.py (USBPcap)", "clock": "capture timestamps",
               "scope": "USB application traffic of device %s; ep6 and ep0 payloads omitted"
                        % device,
               "streams": {"cmd": "bulk OUT 0x01, hex", "rsp": "bulk IN 0x81, hex",
                           "ep6": "bulk IN 0x86, byte count only",
                           "ep0": "vendor control setup + length, payload omitted"}}]
    for ts, r in records:
        if r["device"] != device:
            continue
        t = round(ts, 6)
        if r["transfer"] == TRANSFER_BULK:
            if r["endpoint"] == CMD_OUT and not r["completion"] and r["data"]:
                events.append({"t": t, "d": "cmd", "hex": r["data"].hex()})
            elif r["endpoint"] == RSP_IN and r["completion"] and r["data"]:
                events.append({"t": t, "d": "rsp", "hex": r["data"].hex()})
            elif r["endpoint"] == IMAGE_IN and r["completion"] and r["status"] == 0:
                events.append({"t": t, "d": "ep6", "n": len(r["data"])})
        elif (r["transfer"] == TRANSFER_CONTROL and not r["completion"] and r["stage"] == 0
              and len(r["data"]) >= 8):
            bm, req, wvalue, windex, wlength = struct.unpack("<BBHHH", r["data"][:8])
            if (bm & 0x60) != 0x40:  # vendor requests only
                continue
            inbound = bool(bm & 0x80)
            events.append({"t": t, "d": "ep0", "req": req, "wValue": wvalue, "wIndex": windex,
                           "dir": "in" if inbound else "out", "n": wlength if inbound else 0})
    return events


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("capture")
    ap.add_argument("-o", "--output", default="-")
    ap.add_argument("--label", default="capture")
    ap.add_argument("--device", type=int)
    args = ap.parse_args(argv)
    with open(args.capture, "rb") as f:
        events = convert(f.read(), args.label, args.device)
    out = sys.stdout if args.output == "-" else open(args.output, "w")
    for e in events:
        out.write(json.dumps(e, separators=(",", ":")) + "\n")
    if out is not sys.stdout:
        out.close()
    counts = {}
    for e in events[1:]:
        counts[e["d"]] = counts.get(e["d"], 0) + 1
    print("events: %s" % (", ".join("%s %d" % kv for kv in sorted(counts.items())) or "none"),
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
