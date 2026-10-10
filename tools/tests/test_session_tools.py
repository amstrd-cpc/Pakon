#!/usr/bin/env python3
"""Synthetic-data tests for tools/pcap_to_jsonl.py and tools/compare_sessions.py.
Builds USBPcap captures byte by byte (classic pcap and pcapng), converts them,
and diffs sessions with known differences. Run directly (CTest: session_tools)."""

import io
import json
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

import compare_sessions  # noqa: E402
import pcap_to_jsonl  # noqa: E402


def usbpcap(device, endpoint, transfer, data, completion, status=0, stage=None):
    hlen = 28 if stage is not None else 27
    hdr = struct.pack("<HQIHBHHBBI", hlen, 0x1234, status, 0x0009, 1 if completion else 0, 1,
                      device, endpoint, transfer, len(data))
    if stage is not None:
        hdr += bytes([stage])
    return hdr + data


def session_packets(device=5):
    """(t, packet) for: one command + reply, one EEPROM read pair, an
    acquire-on, two image transfers, acquire-off; plus noise from device 9."""
    setup_out = struct.pack("<BBHHH", 0x40, 0xA4, 0x00A5, 0x1234, 0)
    setup_in = struct.pack("<BBHHH", 0xC0, 0xA9, 0x0000, 0x1234, 8)
    return [
        (1.0, usbpcap(device, 0x01, 3, bytes.fromhex("0403100085"), False)),
        (1.001, usbpcap(device, 0x01, 3, b"", True)),  # OUT completion: no event
        (1.002, usbpcap(device, 0x81, 3, bytes.fromhex("07021000"), True)),
        (1.01, usbpcap(device, 0x00, 2, setup_out, False, stage=0)),
        (1.011, usbpcap(device, 0x80, 2, setup_in, False, stage=0)),
        (1.012, usbpcap(device, 0x80, 2, b"\x01" * 8, True, stage=3)),  # payload: dropped
        (2.0, usbpcap(device, 0x01, 3, bytes.fromhex("0206440382006300"), False)),
        (2.1, usbpcap(device, 0x86, 3, b"\x00" * 20480, True)),
        (2.2, usbpcap(device, 0x86, 3, b"\x00" * 4096, True)),
        (2.3, usbpcap(device, 0x86, 3, b"", True, status=0xC0000011)),  # failed: no event
        (2.5, usbpcap(device, 0x01, 3, bytes.fromhex("0206440382006200"), False)),
        (2.6, usbpcap(9, 0x02, 3, b"\xff\xff", False)),  # another device
    ]


def write_pcap(packets):
    out = struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 249)
    for t, p in packets:
        sec = int(t)
        usec = int(round((t - sec) * 1e6))
        out += struct.pack("<IIII", sec, usec, len(p), len(p)) + p
    return out


def block(btype, body):
    pad = (-len(body)) % 4
    total = 12 + len(body) + pad
    return struct.pack("<II", btype, total) + body + b"\x00" * pad + struct.pack("<I", total)


def write_pcapng(packets):
    shb = block(0x0A0D0D0A, struct.pack("<IHHq", 0x1A2B3C4D, 1, 0, -1))
    # IDB with if_tsresol = 9 (nanoseconds)
    opts = struct.pack("<HH", 9, 1) + b"\x09\x00\x00\x00" + struct.pack("<HH", 0, 0)
    idb = block(1, struct.pack("<HHI", 249, 0, 65535) + opts)
    out = shb + idb
    for t, p in packets:
        ticks = int(round(t * 1e9))
        out += block(6, struct.pack("<IIIII", 0, ticks >> 32, ticks & 0xFFFFFFFF, len(p), len(p))
                     + p)
    return out


class PcapToJsonlTest(unittest.TestCase):
    def check(self, events):
        self.assertEqual(events[0]["d"], "meta")
        body = [{k: v for k, v in e.items() if k != "t"} for e in events[1:]]
        self.assertEqual(body, [
            {"d": "cmd", "hex": "0403100085"},
            {"d": "rsp", "hex": "07021000"},
            {"d": "ep0", "req": 0xA4, "wValue": 0xA5, "wIndex": 0x1234, "dir": "out", "n": 0},
            {"d": "ep0", "req": 0xA9, "wValue": 0, "wIndex": 0x1234, "dir": "in", "n": 8},
            {"d": "cmd", "hex": "0206440382006300"},
            {"d": "ep6", "n": 20480},
            {"d": "ep6", "n": 4096},
            {"d": "cmd", "hex": "0206440382006200"},
        ])
        self.assertAlmostEqual(events[1]["t"], 1.0, places=5)
        self.assertAlmostEqual(events[-1]["t"], 2.5, places=5)
        # no payload bytes of image or control data anywhere
        self.assertNotIn("0101010101", json.dumps(events))

    def test_classic_pcap(self):
        self.check(pcap_to_jsonl.convert(write_pcap(session_packets()), "t"))

    def test_pcapng_nanosecond_resolution(self):
        self.check(pcap_to_jsonl.convert(write_pcapng(session_packets()), "t"))

    def test_device_must_be_chosen_when_ambiguous(self):
        packets = session_packets(5) + session_packets(6)
        with self.assertRaises(ValueError):
            pcap_to_jsonl.convert(write_pcap(packets), "t")
        events = pcap_to_jsonl.convert(write_pcap(packets), "t", device=6)
        self.assertEqual(sum(1 for e in events if e["d"] == "cmd"), 3)

    def test_rejects_other_files(self):
        with self.assertRaises(ValueError):
            pcap_to_jsonl.convert(b"not a capture at all, clearly", "t")


def ev(t, d, **kw):
    e = {"t": t, "d": d}
    e.update(kw)
    return e


def oem_like():
    return [
        ev(0.0, "cmd", hex="0403100085"), ev(0.01, "rsp", hex="07021000"),
        ev(0.1, "cmd", hex="030110"), ev(0.2, "cmd", hex="0103400102"),
        ev(0.3, "ep0", req=0xA4, wValue=0xA5, wIndex=0x1234, dir="out", n=0),
        ev(1.0, "cmd", hex="02084005810600030009"),
        ev(1.01, "cmd_mod", hex="02084005810400030009"),
        ev(2.0, "cmd", hex="0206440382006300"),
        ev(2.5, "ep6", n=1000000), ev(3.0, "ep6", n=1000000),
        ev(3.0, "cmd", hex="0206440382006200"),
    ]


class CompareSessionsTest(unittest.TestCase):
    def run_compare(self, a, b, **kw):
        out = io.StringIO()
        n = compare_sessions.compare(a, b, out=out, **kw)
        return n, out.getvalue()

    def test_identical_sessions_and_masked_polls(self):
        a = oem_like()
        b = [e for e in oem_like() if e.get("hex") not in ("030110", "0103400102")]
        n, text = self.run_compare(a, b)
        self.assertEqual(n, 0, text)
        self.assertIn("A#0: 2000000 B in 1.000 s (2.00 MB/s)", text)
        n, _ = self.run_compare(a, b, keep_polls=True)
        self.assertGreater(n, 0)

    def test_cmd_mod_is_what_reached_the_scanner(self):
        b = [e for e in oem_like() if e["d"] != "cmd_mod"]
        for e in b:
            if e.get("hex") == "02084005810600030009":
                e["hex"] = "02084005810400030009"
        n, text = self.run_compare(oem_like(), b)
        self.assertEqual(n, 0, text)

    def test_reports_changed_frame_window_and_register(self):
        b = oem_like()[:-1] + [ev(3.0, "cmd", hex="0206440382f00010")]
        n, text = self.run_compare(oem_like(), b)
        self.assertGreater(n, 0)
        self.assertIn("A - 0206440382006200", text)
        self.assertIn("B + 0206440382f00010", text)
        self.assertIn("B#0: 2000000 B, never ended", text)
        self.assertIn("registers written by B only: WRITE 44 reg 0x82 sub f0", text)

    def test_reports_control_request_differences(self):
        b = [e for e in oem_like() if e["d"] != "ep0"]
        b.append(ev(0.3, "ep0", req=0xA9, wValue=0x808, wIndex=0x1234, dir="in", n=8))
        n, text = self.run_compare(oem_like(), b)
        self.assertGreater(n, 0)
        self.assertIn("A only: req 0xa4 wValue 0x00a5", text)
        self.assertIn("B only: req 0xa9 wValue 0x0808", text)

    def test_cli_round_trip(self):
        with tempfile.TemporaryDirectory() as d:
            cap = os.path.join(d, "c.pcapng")
            js = os.path.join(d, "c.jsonl")
            with open(cap, "wb") as f:
                f.write(write_pcapng(session_packets()))
            self.assertEqual(pcap_to_jsonl.main([cap, "-o", js, "--label", "x"]), 0)
            self.assertEqual(compare_sessions.main([js, js]), 0)


if __name__ == "__main__":
    unittest.main()
