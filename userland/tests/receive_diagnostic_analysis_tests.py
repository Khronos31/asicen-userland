#!/usr/bin/env python3
"""Offline receive-analysis tests. Optional argv[1] is the C++ framer oracle."""
import csv
import importlib.util
import io
import json
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "analyze-rx-diagnostic.py"
SPEC = importlib.util.spec_from_file_location("rx_analysis", SCRIPT)
RX = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RX)
ORACLE = Path(sys.argv[1]) if len(sys.argv) > 1 else None


def packets(count, start=0, pid=0x123):
    return b"".join(bytes([0x47, pid >> 8, pid & 255, 0x10 | ((start + n) & 15)]) +
                    bytes([0x55 + n % 3]) * 184 for n in range(count))


def fixture(chunks, raw_capacity=None, record_capacity=None):
    total = sum(map(len, chunks))
    raw_capacity = max(total, 1) if raw_capacity is None else raw_capacity
    record_capacity = max(len(chunks), 1) if record_capacity is None else record_capacity
    raw = b""
    rows = []
    replay = RX.FramerReplay()
    offset = 0
    for index, chunk in enumerate(chunks):
        before = replay.counters()
        replay.push(chunk)
        after = replay.counters()
        row = dict.fromkeys(RX.FIELDS, 0)
        count = min(len(chunk), raw_capacity - len(raw)) if index < record_capacity else 0
        row.update(submission_order=index, generation=index // 4 + 1,
                   completion_order=index + 1, slot=index % 4, requested_bytes=65536,
                   actual_bytes=len(chunk), status=0, disposition=2,
                   input_offset=offset, raw_offset=len(raw), raw_bytes=count,
                   raw_dropped_bytes=len(chunk) - count,
                   framer_accepted_delta=after[0] - before[0],
                   framer_emitted_delta=after[1] - before[1],
                   sync_discard_delta=after[2] - before[2], sync_loss_delta=after[3] - before[3],
                   framer_pending_after=after[4],
                   emit_calls=after[1] - before[1], emit_accepted=after[1] - before[1])
        stamp = 1000 + index * 10000
        row.update(submit_begin_ns=stamp, submit_end_ns=stamp + 100,
                   callback_entry_ns=stamp + 1000, callback_ready_ns=stamp + 1100,
                   processing_begin_ns=stamp + 1200, raw_copy_end_ns=stamp + 1300,
                   decode_begin_ns=stamp + 1400, decode_end_ns=stamp + 2400,
                   processing_end_ns=stamp + 2500)
        if row["emit_calls"]:
            row.update(emit_first_ns=stamp + 1500, emit_last_ns=stamp + 2300,
                       emit_total_ns=100)
        if index < record_capacity:
            rows.append(row)
        raw += chunk[:count]
        offset += len(chunk)
    metadata = dict(asicen_rx_diagnostic=1, **RX.TEXT_META, raw_capacity=raw_capacity,
                    record_capacity=record_capacity, stored_rows=len(rows),
                    dropped_records=len(chunks) - len(rows), raw_bytes_saved=len(raw),
                    raw_bytes_omitted=total - len(raw), observed_valid_bytes=total,
                    decoder_input_bytes=total, submissions=len(chunks), completions=len(chunks),
                    invalid_lengths=0, terminal_result=0, finalized=1)
    return metadata, rows, raw


def write_capture(prefix, metadata, rows, raw):
    stream = io.StringIO(newline="")
    header = ("asicen_rx_diagnostic", "timestamps", "callback_time", "raw_capacity", "record_capacity")
    for key in header:
        stream.write(f"# {key}={metadata[key]}\n")
    writer = csv.writer(stream, lineterminator="\n")
    writer.writerow(RX.FIELDS)
    writer.writerows([[row[key] for key in RX.FIELDS] for row in rows])
    for key, value in metadata.items():
        if key not in header:
            stream.write(f"# {key}={value}\n")
    Path(str(prefix) + ".csv").write_text(stream.getvalue(), encoding="ascii")
    Path(str(prefix) + ".raw").write_bytes(raw)


class FramerTests(unittest.TestCase):
    def compare_oracle(self, data, chunks):
        replay = RX.FramerReplay()
        expected = []
        offset = 0
        for amount in chunks:
            replay.push(data[offset:offset + amount])
            offset += amount
            expected.append(replay.counters())
        self.assertEqual(offset, len(data))
        if ORACLE is not None:
            result = subprocess.run([str(ORACLE)] + list(map(str, chunks)), input=data,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            actual = [tuple(map(int, line.split(b","))) for line in result.stdout.splitlines()]
            self.assertEqual(actual, expected)
        return replay

    def test_four_complete_packet_probe(self):
        data = bytes(187) + packets(4)
        replay = self.compare_oracle(data, [187 + 3 * 188 + 1, 187])
        self.assertEqual(replay.counters(), (939, 4, 187, 0, 0))
        replay = self.compare_oracle(packets(4)[:3 * 188 + 1], [3 * 188 + 1])
        self.assertEqual(replay.emitted, 0)

    def test_loss_reacquisition_and_terminal_partial(self):
        data = packets(4) + b"\x00" + packets(5, 4) + packets(1, 9)[:117]
        replay = self.compare_oracle(data, [752, 1, 5 * 188 + 117])
        self.assertEqual(replay.counters(), (len(data), 9, 1, 1, 117))

    def test_long_garbage_retains_probe_carry(self):
        data = b"\x00" * 10000
        replay = self.compare_oracle(data, [1, 750, 1, 1000, 8248])
        self.assertEqual(replay.counters(), (10000, 0, 9249, 0, 751))

    def test_fragmentation_and_compaction(self):
        generator = random.Random(2048)
        for noise_size in (0, 1, 187, 752):
            data = bytes([0x11]) * noise_size + packets(400) + b"\x00" + packets(350, 400) + b"tail"
            chunks, remaining = [], len(data)
            while remaining:
                amount = min(remaining, generator.choice([1, 187, 188, 189, 751, 65536]))
                chunks.append(amount)
                remaining -= amount
            replay = self.compare_oracle(data, chunks)
            self.assertEqual(replay.emitted, 750)
            self.assertEqual(replay.discarded, noise_size + 1)


class CaptureTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="asicen-rx-analysis-")
        self.addCleanup(self.temp.cleanup)
        self.prefix = Path(self.temp.name) / "capture"

    def run_capture(self, chunks, **kwargs):
        values = fixture(chunks, **kwargs)
        write_capture(self.prefix, *values)
        return RX.analyze(self.prefix)

    def test_complete_capture_and_timing(self):
        result = self.run_capture([packets(4), packets(4, 4), packets(4, 8), packets(4, 12), packets(4, 16)])
        self.assertTrue(result["raw_capture_complete"])
        self.assertTrue(result["framer_replay"]["compared_rows_match"])
        self.assertEqual(result["framer_replay"]["rows_compared"], 5)
        self.assertEqual(result["timing"]["callback_to_resubmit"]["max_ns"], 39000)
        self.assertEqual(result["timing"]["decode_excluding_emit"]["max_ns"], 900)
        self.assertEqual(result["raw_headers"]["payload_cc_transition_candidates"], 0)

    def test_byte_cap_is_explicit(self):
        result = self.run_capture([packets(4), packets(4, 4)], raw_capacity=900)
        self.assertFalse(result["raw_capture_complete"])
        self.assertEqual(result["framer_replay"]["rows_compared"], 1)
        self.assertEqual(result["framer_replay"]["counters"][-1], 148)
        self.assertEqual(result["capture"]["raw_bytes_omitted"], 604)

    def test_record_cap_is_explicit(self):
        result = self.run_capture([packets(4), packets(4, 4)], record_capacity=1)
        self.assertFalse(result["raw_capture_complete"])
        self.assertEqual(result["framer_replay"]["comparison_stopped_reason"], "metadata_limit")

    def test_sink_failure_does_not_claim_equal_replay(self):
        metadata, rows, raw = fixture([packets(4)])
        rows[0].update(decode_error=8, emit_calls=2, emit_accepted=1, framer_emitted_delta=1)
        write_capture(self.prefix, metadata, rows, raw)
        result = RX.analyze(self.prefix)
        self.assertEqual(result["framer_replay"]["rows_compared"], 0)
        self.assertTrue(result["raw_capture_complete"])
        self.assertFalse(result["framer_replay"]["comparison_complete"])
        self.assertEqual(result["framer_replay"]["comparison_stopped_reason"], "live_decoder_or_sink_failure")

    def test_counter_mismatch_detected(self):
        metadata, rows, raw = fixture([packets(4)])
        rows[0]["framer_pending_after"] += 1
        write_capture(self.prefix, metadata, rows, raw)
        result = RX.analyze(self.prefix)
        self.assertFalse(result["framer_replay"]["compared_rows_match"])

    def test_raw_cc_jump_labeled_candidate(self):
        result = self.run_capture([packets(4) + packets(4, 9)])
        self.assertEqual(result["raw_headers"]["payload_cc_transition_candidates"], 1)
        self.assertEqual(result["raw_headers"]["candidate_examples"][0]["raw_offset"], 752)
        self.assertIn("not validated continuity errors", " ".join(result["limits"]))

    def test_truncated_and_corrupt_files_rejected(self):
        for mutation in ("no_final_marker", "no_newline", "short_raw", "long_raw", "missing_raw",
                         "wrong_rows", "offset_overlap", "unknown_column", "bad_integer",
                         "duplicate_completion", "time_reversal"):
            with self.subTest(mutation=mutation):
                metadata, rows, raw = fixture([packets(4), packets(4, 4)])
                if mutation == "wrong_rows": metadata["stored_rows"] += 1
                if mutation == "offset_overlap": rows[1]["raw_offset"] -= 1
                if mutation == "duplicate_completion": rows[1]["completion_order"] = 1
                if mutation == "time_reversal": rows[0]["decode_end_ns"] = 1
                write_capture(self.prefix, metadata, rows, raw)
                csv_path, raw_path = Path(str(self.prefix) + ".csv"), Path(str(self.prefix) + ".raw")
                if mutation == "no_final_marker": csv_path.write_text(csv_path.read_text().replace("# finalized=1\n", ""))
                if mutation == "no_newline": csv_path.write_bytes(csv_path.read_bytes()[:-1])
                if mutation == "short_raw": raw_path.write_bytes(raw[:-1])
                if mutation == "long_raw": raw_path.write_bytes(raw + b"\x00")
                if mutation == "missing_raw": raw_path.unlink()
                if mutation == "unknown_column": csv_path.write_text(csv_path.read_text().replace("submission_order,", "unknown,"))
                if mutation == "bad_integer": csv_path.write_text(csv_path.read_text().replace("# submissions=2", "# submissions=nan"))
                with self.assertRaises(RX.DiagnosticError): RX.analyze(self.prefix)

    def test_cli_emits_json_and_errors(self):
        self.run_capture([packets(4)])
        result = subprocess.run([sys.executable, str(SCRIPT), str(self.prefix)],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["schema"], "asicen_rx_analysis_v1")
        Path(str(self.prefix) + ".raw").write_bytes(b"")
        result = subprocess.run([sys.executable, str(SCRIPT), str(self.prefix)],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
        self.assertEqual(result.returncode, 2)
        self.assertIn("error", json.loads(result.stdout))


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
