#!/usr/bin/env python3
"""Validate and summarize local ASICEN receive diagnostics without transport keys.

Usage: python3 scripts/analyze-rx-diagnostic.py PREFIX
Reads PREFIX.csv and PREFIX.raw. Writes JSON only; never opens USB or uploads data.
Raw CC transitions are candidates: adaptation-field discontinuity flags are
transformed, so the raw capture is not a valid input to the normal CC checker.
"""
import argparse
import csv
import io
import json
import math
from pathlib import Path
import re
import sys


FIELDS = (
    "submission_order generation completion_order submit_begin_ns submit_end_ns "
    "callback_entry_ns callback_ready_ns processing_begin_ns raw_copy_end_ns "
    "decode_begin_ns decode_end_ns processing_end_ns emit_first_ns emit_last_ns "
    "emit_total_ns emit_calls emit_accepted input_offset raw_offset raw_bytes "
    "raw_dropped_bytes sync_loss_delta sync_discard_delta framer_accepted_delta "
    "framer_emitted_delta framer_pending_after slot requested_bytes submit_result "
    "status actual_bytes decode_error disposition callback_during_drain"
).split()
REQUIRED_META = (
    "raw_capacity record_capacity stored_rows dropped_records raw_bytes_saved raw_bytes_omitted "
    "observed_valid_bytes decoder_input_bytes submissions completions "
    "invalid_lengths terminal_result finalized"
).split()
SIGNED_FIELDS = {"submit_result", "status", "actual_bytes", "decode_error"}
TEXT_META = {"timestamps": "host_steady_clock_nanoseconds_since_init_plus_one",
             "callback_time": "host_observation_not_usb_bus_arrival"}
MAX_RAW_BYTES = 64 * 1024 * 1024
MAX_RECORDS = 65536
MAX_CSV_BYTES = 64 * 1024 * 1024


class DiagnosticError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise DiagnosticError(message)


class FramerReplay:
    """The actual TsFramer Mode7 algorithm, including its four-full-packet probe.

    This accepts every framed packet. A live failing sink can stop earlier; such
    rows are explicitly excluded from equal-counter claims by analyze().
    """

    def __init__(self, sink=None):
        self.pending = bytearray()
        self.pending_offset = 0
        self.synchronized = False
        self.input_bytes = 0
        self.emitted = 0
        self.discarded = 0
        self.losses = 0
        self.sink = sink

    def counters(self):
        return (self.input_bytes, self.emitted, self.discarded, self.losses,
                len(self.pending))

    def push(self, data):
        require(len(data) <= 1048576, "framer input exceeds the C++ input bound")
        require(len(data) + len(self.pending) <= 1048576 + 4 * 188,
                "framer pending data exceeds the C++ bound")
        self.pending.extend(data)
        self.input_bytes += len(data)
        consumed = 0
        while True:
            available = len(self.pending) - consumed
            if not self.synchronized:
                if available < 4 * 188:
                    break
                # C++ tests four sync bytes but requires four *complete* packets.
                candidate = consumed
                last = len(self.pending) - 4 * 188
                while candidate <= last:
                    candidate = self.pending.find(0x47, candidate, last + 1)
                    if candidate < 0:
                        break
                    if all(self.pending[candidate + n * 188] == 0x47 for n in range(4)):
                        break
                    candidate += 1
                if candidate < 0 or candidate > last:
                    amount = available - (4 * 188 - 1)
                    self.discarded += amount
                    consumed += amount
                    break
                self.discarded += candidate - consumed
                consumed = candidate
                self.synchronized = True
                available = len(self.pending) - consumed
            if available < 188:
                break
            if self.pending[consumed] != 0x47:
                self.synchronized = False
                self.losses += 1
                self.discarded += 1
                consumed += 1
                continue
            if self.sink is not None:
                self.sink(bytes(self.pending[consumed:consumed + 188]),
                          self.pending_offset + consumed)
            self.emitted += 1
            consumed += 188
        if consumed:
            del self.pending[:consumed]
            self.pending_offset += consumed


def read_bounded(path, limit):
    require(path.is_file(), f"missing or non-regular input: {path.name}")
    require(path.stat().st_size <= limit, f"input exceeds diagnostic bound: {path.name}")
    with path.open("rb") as source:
        data = source.read(limit + 1)
    require(len(data) <= limit, f"input grew beyond diagnostic bound: {path.name}")
    return data


def integer(value, label, signed=False):
    require(re.fullmatch(r"-?[0-9]+", value) is not None, f"invalid integer: {label}")
    require(len(value) <= 21, f"out-of-range integer: {label}")
    result = int(value)
    low, high = (-(1 << 31), (1 << 31) - 1) if signed else (0, (1 << 64) - 1)
    require(low <= result <= high, f"out-of-range integer: {label}")
    return result


def load_capture(prefix):
    contents = read_bounded(Path(str(prefix) + ".csv"), MAX_CSV_BYTES)
    require(contents.endswith(b"\n"), "CSV is truncated: final newline missing")
    try:
        lines = contents.decode("ascii").splitlines()
    except UnicodeDecodeError as error:
        raise DiagnosticError("CSV contains non-ASCII data") from error
    require(lines and lines[0] == "# asicen_rx_diagnostic=1", "unsupported diagnostic schema")
    require(lines[-1] == "# finalized=1", "CSV is incomplete: finalization marker missing")
    metadata, data_lines = {}, []
    footer_started = False
    for line in lines:
        require(bool(line), "unexpected blank CSV line")
        if line.startswith("# "):
            key, separator, value = line[2:].partition("=")
            require(separator and key not in metadata, "invalid or duplicate metadata field")
            if key in TEXT_META:
                require(value == TEXT_META[key], f"unsupported {key} metadata")
                metadata[key] = value
            else:
                metadata[key] = integer(value, key)
            footer_started = bool(data_lines)
        else:
            require(not footer_started, "CSV row after footer")
            data_lines.append(line)
    require(all(key in metadata for key in REQUIRED_META), "CSV summary fields missing")
    reader = csv.reader(io.StringIO("\n".join(data_lines)), strict=True)
    require(next(reader, None) == FIELDS, "unexpected CSV column schema")
    rows = []
    for fields in reader:
        require(len(rows) < MAX_RECORDS, "too many diagnostic records")
        require(len(fields) == len(FIELDS), "CSV record has wrong column count")
        row = {key: integer(value, key, key in SIGNED_FIELDS)
               for key, value in zip(FIELDS, fields)}
        rows.append(row)
    raw = read_bounded(Path(str(prefix) + ".raw"), MAX_RAW_BYTES)
    validate_capture(metadata, rows, raw)
    return metadata, rows, raw


def validate_capture(metadata, rows, raw):
    require(0 < metadata["raw_capacity"] <= MAX_RAW_BYTES and
            0 < metadata["record_capacity"] <= MAX_RECORDS, "invalid capture capacity")
    require(len(raw) <= metadata["raw_capacity"] and len(rows) <= metadata["record_capacity"],
            "capture exceeds declared capacity")
    require(metadata["stored_rows"] == len(rows), "CSV stored-row count mismatch")
    require(metadata["submissions"] == len(rows) + metadata["dropped_records"],
            "CSV submission accounting mismatch")
    require(metadata["raw_bytes_saved"] == len(raw), "raw dump length mismatch or truncation")
    require(metadata["raw_bytes_saved"] + metadata["raw_bytes_omitted"] ==
            metadata["decoder_input_bytes"], "raw/decoder byte accounting mismatch")
    require(metadata["observed_valid_bytes"] >= metadata["decoder_input_bytes"],
            "decoder bytes exceed observed valid completion bytes")
    raw_end, input_end, completed, previous_slots = 0, 0, set(), {}
    for index, row in enumerate(rows):
        tag = f"record {index}"
        require(row["submission_order"] == index, f"{tag}: noncontiguous submission order")
        require(row["slot"] < 4 and row["requested_bytes"] <= 1048576,
                f"{tag}: invalid slot or requested length")
        require(row["disposition"] <= 6 and row["callback_during_drain"] <= 1,
                f"{tag}: invalid disposition or boolean")
        require(-1 <= row["status"] <= 6, f"{tag}: invalid libusb completion status")
        if row["slot"] in previous_slots:
            require(row["generation"] == previous_slots[row["slot"]] + 1,
                    f"{tag}: generation sequence mismatch")
        previous_slots[row["slot"]] = row["generation"]
        if row["completion_order"]:
            require(row["completion_order"] not in completed, f"{tag}: duplicate completion order")
            completed.add(row["completion_order"])
            require(row["completion_order"] <= metadata["completions"],
                    f"{tag}: completion order exceeds total")
        for start, end in (("submit_begin_ns", "submit_end_ns"),
                           ("callback_entry_ns", "callback_ready_ns"),
                           ("callback_ready_ns", "processing_begin_ns"),
                           ("processing_begin_ns", "raw_copy_end_ns"),
                           ("decode_begin_ns", "decode_end_ns"),
                           ("decode_end_ns", "processing_end_ns"),
                           ("emit_first_ns", "emit_last_ns")):
            if row[start] and row[end]:
                require(row[end] >= row[start], f"{tag}: reversed {start}/{end}")
        require(row["emit_accepted"] <= row["emit_calls"], f"{tag}: invalid emit counts")
        require(row["framer_emitted_delta"] <= row["emit_accepted"],
                f"{tag}: emitted packets exceed accepted sink calls")
        if row["raw_bytes"] or row["raw_dropped_bytes"]:
            require(row["status"] == 0 and row["actual_bytes"] > 0,
                    f"{tag}: raw data from non-completed transfer")
            require(row["actual_bytes"] <= row["requested_bytes"], f"{tag}: invalid actual length")
            require(row["raw_bytes"] + row["raw_dropped_bytes"] == row["actual_bytes"],
                    f"{tag}: captured transfer byte accounting mismatch")
            require(row["input_offset"] == input_end, f"{tag}: decoder input offset gap")
            require(row["raw_offset"] == raw_end, f"{tag}: raw offset gap or overlap")
            input_end += row["actual_bytes"]
            raw_end += row["raw_bytes"]
    require(raw_end == len(raw), "raw bytes lack matching CSV records")
    require(input_end <= metadata["decoder_input_bytes"], "recorded decoder bytes exceed total")
    require(sum(row["raw_dropped_bytes"] for row in rows) <= metadata["raw_bytes_omitted"],
            "recorded raw omissions exceed total")
    if metadata["dropped_records"] == 0:
        require(input_end == metadata["decoder_input_bytes"], "decoder inputs lack matching records")
        require(len(completed) == metadata["completions"], "completions lack matching records")


def summarize_durations(samples):
    if not samples:
        return {"samples": 0}
    values = sorted(value for value, _ in samples)
    maximum = max(samples, key=lambda pair: pair[0])
    return {"samples": len(values), "p50_ns": values[math.ceil(len(values) * .50) - 1],
            "p95_ns": values[math.ceil(len(values) * .95) - 1],
            "p99_ns": values[math.ceil(len(values) * .99) - 1],
            "max_ns": maximum[0], "max_submission_order": maximum[1]}


def timing_summary(rows):
    phases = {"submission_window": ("submit_begin_ns", "submit_end_ns"),
              "submit_to_callback": ("submit_begin_ns", "callback_entry_ns"),
              "callback_to_ready": ("callback_entry_ns", "callback_ready_ns"),
              "ready_to_processing": ("callback_ready_ns", "processing_begin_ns"),
              "processing_to_copy_end": ("processing_begin_ns", "raw_copy_end_ns"),
              "decode_including_emit": ("decode_begin_ns", "decode_end_ns"),
              "processing": ("processing_begin_ns", "processing_end_ns")}
    samples = {key: [] for key in phases}
    samples.update(emit=[], decode_excluding_emit=[], callback_to_resubmit=[])
    previous = {}
    for row in rows:
        order = row["submission_order"]
        for key, (begin, end) in phases.items():
            if row[begin] and row[end] and row[end] >= row[begin]:
                samples[key].append((row[end] - row[begin], order))
        if row["decode_begin_ns"] and row["decode_end_ns"]:
            duration = row["decode_end_ns"] - row["decode_begin_ns"]
            require(row["emit_total_ns"] <= duration, "emit time exceeds decoder duration")
            samples["emit"].append((row["emit_total_ns"], order))
            samples["decode_excluding_emit"].append((duration - row["emit_total_ns"], order))
        earlier = previous.get(row["slot"])
        if earlier and earlier["callback_entry_ns"] and row["submit_begin_ns"]:
            duration = row["submit_begin_ns"] - earlier["callback_entry_ns"]
            require(duration >= 0, "resubmission precedes the slot's previous callback")
            samples["callback_to_resubmit"].append((duration, earlier["submission_order"]))
        previous[row["slot"]] = row
    return {key: summarize_durations(value) for key, value in samples.items()}


def analyze(prefix):
    metadata, rows, raw = load_capture(prefix)
    header_counts = {"tei_packets": 0, "null_packets": 0, "invalid_afc_packets": 0,
                     "payload_cc_transition_candidates": 0}
    baseline, examples = {}, []
    current_order = 0

    def observe_header(packet, offset):
        pid = ((packet[1] & 31) << 8) | packet[2]
        afc, cc = (packet[3] >> 4) & 3, packet[3] & 15
        if pid == 8191:
            header_counts["null_packets"] += 1
            return
        if packet[1] & 0x80 or afc == 0:
            header_counts["tei_packets"] += bool(packet[1] & 0x80)
            header_counts["invalid_afc_packets"] += afc == 0
            baseline.pop(pid, None)
            return
        if afc == 2:
            # Unknown encrypted discontinuity flags can rebase the CC here.
            baseline.pop(pid, None)
            return
        previous = baseline.get(pid)
        if previous is not None and cc not in (previous[0], (previous[0] + 1) & 15):
            header_counts["payload_cc_transition_candidates"] += 1
            if len(examples) < 12:
                examples.append({"pid": pid, "previous_cc": previous[0], "cc": cc,
                                 "previous_raw_offset": previous[1], "raw_offset": offset,
                                 "delivery_submission_order": current_order})
        baseline[pid] = (cc, offset)

    replay = FramerReplay(observe_header)
    compared, mismatch_count, mismatches, stopped_reason = 0, 0, [], None
    for row in rows:
        if not row["raw_bytes"] and not row["raw_dropped_bytes"]:
            continue
        current_order = row["submission_order"]
        before = replay.counters()
        start = row["raw_offset"]
        replay.push(raw[start:start + row["raw_bytes"]])
        after = replay.counters()
        if row["raw_dropped_bytes"]:
            stopped_reason = "raw_capacity_or_metadata_limit"
            break
        if row["decode_error"] or row["emit_calls"] != row["emit_accepted"]:
            stopped_reason = "live_decoder_or_sink_failure"
            break
        observed = (row["framer_accepted_delta"], row["framer_emitted_delta"],
                    row["sync_discard_delta"], row["sync_loss_delta"], row["framer_pending_after"])
        expected = tuple(after[n] - before[n] for n in range(4)) + (after[4],)
        compared += 1
        if expected != observed:
            mismatch_count += 1
            if len(mismatches) < 12:
                mismatches.append({"submission_order": current_order,
                                   "offline": list(expected), "live": list(observed)})
    if stopped_reason is None and metadata["dropped_records"]:
        stopped_reason = "metadata_limit"
    full = not metadata["raw_bytes_omitted"]
    return {"schema": "asicen_rx_analysis_v1", "capture": metadata,
            "raw_capture_complete": full,
            "metadata_complete": not metadata["dropped_records"],
            "raw_capture_scope": "completed bulk bytes passed to decoder; excludes drain/error/timeout payloads",
            "valid_callback_bytes_not_passed_to_decoder":
                metadata["observed_valid_bytes"] - metadata["decoder_input_bytes"],
            "timing": timing_summary(rows),
            "framer_replay": {"scope": "captured decoder-input prefix; always-accepting sink",
                              "counter_order": ["input_bytes", "emitted_packets", "discarded_bytes",
                                                "sync_loss_events", "pending_bytes"],
                              "counters": list(replay.counters()), "rows_compared": compared,
                              "comparison_complete": full and not metadata["dropped_records"]
                                                     and stopped_reason is None,
                              "comparison_stopped_reason": stopped_reason,
                              "counter_mismatch_count": mismatch_count,
                              "counter_mismatch_examples": mismatches,
                              "compared_rows_match": (not mismatches) if compared else None},
            "raw_headers": dict(header_counts, candidate_examples=examples),
            "limits": ["Callback time is host observation, not USB bus arrival time.",
                       "Timing correlation cannot prove a device FIFO overflow or locate loss among RF, device and USB.",
                       "Raw adaptation fields are transformed: CC candidates are not validated continuity errors.",
                       "Header candidates are not exhaustive: repeated CC and adaptation-only transitions are not classified.",
                       "Matching replay counters test framing reproducibility, not payload transform correctness or clean reception.",
                       "Timing statistics cover retained metadata rows only; absent stages are excluded.",
                       "Submission windows include queue-lock overhead; processing-to-copy-end includes dispatch overhead.",
                       "Instrumentation adds memory copies and clock reads; compare enabled and disabled runs."]}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("prefix", type=Path, help="path before .csv and .raw")
    args = parser.parse_args(argv)
    try:
        result = analyze(args.prefix)
    except (DiagnosticError, OSError, csv.Error) as error:
        print(json.dumps({"schema": "asicen_rx_analysis_v1", "error": str(error)}))
        return 2
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
