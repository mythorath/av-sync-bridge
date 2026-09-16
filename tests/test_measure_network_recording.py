#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generated network-fixture analysis tests; no network, OBS, or media tools."""
import contextlib
import copy
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
try:
    SPEC = importlib.util.spec_from_file_location("measure_network_recording", TOOLS / "measure_network_recording.py")
    measure = importlib.util.module_from_spec(SPEC)
    SPEC.loader.exec_module(measure)
finally:
    sys.path.pop(0)


class NetworkRecordingTests(unittest.TestCase):
    offsets = [48000, 112800, 196800, 321600, 487200, 710400]
    origin = 1_000_000_000_000
    successor_origin = origin + 16_000_000_000
    maximum_ns = (1 << 63) - 1

    def fixture(self, role=1, count=6, origin=None):
        origin = (self.origin if role == 1 else self.successor_origin) if origin is None else origin
        header = {"schema": 1, "type": "header", "fixture_id": role,
                  "sender_session": str(9007199254740993 + role),
                  "clock_epoch": str(18446744073709551615 - role), "generation": str(123 + role),
                  "sample_rate": 48000, "channels": 2, "warmup_frames": 480000,
                  "duration_frames": 1440, "capture_origin_ns": str(origin),
                  "frame_offsets": list(self.offsets)}
        events = [{"schema": 1, "type": "marker", "fixture_id": role, "event": event,
                   "frame_position": 480000 + offset, "duration_frames": 1440,
                   "frequency_hz": 660 + 220 * (event - 1) + 2200 * (role - 1),
                   "capture_ns": str(origin + (480000 + offset) * 1_000_000_000 // 48000)}
                  for event, offset in enumerate(self.offsets[:count], 1)]
        return {"header": header, "events": events}

    @staticmethod
    def text(fixture):
        return "".join("AVSYNC_FIXTURE " + json.dumps(item) + "\n"
                       for item in [fixture["header"], *fixture["events"]])

    def parse(self, fixture, role=None, count=None):
        return measure.parse_manifest(self.text(fixture), role or fixture["header"]["fixture_id"],
                                      count or len(fixture["events"]))

    def expected(self, restart=True):
        if restart:
            return measure.expected_events(self.parse(self.fixture(1, 3)), self.parse(self.fixture(2, 6)))
        return measure.expected_events(self.parse(self.fixture()))

    @staticmethod
    def audio(expected, shift=0):
        first = expected[0][-1]
        return [(role, event, (stamp - first) / 1e9 - .25 + shift) for role, event, stamp in expected]

    @staticmethod
    def rows(expected, offset_ns=0, missing=None, extra=None, duration_ns=30_000_000):
        starts = [event[-1] for event in expected]
        if missing is not None:
            del starts[missing]
        if extra is not None:
            starts.append(extra)
        starts.sort()
        active = 0
        for stamp in range(starts[0] - 20_000_000, starts[-1] + duration_ns + 20_000_000, 1_000_000):
            while active < len(starts) and stamp >= starts[active] + duration_ns:
                active += 1
            yield {"timestamp_ns": stamp + offset_ns, "frames": 48,
                   "rms": .1 if active < len(starts) and stamp >= starts[active] else 0}

    def assert_encoded_rejected(self, audio, expected):
        try:
            report = measure.analyze_encoded(audio, expected)
        except measure.MeasurementError:
            return
        self.assertFalse(report["valid_marker_match"] and report["timing_gate"]["passed"], report)

    def test_exact_supported_manifests_preserve_decimal_strings(self):
        for role, count in ((1, 6), (1, 3), (2, 6)):
            fixture = self.fixture(role, count)
            with self.subTest(role=role, count=count):
                self.assertEqual(self.parse(fixture), fixture)
                self.assertIsInstance(self.parse(fixture)["events"][0]["capture_ns"], str)

    def test_unknown_scope_counts_and_roles_rejected(self):
        fixture = self.text(self.fixture())
        for role, count in ((0, 6), (3, 6), (True, 6), (1.0, 6), (1, 1), (1, 5),
                            (2, 3), (1, True), (1, 6.0), ("1", 6)):
            with self.subTest(role=role, count=count), self.assertRaises(measure.MeasurementError):
                measure.parse_manifest(fixture, role, count)

    def test_missing_duplicate_reordered_and_extra_manifest_records_rejected(self):
        fixture = self.fixture()
        lines = self.text(fixture).splitlines(keepends=True)
        variants = ["", "".join(lines[1:]), "".join(lines[:-1]), "".join(lines + [lines[-1]]),
                    "".join([lines[0], *lines]), "".join([lines[1], lines[0], *lines[2:]]),
                    "".join([lines[0], lines[2], lines[1], *lines[3:]]),
                    self.text(self.fixture(2)), self.text(fixture) + "AVSYNC_FIXTURE {}\n"]
        for value in variants:
            with self.subTest(text=value[:90]), self.assertRaises(measure.MeasurementError):
                measure.parse_manifest(value, 1, 6)

    def test_duplicate_json_keys_malformed_json_and_unknown_schema_rejected(self):
        text = self.text(self.fixture())
        variants = [text.replace('"schema": 1', '"schema":1,"schema":1', 1),
                    text.replace('"capture_ns":', '"capture_ns":"1","capture_ns":', 1),
                    text.replace('"schema": 1', '"schema":NaN', 1),
                    text.replace('"schema": 1', '"schema":Infinity', 1),
                    "AVSYNC_FIXTURE {\n", "AVSYNC_FIXTURE []\n", "AVSYNC_FIXTURE null\n",
                    "AVSYNC_FIXTURE " + "[" * 2000 + "\n"]
        for value in variants:
            with self.subTest(text=value[:90]), self.assertRaises(measure.MeasurementError):
                measure.parse_manifest(value, 1, 6)
        for target in ("header", "event"):
            for field, value in (("schema", True), ("schema", 2), ("type", "unknown"), ("extra", 0)):
                fixture = self.fixture()
                (fixture["header"] if target == "header" else fixture["events"][0])[field] = value
                with self.subTest(target=target, field=field, value=value), self.assertRaises(measure.MeasurementError):
                    self.parse(fixture)

    def test_header_source_identities_are_canonical_nonzero_uint64_strings(self):
        for key in ("sender_session", "clock_epoch", "generation"):
            for value in (0, 17, True, None, "0", "01", "-1", "+1", " 1", "1.0", str(1 << 64)):
                fixture = self.fixture()
                fixture["header"][key] = value
                with self.subTest(key=key, value=value), self.assertRaises(measure.MeasurementError):
                    self.parse(fixture)
        fixture = self.fixture()
        for key in ("sender_session", "clock_epoch", "generation"):
            fixture["header"][key] = str((1 << 64) - 1)
        self.assertEqual(self.parse(fixture), fixture)

    def test_fixed_header_format_never_coerces_float_or_bool(self):
        fields = {"fixture_id": 1, "sample_rate": 48000, "channels": 2,
                  "warmup_frames": 480000, "duration_frames": 1440}
        for key, required in fields.items():
            for value in (True, float(required), str(required), required + 1, None):
                fixture = self.fixture()
                fixture["header"][key] = value
                with self.subTest(key=key, value=value), self.assertRaises(measure.MeasurementError):
                    measure.parse_manifest(self.text(fixture), 1, 6)
        for offsets in (self.offsets[:-1], self.offsets + [720000], list(reversed(self.offsets)),
                        [48000.0, *self.offsets[1:]], [True, *self.offsets[1:]], "48000"):
            fixture = self.fixture()
            fixture["header"]["frame_offsets"] = offsets
            with self.subTest(offsets=offsets), self.assertRaises(measure.MeasurementError):
                self.parse(fixture)

    def test_markers_exact_frequency_position_duration_role_and_event(self):
        for key in ("fixture_id", "event", "frame_position", "duration_frames", "frequency_hz"):
            original = self.fixture()["events"][0][key]
            for value in (True, float(original), str(original), original + 1, None):
                fixture = self.fixture()
                fixture["events"][0][key] = value
                with self.subTest(key=key, value=value), self.assertRaises(measure.MeasurementError):
                    self.parse(fixture)

    def test_capture_timestamps_are_canonical_bounded_integer_strings(self):
        for target, key in (("header", "capture_origin_ns"), ("event", "capture_ns")):
            for value in (True, 123, 123.0, None, "-1", "+1", "01", "1.0", "NaN", str(1 << 63)):
                fixture = self.fixture()
                (fixture["header"] if target == "header" else fixture["events"][0])[key] = value
                with self.subTest(target=target, value=value), self.assertRaises(measure.MeasurementError):
                    self.parse(fixture)

    def test_capture_nominal_consistency_has_bounded_tolerance_not_rebasing(self):
        for offset in (-100_000_000, 100_000_000):
            fixture = self.fixture()
            fixture["events"][2]["capture_ns"] = str(int(fixture["events"][2]["capture_ns"]) + offset)
            self.assertEqual(self.parse(fixture), fixture)
        for offset in (-100_000_001, 100_000_001, 20_000_000_000):
            fixture = self.fixture()
            fixture["events"][2]["capture_ns"] = str(int(fixture["events"][2]["capture_ns"]) + offset)
            with self.subTest(offset=offset), self.assertRaises(measure.MeasurementError):
                self.parse(fixture)

    def test_baseline_and_restart_expected_events_use_every_original_anchor(self):
        for restart in (False, True):
            actual = self.expected(restart)
            manifests = [self.fixture(1, 3), self.fixture(2)] if restart else [self.fixture()]
            expected = [(event["fixture_id"], event["event"], int(event["capture_ns"]) + 2_000_000_000)
                        for manifest in manifests for event in manifest["events"]]
            self.assertEqual(actual, expected)

    def test_restart_requires_new_session_and_clock_without_reusing_identity(self):
        before = self.parse(self.fixture(1, 3))
        for key in ("sender_session", "clock_epoch"):
            after = self.parse(self.fixture(2))
            after["header"][key] = before["header"][key]
            with self.subTest(key=key), self.assertRaises(measure.MeasurementError):
                measure.expected_events(before, after)

    def test_wrong_restart_scope_nonmonotonic_span_and_presentation_overflow(self):
        before = self.parse(self.fixture(1, 3))
        for origin in (self.origin, self.origin - 30_000_000_000,
                       self.origin + 130_000_000_000):
            after = self.parse(self.fixture(2, origin=origin))
            with self.subTest(origin=origin), self.assertRaises(measure.MeasurementError):
                measure.expected_events(before, after)
        for first, second in ((self.parse(self.fixture()), self.parse(self.fixture(2))),
                               (self.parse(self.fixture(2)), None), (before, None)):
            with self.subTest(count=len(first["events"])), self.assertRaises(measure.MeasurementError):
                measure.expected_events(first, second)
        with self.assertRaises(measure.MeasurementError):
            measure.expected_events(self.fixture(origin=self.maximum_ns - 25_000_000_000))

    def test_manifest_size_record_size_and_raw_row_count_are_bounded(self):
        valid = self.text(self.fixture())
        for text in (valid + "x" * measure.MAX_LOG,
                     valid.replace('"type": "header"', '"type": "' + "x" * 2048 + '"', 1),
                     None, 123):
            with self.subTest(text_type=type(text)), self.assertRaises(measure.MeasurementError):
                measure.parse_manifest(text, 1, 6)
        with mock.patch.object(measure, "MAX_ROWS", 1), self.assertRaises(measure.MeasurementError):
            measure.analyze_mixer(self.rows(self.expected()), self.expected())

    def test_expected_sequence_revalidated_by_both_analysis_entrypoints(self):
        expected = self.expected()
        variants = [[], expected[:-1], expected + [expected[-1]]]
        for value in (True, 1.5, float("nan"), 0, self.maximum_ns + 1):
            changed = copy.deepcopy(expected)
            changed[0] = (*changed[0][:2], value)
            variants.append(changed)
        reversed_ids = list(expected)
        reversed_ids[0] = (2, 1, reversed_ids[0][-1])
        variants.append(reversed_ids)
        for malformed in variants:
            for analyzer, values in ((measure.analyze_encoded, self.audio(expected)),
                                     (measure.analyze_mixer, self.rows(expected))):
                with self.subTest(analyzer=analyzer.__name__, expected=malformed), self.assertRaises(measure.MeasurementError):
                    analyzer(values, malformed)

    def test_encoded_exact_full_baseline_and_restart_identities(self):
        for restart in (False, True):
            expected = self.expected(restart)
            report = measure.analyze_encoded(self.audio(expected), expected)
            self.assertTrue(report["valid_marker_match"], report)
            self.assertTrue(report["timing_gate"]["passed"], report)

    def test_encoded_missing_duplicate_old_replay_reorder_and_wrong_role_fail(self):
        expected = self.expected()
        audio = self.audio(expected)
        variants = [audio[:-1], audio + [(2, 6, audio[-1][-1] + 1)],
                    audio[:3] + [(1, 4, audio[2][-1] + 1)] + audio[3:],
                    [(2, 1, audio[0][-1])] + audio[1:],
                    [(True, 1, audio[0][-1])] + audio[1:],
                    [(1, True, audio[0][-1])] + audio[1:]]
        swapped = list(audio)
        swapped[0] = (*audio[1][:2], audio[0][-1])
        swapped[1] = (*audio[0][:2], audio[1][-1])
        variants.append(swapped)
        for variant in variants:
            with self.subTest(audio=variant):
                self.assert_encoded_rejected(variant, expected)

    def test_encoded_whole_timeline_boundaries_and_per_role_rebasing_rejected(self):
        expected = self.expected()
        audio = self.audio(expected)
        for difference, accepted in ((.04, True), (.04001, False), (20, False)):
            changed = [(role, event, stamp + (difference if role == 2 else 0)) for role, event, stamp in audio]
            report = measure.analyze_encoded(changed, expected)
            self.assertEqual(bool(report["valid_marker_match"] and report["timing_gate"]["passed"]), accepted)
        rebased = [(role, event, stamp - (16 if role == 2 else 0)) for role, event, stamp in audio]
        self.assert_encoded_rejected(rebased, expected)

    def test_encoded_global_origin_is_unknown_but_raw_global_shift_is_not(self):
        expected = self.expected()
        for shift in (-100, 0, 20):
            self.assertTrue(measure.analyze_encoded(self.audio(expected, shift), expected)["timing_gate"]["passed"])
        with self.assertRaises(measure.MeasurementError):
            measure.analyze_mixer(self.rows(expected, offset_ns=20_000_000_000), expected)

    def test_encoded_nonfinite_bool_or_nonmonotonic_onsets_rejected(self):
        expected = self.expected()
        for value in (float("nan"), float("inf"), True, "1", self.audio(expected)[0][-1]):
            audio = self.audio(expected)
            audio[1] = (*audio[1][:2], value)
            with self.subTest(value=value), self.assertRaises(measure.MeasurementError):
                measure.analyze_encoded(audio, expected)

    def test_entire_mixer_trace_requires_all_regions_and_absolute_onset_timing(self):
        for restart in (False, True):
            expected = self.expected(restart)
            for offset, accepted in ((0, True), (-2_000_000, True), (2_000_000, True), (2_001_000, False)):
                report = measure.analyze_mixer(self.rows(expected, offset), expected)
                self.assertTrue(report["valid_marker_match"])
                self.assertEqual(report["timing_gate"]["passed"], accepted)
            variants = [self.rows(expected, missing=2),
                        self.rows(expected, extra=expected[0][-1] - 1_000_000_000),
                        self.rows(expected, extra=expected[-1][-1] + 1_000_000_000)]
            if restart:
                variants.append(self.rows(expected, extra=self.origin + 18_700_000_000))
            for rows in variants:
                report = measure.analyze_mixer(rows, expected)
                self.assertFalse(report["valid_marker_match"])
                self.assertIsNone(report["timing_gate"]["passed"])

    def test_raw_csv_decimal_strings_accepted_without_losing_precision(self):
        expected = self.expected()
        rows = ({key: str(value) for key, value in row.items()} for row in self.rows(expected))
        self.assertTrue(measure.analyze_mixer(rows, expected)["timing_gate"]["passed"])

    def test_missing_raw_rows_or_islands_cannot_claim_complete_trace(self):
        expected = self.expected()
        rows = list(self.rows(expected))
        dropped_quiet = rows[:100] + rows[101:]
        dropped_loud = rows[:25] + rows[26:]
        islands = [row for row in rows if any(-5_000_000 <= row["timestamp_ns"] - stamp < 35_000_000
                                              for _, _, stamp in expected)]
        for broken in (dropped_quiet, dropped_loud, islands):
            with self.subTest(rows=len(broken)), self.assertRaises(measure.MeasurementError):
                measure.analyze_mixer(broken, expected)

    def test_raw_trace_needs_pre_and_post_marker_coverage(self):
        expected = self.expected()
        rows = list(self.rows(expected))
        for broken in (rows[11:], rows[:-11]):
            with self.assertRaises(measure.MeasurementError):
                measure.analyze_mixer(broken, expected)

    def test_raw_continuity_allows_only_nanosecond_rounding_not_one_ms_gaps(self):
        expected = self.expected()
        rows = list(self.rows(expected))
        for jitter, valid in ((2, True), (3, False), (-3, False)):
            changed = [dict(row) for row in rows]
            changed[100]["timestamp_ns"] += jitter
            if valid:
                self.assertTrue(measure.analyze_mixer(changed, expected)["timing_gate"]["passed"])
            else:
                with self.subTest(jitter=jitter), self.assertRaises(measure.MeasurementError):
                    measure.analyze_mixer(changed, expected)

    def test_raw_csv_reader_strict_header_columns_encoding_and_finite_bounds(self):
        header = "timestamp_ns,frames,rms\n"
        valid = header + "1000000000,48,0.1\n1001000000,48,0\n"
        variants = [valid.replace("timestamp_ns", "timestamp"), header + header,
                    valid.replace("1000000000,48,0.1", "1000000000,48,0.1,extra"),
                    valid.replace("1000000000,48,0.1", "1000000000,48"),
                    valid.replace("1000000000,48,0.1", '"1000000000",48,0.1'),
                    valid + "\n", valid.replace("0.1", "\N{LATIN SMALL LETTER E WITH ACUTE}"),
                    valid.replace("1000000000", "01000000000"), valid.replace(",48,", ",048,"),
                    valid.replace("1000000000", "+1000000000"),
                    header + "1,48," + "0" * 129 + "\n"]
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "private.csv"
            path.write_text(valid, encoding="utf-8")
            rows = measure.read_mixer_rows(path)
            self.assertEqual(len(rows), 2)
            self.assertEqual(set(rows[0]), {"timestamp_ns", "frames", "rms"})
            for value in variants:
                path.write_text(value, encoding="utf-8")
                with self.subTest(text=value[:100]), self.assertRaises(measure.MeasurementError):
                    measure.read_mixer_rows(path)
            path.write_text(valid, encoding="utf-8")
            with mock.patch.object(measure, "MAX_TRACE_BYTES", 20), self.assertRaises(measure.MeasurementError):
                measure.read_mixer_rows(path)
            with mock.patch.object(measure, "MAX_ROWS", 1), self.assertRaises(measure.MeasurementError):
                measure.read_mixer_rows(path)

    def test_raw_invalid_integer_fields_nonfinite_levels_and_nonmonotonic_trace(self):
        expected = self.expected()
        row = {"timestamp_ns": expected[0][-1], "frames": 48, "rms": .1}
        variants = [[row, row], [dict(row, timestamp_ns=-1)], [dict(row, timestamp_ns=1 << 63)],
                    [row, dict(row, timestamp_ns=row["timestamp_ns"] + 131_000_000_000)]]
        for key in ("timestamp_ns", "frames"):
            variants += [[dict(row, **{key: value})] for value in (True, 1.5, "1.5", None, float("nan"))]
        variants += [[dict(row, frames=value)] for value in (0, 49)]
        variants += [[dict(row, rms=value)] for value in (float("nan"), float("inf"), -.1, True)]
        for rows in variants:
            with self.subTest(rows=rows), self.assertRaises(measure.MeasurementError):
                measure.analyze_mixer(rows, expected)

    def test_weak_empty_and_unexpected_short_or_long_regions_never_ignored(self):
        expected = self.expected()
        for rows in ([], [{"timestamp_ns": 1, "frames": 48, "rms": .001}],
                     self.rows(expected, duration_ns=2_000_000), self.rows(expected, duration_ns=90_000_000)):
            with self.assertRaises(measure.MeasurementError):
                measure.analyze_mixer(rows, expected)

    def invoke(self, directory, restart=True, encoded_role_shift=0, raw_offset_ns=0, decode_error=None):
        before, after, trace = directory / "private-before.log", directory / "private-after.log", directory / "private.csv"
        before.write_text(self.text(self.fixture(1, 3 if restart else 6)), encoding="utf-8")
        after.write_text(self.text(self.fixture(2)), encoding="utf-8")
        expected = self.expected(restart)
        trace.write_text("timestamp_ns,frames,rms\n" + "".join(
            f"{row['timestamp_ns']},{row['frames']},{row['rms']}\n"
            for row in self.rows(expected, raw_offset_ns)), encoding="utf-8")
        audio = [(role, event, stamp + (encoded_role_shift if role == 2 else 0))
                 for role, event, stamp in self.audio(expected)]
        argv = ["--recording", str(directory / "private-recording.mkv"), "--predecessor-log", str(before),
                "--mixer-trace", str(trace)]
        if restart:
            argv += ["--successor-log", str(after)]
        output = io.StringIO()
        with mock.patch.object(measure, "measure_audio", side_effect=decode_error,
                               return_value=(audio, {"video_analyzed": False})), contextlib.redirect_stdout(output):
            code = measure.main(argv)
        return code, json.loads(output.getvalue()), output.getvalue()

    def test_cli_requires_raw_absolute_timing_and_complete_encoded_timeline(self):
        for restart, shift, raw, expected_code in ((False, 0, 0, 0), (True, 0, 0, 0),
                                                  (True, .05, 0, 3), (True, 0, 3_000_000, 3)):
            with self.subTest(restart=restart, shift=shift, raw=raw), tempfile.TemporaryDirectory() as temporary:
                code, report, serialized = self.invoke(Path(temporary), restart, shift, raw)
                self.assertEqual(code, expected_code)
                self.assertEqual(report["passed"], code == 0)
                self.assertFalse(report["queued_stale_replay_verified"])
                self.assertFalse(report["decode"]["video_analyzed"])
                self.assertIn("NOT A/V", report["scope"])
                for private in (temporary, "private-recording", str(self.origin),
                                "sender_session", "clock_epoch", "capture_origin_ns"):
                    self.assertNotIn(private, serialized)

    def test_cli_decoder_and_io_errors_never_disclose_private_paths(self):
        for exception in (measure.bounded.MeasurementError("decoder failed /private/secret.mkv"),
                          OSError("cannot open /private/secret.log"),
                          measure.MeasurementError("/private/secret source invalid")):
            with self.subTest(exception=type(exception)), tempfile.TemporaryDirectory() as temporary:
                code, report, serialized = self.invoke(Path(temporary), decode_error=exception)
                self.assertEqual(code, 1)
                self.assertFalse(report["passed"])
                for private in (temporary, "secret", "private/"):
                    self.assertNotIn(private, serialized)

    @staticmethod
    def metadata():
        return {"format": {"duration": "42"}, "streams": [
            {"index": 0, "codec_type": "video", "width": 640, "height": 360},
            {"index": 1, "codec_type": "audio", "sample_rate": "48000"}]}

    def test_decode_preserves_pts_and_bounds_raw_output_without_real_media_tools(self):
        frames = [{"pts_time": -.01, "nb_samples": 1024}, {"pts_time": .011333333, "nb_samples": 1024}]
        with mock.patch.object(measure.bounded, "probe", side_effect=[self.metadata(), {"frames": frames}]), \
                mock.patch.object(measure.bounded, "run", return_value=b"\0" * 8192) as decode, \
                mock.patch.object(measure, "detect_audio", return_value=[]) as detect:
            _, metadata = measure.measure_audio("unused", "ffmpeg", "ffprobe")
        self.assertEqual(decode.call_count, 1)
        self.assertEqual(decode.call_args.args[1], 121 * 48000 * 4)
        for argument in ("-copyts", "-nostdin", "121", "-vn", "0:1"):
            self.assertIn(argument, decode.call_args.args[0])
        self.assertAlmostEqual(detect.call_args.args[1][0], -.01)
        self.assertEqual(metadata["decoded_audio_samples"], 2048)
        self.assertFalse(metadata["video_analyzed"])

    def test_invalid_recording_metadata_and_mismatched_pcm_rejected(self):
        variants = []
        for duration in ("0", "121", "nan", "inf", True):
            metadata = self.metadata()
            metadata["format"]["duration"] = duration
            variants.append(metadata)
        for streams in ([], [None], [{"index": 1, "codec_type": "video"}], self.metadata()["streams"] * 5):
            metadata = self.metadata()
            metadata["streams"] = streams
            variants.append(metadata)
        for rate in ("44100", True, None):
            metadata = self.metadata()
            metadata["streams"][1]["sample_rate"] = rate
            variants.append(metadata)
        for metadata in variants:
            with self.subTest(metadata=metadata), mock.patch.object(measure.bounded, "probe", return_value=metadata), \
                    mock.patch.object(measure.bounded, "run") as decode:
                with self.assertRaises((measure.MeasurementError, measure.bounded.MeasurementError)):
                    measure.measure_audio("unused", "ffmpeg", "ffprobe")
                decode.assert_not_called()
        with mock.patch.object(measure.bounded, "probe", side_effect=[self.metadata(), {"frames": [
            {"pts_time": 0, "nb_samples": 1024}, {"pts_time": .021333333, "nb_samples": 1024}]}]), \
                mock.patch.object(measure.bounded, "run", return_value=b"\0" * 8188):
            with self.assertRaises(measure.MeasurementError):
                measure.measure_audio("unused", "ffmpeg", "ffprobe")


if __name__ == "__main__":
    unittest.main()
