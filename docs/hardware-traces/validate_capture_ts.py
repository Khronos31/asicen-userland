#!/usr/bin/env python3
"""Independent 188-byte MPEG-TS framing, continuity, PAT, and PMT validator."""

import argparse
import json
import sys
from collections import Counter, defaultdict
from pathlib import Path

TS_SIZE = 188
SYNC = 0x47
CRC_POLY = 0x04C11DB7


def mpeg2_crc32(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for octet in data:
        crc ^= octet << 24
        for _ in range(8):
            crc = ((crc << 1) ^ CRC_POLY) & 0xFFFFFFFF if crc & 0x80000000 else (crc << 1) & 0xFFFFFFFF
    return crc


def crc_ok(section: bytes) -> bool:
    return len(section) >= 4 and mpeg2_crc32(section) == 0


class SectionAssembler:
    """Reassemble PSI sections for one PID; incomplete leading data is ignored."""

    def __init__(self):
        self.pending = bytearray()
        self.pending_start_packet = None
        self.sections = []
        self.section_spans = []
        self.truncated = 0
        self.truncated_events = []

    def _discard_pending(self, packet_index=None, reason="incomplete_at_next_start"):
        if self.pending:
            self.truncated += 1
            self.truncated_events.append({"start_packet": self.pending_start_packet,
                                          "end_packet": packet_index,
                                          "reason": reason})
            self.pending.clear()
            self.pending_start_packet = None

    def _parse_new_sections(self, data: bytes, packet_index=None):
        pos = 0
        while pos < len(data):
            if data[pos] == 0xFF:
                return
            if len(data) - pos < 3:
                self.pending = bytearray(data[pos:])
                self.pending_start_packet = packet_index
                return
            section_length = ((data[pos + 1] & 0x0F) << 8) | data[pos + 2]
            total = 3 + section_length
            if section_length < 4 or total > 4096:
                self.truncated += 1
                return
            if len(data) - pos < total:
                self.pending = bytearray(data[pos:])
                self.pending_start_packet = packet_index
                return
            self.sections.append(bytes(data[pos:pos + total]))
            self.section_spans.append({"start_packet": packet_index,
                                       "end_packet": packet_index})
            pos += total

    def _continue_pending(self, data: bytes, packet_index=None) -> bytes:
        pos = 0
        while self.pending and pos < len(data):
            target = 3 if len(self.pending) < 3 else self._pending_total()
            take = min(target - len(self.pending), len(data) - pos)
            self.pending.extend(data[pos:pos + take])
            pos += take
            if len(self.pending) >= 3:
                total = self._pending_total()
                if total < 7 or total > 4096:
                    self.truncated += 1
                    self.pending.clear()
                    self.pending_start_packet = None
                    return b""
                if len(self.pending) == total:
                    self.sections.append(bytes(self.pending))
                    self.section_spans.append({
                        "start_packet": self.pending_start_packet,
                        "end_packet": packet_index})
                    self.pending.clear()
                    self.pending_start_packet = None
                    return data[pos:]
        return b""

    def feed(self, payload: bytes, payload_start: bool, packet_index=None):
        if not payload:
            return
        if payload_start:
            pointer = payload[0]
            if pointer > len(payload) - 1:
                self._discard_pending(packet_index, "pointer_out_of_range")
                self.truncated += 1
                self.truncated_events.append({"start_packet": packet_index,
                                              "end_packet": packet_index,
                                              "reason": "pointer_out_of_range"})
                return
            prefix = payload[1:1 + pointer]
            if self.pending:
                self._continue_pending(prefix, packet_index)
            # A PUSI pointer may start at a section boundary; any uncompleted
            # old section is a discarded leading fragment, not a bad section.
            self._discard_pending(packet_index)
            self._parse_new_sections(payload[1 + pointer:], packet_index)
        elif self.pending:
            rest = self._continue_pending(payload, packet_index)
            if not self.pending and rest:
                self._parse_new_sections(rest, packet_index)

    def _pending_total(self) -> int:
        if len(self.pending) < 3:
            return 3
        length = ((self.pending[1] & 0x0F) << 8) | self.pending[2]
        return 3 + length


def find_alignment(data: bytes):
    candidates = []
    for offset in range(min(TS_SIZE, len(data))):
        count = 0
        pos = offset
        while pos < len(data) and data[pos] == SYNC:
            count += 1
            pos += TS_SIZE
        if count:
            candidates.append((count, -offset, offset))
    if not candidates:
        raise ValueError("no 188-byte sync alignment found")
    count, _, offset = max(candidates)
    if count < 2 and len(data) >= TS_SIZE * 2:
        raise ValueError("fewer than two aligned sync bytes")
    return offset, count


def parse_ts_packet(packet: bytes):
    if len(packet) != TS_SIZE or packet[0] != SYNC:
        return None
    tei = (packet[1] >> 7) & 1
    pusi = (packet[1] >> 6) & 1
    pid = ((packet[1] & 0x1F) << 8) | packet[2]
    scrambling = (packet[3] >> 6) & 3
    adaptation_control = (packet[3] >> 4) & 3
    cc = packet[3] & 0x0F
    if adaptation_control == 0:
        return {"pid": pid, "tei": tei, "pusi": pusi, "scrambling": scrambling,
                "cc": cc, "invalid_adaptation_control": True, "payload": b"",
                "has_payload": False, "adaptation_only": False,
                "discontinuity": False}
    pos = 4
    discontinuity = False
    if adaptation_control & 2:
        adaptation_length = packet[pos]
        end = pos + 1 + adaptation_length
        if end > TS_SIZE:
            return {"pid": pid, "tei": tei, "pusi": pusi, "scrambling": scrambling,
                    "cc": cc, "invalid_adaptation_control": False, "payload": b"",
                    "has_payload": False, "adaptation_only": adaptation_control == 2,
                    "discontinuity": False, "invalid_adaptation": True}
        if adaptation_length:
            discontinuity = bool(packet[pos + 1] & 0x80)
        pos = end
    has_payload = bool(adaptation_control & 1) and pos < TS_SIZE
    return {"pid": pid, "tei": tei, "pusi": pusi, "scrambling": scrambling,
            "cc": cc, "invalid_adaptation_control": False, "payload": packet[pos:] if has_payload else b"",
            "has_payload": has_payload, "adaptation_only": adaptation_control == 2,
            "discontinuity": discontinuity}


def continuity_stats(pid_packets):
    last_cc = None
    last_packet = None
    stats = {"discontinuities": 0, "duplicates": 0, "duplicate_cc_conflicts": 0,
             "discontinuity_resets": 0}
    events = []
    for pkt, raw, packet_index in pid_packets:
        if pkt.get("invalid_adaptation") or pkt["invalid_adaptation_control"]:
            continue
        if pkt["discontinuity"]:
            stats["discontinuity_resets"] += 1
            last_cc = None
            last_packet = None
        if not pkt["has_payload"]:
            continue
        cc = pkt["cc"]
        if last_cc is not None:
            expected = (last_cc + 1) & 0x0F
            if cc == last_cc:
                if raw == last_packet:
                    stats["duplicates"] += 1
                    events.append({"packet": packet_index, "kind": "duplicate",
                                   "cc": cc, "tei": pkt["tei"]})
                else:
                    stats["duplicate_cc_conflicts"] += 1
                    events.append({"packet": packet_index,
                                   "kind": "duplicate_cc_conflict", "cc": cc,
                                   "tei": pkt["tei"]})
            elif cc != expected:
                stats["discontinuities"] += 1
                events.append({"packet": packet_index, "kind": "cc_discontinuity",
                               "cc": cc, "expected_cc": expected})
        last_cc = cc
        last_packet = raw
    stats["events"] = events
    return stats


def parse_pat(section: bytes):
    result = {"table_id": section[0] if section else None,
              "crc_valid": crc_ok(section), "programs": []}
    if len(section) < 12 or section[0] != 0x00:
        result["structure_valid"] = False
        return result
    section_length = ((section[1] & 0x0F) << 8) | section[2]
    result["structure_valid"] = (3 + section_length == len(section) and
                                 (section_length - 9) % 4 == 0)
    if not result["structure_valid"]:
        return result
    result.update({"transport_stream_id": (section[3] << 8) | section[4],
                   "version": (section[5] >> 1) & 0x1F,
                   "section_number": section[6],
                   "last_section_number": section[7]})
    for pos in range(8, len(section) - 4, 4):
        program = (section[pos] << 8) | section[pos + 1]
        pid = ((section[pos + 2] & 0x1F) << 8) | section[pos + 3]
        if program:
            result["programs"].append({"program_number": program, "pmt_pid": pid})
    return result


def parse_pmt(section: bytes):
    result = {"table_id": section[0] if section else None,
              "crc_valid": crc_ok(section), "streams": []}
    if len(section) < 16 or section[0] != 0x02:
        result["structure_valid"] = False
        return result
    section_length = ((section[1] & 0x0F) << 8) | section[2]
    end = 3 + section_length
    if end != len(section):
        result["structure_valid"] = False
        return result
    program_info_length = ((section[10] & 0x0F) << 8) | section[11]
    pos = 12 + program_info_length
    limit = len(section) - 4
    if pos > limit:
        result["structure_valid"] = False
        return result
    streams = []
    while pos < limit:
        if pos + 5 > limit:
            result["structure_valid"] = False
            return result
        stream_type = section[pos]
        elementary_pid = ((section[pos + 1] & 0x1F) << 8) | section[pos + 2]
        es_info_length = ((section[pos + 3] & 0x0F) << 8) | section[pos + 4]
        pos += 5
        if pos + es_info_length > limit:
            result["structure_valid"] = False
            return result
        streams.append({"stream_type": stream_type, "elementary_pid": elementary_pid,
                        "es_info_length": es_info_length})
        pos += es_info_length
    result.update({"structure_valid": True,
                   "program_number": (section[3] << 8) | section[4],
                   "version": (section[5] >> 1) & 0x1F,
                   "section_number": section[6],
                   "last_section_number": section[7],
                   "pcr_pid": ((section[8] & 0x1F) << 8) | section[9],
                   "streams": streams})
    return result


def validate_bytes(data: bytes):
    offset, sync_run = find_alignment(data)
    usable = (len(data) - offset) // TS_SIZE
    trailing_bytes = len(data) - offset - usable * TS_SIZE
    pid_packets = defaultdict(list)
    pid_counts = defaultdict(lambda: Counter())
    assemblers = {0: SectionAssembler()}
    raw_packets = []
    invalid_packets = 0
    invalid_packet_indices = []
    tei_packet_indices = []
    scrambling_counts = Counter()
    for idx in range(usable):
        start = offset + idx * TS_SIZE
        raw = data[start:start + TS_SIZE]
        pkt = parse_ts_packet(raw)
        if pkt is None:
            invalid_packets += 1
            invalid_packet_indices.append(idx)
            continue
        pid = pkt["pid"]
        raw_packets.append((pkt, raw, idx))
        pid_packets[pid].append((pkt, raw, idx))
        counts = pid_counts[pid]
        counts["packets"] += 1
        counts["tei"] += pkt["tei"]
        counts["scrambled_packets"] += pkt["scrambling"] != 0
        scrambling_counts[str(pkt["scrambling"])] += 1
        counts["payload_packets"] += pkt["has_payload"]
        counts["adaptation_only_packets"] += pkt["adaptation_only"]
        counts["discontinuity_indicators"] += pkt["discontinuity"]
        counts["invalid_adaptation"] += pkt.get("invalid_adaptation", False)
        counts["invalid_adaptation_control"] += pkt["invalid_adaptation_control"]
        if pkt["tei"]:
            tei_packet_indices.append(idx)
        if pid == 0 and pkt["has_payload"]:
            assemblers[0].feed(pkt["payload"], bool(pkt["pusi"]), idx)

    pat_sections = assemblers[0].sections
    pats = [parse_pat(sec) for sec in pat_sections]
    for pat, span in zip(pats, assemblers[0].section_spans):
        pat["packet_span"] = span
    pmt_pids = sorted({entry["pmt_pid"] for pat in pats if pat.get("structure_valid") and
                       pat.get("crc_valid") for entry in pat["programs"]})
    unique_program_map = {}
    conflicting_program_numbers = []
    for pat in pats:
        if not pat.get("structure_valid") or not pat.get("crc_valid"):
            continue
        for entry in pat["programs"]:
            number = entry["program_number"]
            previous = unique_program_map.get(number)
            if previous is not None and previous != entry["pmt_pid"]:
                conflicting_program_numbers.append(number)
            unique_program_map[number] = entry["pmt_pid"]
    for pid in pmt_pids:
        assemblers[pid] = SectionAssembler()
    # Revisit packets after PAT has identified PMT PIDs.
    for pkt, raw, packet_index in raw_packets:
        pid = pkt["pid"]
        if pid in assemblers and pid != 0 and pkt["has_payload"]:
            assemblers[pid].feed(pkt["payload"], bool(pkt["pusi"]), packet_index)
    pmts = []
    for pid in pmt_pids:
        sections = [parse_pmt(sec) for sec in assemblers[pid].sections]
        for section, span in zip(sections, assemblers[pid].section_spans):
            section["pmt_pid"] = pid
            section["packet_span"] = span
        pmts.extend(sections)

    pid_summary = {}
    for pid in sorted(pid_counts):
        counts = pid_counts[pid]
        cc = (continuity_stats(pid_packets[pid]) if pid != 0x1FFF else
              {"discontinuities": 0, "duplicates": 0,
               "duplicate_cc_conflicts": 0, "discontinuity_resets": 0,
               "events": []})
        counts.update({key: value for key, value in cc.items() if key != "events"})
        counts["continuity_events"] = cc["events"]
        pid_summary[f"0x{pid:04x}"] = dict(counts)
    def packet_segment(start_index, end_index):
        entries = [row for row in raw_packets if start_index <= row[2] < end_index]
        per_pid = defaultdict(list)
        for pkt, raw, packet_index in entries:
            per_pid[pkt["pid"]].append((pkt, raw, packet_index))
        cc = Counter()
        for pid, rows in per_pid.items():
            if pid == 0x1FFF:
                continue
            cc.update({k: v for k, v in continuity_stats(rows).items()
                       if k != "events"})
        invalid_slots = [idx for idx in invalid_packet_indices
                         if start_index <= idx < end_index]
        tei = [idx for idx in tei_packet_indices if start_index <= idx < end_index]
        scrambled_count = sum(pkt["scrambling"] != 0 for pkt, _, _ in entries)
        return {"start_packet_inclusive": start_index,
                "end_packet_exclusive": min(end_index, usable),
                "valid_packets": len(entries), "invalid_sync_packets": len(invalid_slots),
                "invalid_sync_packet_indices": invalid_slots,
                "tei_packets": len(tei), "tei_packet_indices": tei,
                "scrambled_packets": scrambled_count,
                "scrambling_control_counts": {
                    str(value): sum(pkt["scrambling"] == value
                                    for pkt, _, _ in entries)
                    for value in range(4)},
                "payload_packets": sum(pkt["has_payload"] for pkt, _, _ in entries),
                "adaptation_only_packets": sum(pkt["adaptation_only"]
                                                for pkt, _, _ in entries),
                "discontinuity_indicators": sum(pkt["discontinuity"]
                                                for pkt, _, _ in entries),
                "continuity": dict(cc)}
    pat_valid = sum(p.get("crc_valid") and p.get("structure_valid") for p in pats)
    pmt_valid = sum(p.get("crc_valid") and p.get("structure_valid") for p in pmts)
    return {
        "file_bytes": len(data),
        "alignment": {"packet_size": TS_SIZE, "offset": offset,
                      "aligned_sync_run": sync_run, "packets": usable,
                      "trailing_bytes": trailing_bytes,
                      "invalid_sync_packet_indices": invalid_packet_indices,
                      "tei_packet_indices": tei_packet_indices,
                      "scrambling_control_counts": {
                          str(value): scrambling_counts[str(value)]
                          for value in range(4)}},
        "packet_totals": {
            "valid_packets": usable - invalid_packets,
            "invalid_packets": invalid_packets,
            "tei_packets": len(tei_packet_indices),
            "scrambled_packets": sum(scrambling_counts[str(value)]
                                      for value in (1, 2, 3)),
            "payload_packets": sum(p[0]["has_payload"] for p in raw_packets),
            "adaptation_only_packets": sum(p[0]["adaptation_only"] for p in raw_packets),
            "discontinuity_indicators": sum(p[0]["discontinuity"] for p in raw_packets)},
        "pid_counts": pid_summary,
        "continuity_totals": {name: sum(v[name] for v in pid_summary.values())
                              for name in ("discontinuities", "duplicates",
                                           "duplicate_cc_conflicts", "discontinuity_resets")},
        "continuity_note": "Null PID (0x1fff) is excluded from CC checks; TEI is counted only for packets with valid sync.",
        "packet_segments": {"first_128": packet_segment(0, 128),
                            "after_first_128": packet_segment(128, usable)},
        "psi": {
            "pat": {"sections": len(pats), "valid": pat_valid,
                    "invalid": len(pats) - pat_valid, "programs": pats,
                    "unique_program_to_pmt_pid": {
                        str(number): pid for number, pid in sorted(unique_program_map.items())},
                    "conflicting_program_numbers": sorted(set(conflicting_program_numbers)),
                    "derived_pmt_pids": pmt_pids,
                    "incomplete_at_eof": len(assemblers[0].pending),
                    "truncated_sections": assemblers[0].truncated,
                    "truncated_events": assemblers[0].truncated_events},
            "pmt": {"sections": len(pmts), "valid": pmt_valid,
                    "invalid": len(pmts) - pmt_valid, "programs": pmts,
                    "incomplete_by_pid": {f"0x{pid:04x}": len(assemblers[pid].pending)
                                           for pid in pmt_pids},
                    "truncated_by_pid": {f"0x{pid:04x}": assemblers[pid].truncated
                                         for pid in pmt_pids},
                    "truncated_events_by_pid": {
                        f"0x{pid:04x}": assemblers[pid].truncated_events
                        for pid in pmt_pids}},
        },
    }


def _append_crc(section_without_crc: bytes) -> bytes:
    crc = mpeg2_crc32(section_without_crc)
    return section_without_crc + crc.to_bytes(4, "big")


def _packet(pid: int, cc: int, payload: bytes, pusi=False) -> bytes:
    if len(payload) > 184:
        raise ValueError("test payload too large")
    header = bytes((SYNC, ((0x40 if pusi else 0) | ((pid >> 8) & 0x1F)),
                    pid & 0xFF, 0x10 | (cc & 0x0F)))
    return header + payload + bytes([0xFF]) * (184 - len(payload))


def _adaptation_only(pid: int, cc: int, discontinuity=False) -> bytes:
    flags = 0x80 if discontinuity else 0
    return bytes((SYNC, (pid >> 8) & 0x1F, pid & 0xFF, 0x20 | (cc & 0x0F),
                  183, flags)) + bytes([0xFF]) * 182


def run_self_tests():
    assert mpeg2_crc32(b"123456789") == 0x0376E6E7
    # PAT with many entries crosses a packet boundary; PMT is on its derived PID.
    entries = b"".join(bytes((0, n, 0xE1, n)) for n in range(1, 48))
    pat_body = bytes((0x00, 0xB0, 0, 0x12, 0x34, 0xC1, 0, 0)) + entries
    pat_len = len(pat_body) - 3 + 4
    pat = _append_crc(pat_body[:1] + bytes((0xB0 | ((pat_len >> 8) & 0x0F), pat_len & 0xFF)) + pat_body[3:])
    assert len(pat) > 184 and crc_ok(pat)
    split = 183
    pat_packets = [_packet(0, 0, b"\x00" + pat[:split], True),
                   _packet(0, 1, pat[split:], False)]

    pmt_pid = 0x101
    pmt_body = bytes((0x02, 0xB0, 0, 0, 1, 0xC1, 0, 0,
                      0xE1, 0x01, 0xF0, 0x00,
                      0x1B, 0xE1, 0x02, 0xF0, 0x00))
    pmt_len = len(pmt_body) - 3 + 4
    pmt = _append_crc(pmt_body[:1] + bytes((0xB0 | ((pmt_len >> 8) & 0x0F), pmt_len & 0xFF)) + pmt_body[3:])
    packets = pat_packets + [_packet(pmt_pid, 0, b"\x00" + pmt, True)]
    result = validate_bytes(b"".join(packets))
    assert result["psi"]["pat"]["valid"] == 1
    assert result["psi"]["pat"]["programs"][0]["programs"][0]["pmt_pid"] == pmt_pid
    assert result["psi"]["pmt"]["valid"] == 1
    assert result["psi"]["pmt"]["programs"][0]["streams"][0]["elementary_pid"] == 0x102
    assert result["packet_totals"]["tei_packets"] == 0

    # A PUSI pointer completes a prior section, then starts a second section.
    small_body = bytes((0, 0xB0, 0, 0x22, 0x22, 0xC1, 0, 0,
                        0, 1, 0xE1, 0x01))
    small_len = len(small_body) - 3 + 4
    small_pat = _append_crc(small_body[:1] + bytes((0xB0, small_len)) + small_body[3:])
    assembler = SectionAssembler()
    assembler.feed(b"\x00" + small_pat[:6], True)
    pointer = len(small_pat) - 6
    assembler.feed(bytes((pointer,)) + small_pat[6:] + small_pat, True)
    assert len(assembler.sections) == 2
    assert all(crc_ok(section) for section in assembler.sections)

    # Adaptation-only packets do not advance payload CC; explicit discontinuity
    # resets the expectation, and an identical repeated payload is a duplicate.
    first = _packet(0x120, 0, b"a")
    second = _packet(0x120, 1, b"b")
    cc_packets = [(parse_ts_packet(p), p) for p in
                  (first, _adaptation_only(0x120, 1), second, second,
                   _packet(0x120, 4, b"c"), _adaptation_only(0x120, 9, True),
                   _packet(0x120, 7, b"d"))]
    cc_packets = [(pkt, raw, i) for i, (pkt, raw) in enumerate(cc_packets)]
    cc_stats = continuity_stats(cc_packets)
    assert cc_stats["duplicates"] == 1
    assert cc_stats["discontinuities"] == 1
    assert cc_stats["discontinuity_resets"] == 1

    # CRC corruption is counted invalid, not silently accepted.
    damaged = bytearray(pat)
    damaged[-1] ^= 1
    bad = validate_bytes(b"".join([
        _packet(0, 0, b"\x00" + bytes(damaged[:split]), True),
        _packet(0, 1, bytes(damaged[split:]), False),
        _packet(pmt_pid, 0, b"\x00" + pmt, True)]))
    assert bad["psi"]["pat"]["invalid"] == 1
    assert bad["psi"]["pmt"]["sections"] == 0
    print("self-test: CRC, PAT/PMT reassembly, pointer/multiple sections, and continuity passed",
          file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", nargs="?", help="188-byte MPEG-TS capture file")
    parser.add_argument("--self-test", action="store_true", help="run synthetic PAT/PMT checks")
    args = parser.parse_args()
    if args.self_test:
        run_self_tests()
        return 0
    if not args.capture:
        parser.error("capture path is required unless --self-test is used")
    try:
        result = validate_bytes(Path(args.capture).read_bytes())
    except (OSError, ValueError) as exc:
        print(json.dumps({"error": str(exc)}, indent=2), file=sys.stderr)
        return 1
    json.dump(result, sys.stdout, indent=2, sort_keys=True)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
