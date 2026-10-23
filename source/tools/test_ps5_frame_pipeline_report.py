#!/usr/bin/env python3
# Lumen, 2026. SPDX-License-Identifier: GPL-2.0-only
"""Measurement integrity checks for the offline PS5 pipeline report."""

import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest

import ps5_frame_pipeline_report as report


def window(index=1, *, present=48, guest=40, repeat=6, blank=2, **changes):
    fields = dict(window=index, start_ticks=index * 2_000_000_000,
                  end_ticks=(index + 1) * 2_000_000_000, frequency=1_000_000_000,
                  elapsed_ms=2000.0, window_present=present, guest_submission=guest,
                  host_repeat=repeat, host_blank=blank, failed=1,
                  interval_count=present - (index == 1), avg_interval_ms=41.0,
                  min_interval_ms=30.0, max_interval_ms=60.0,
                  acquire_total_ms=1.0, submit_present_total_ms=2.0,
                  cumulative_present=present * index, cumulative_guest_submission=guest * index,
                  cumulative_host_repeat=repeat * index, cumulative_host_blank=blank * index,
                  cumulative_failed=index, cumulative_acquire_ticks=index * 1_000_000,
                  cumulative_submit_present_ticks=index * 2_000_000)
    fields.update(changes)
    return "PS5_FRAME_PIPELINE_V1 " + " ".join(f"{key}={value}" for key, value in fields.items())


def gpu_slice(sequence=1, start=2_500_000_000, **changes):
    fields = dict(sequence=sequence, tid=7, start_ns=start, end_ns=start + 100,
                  submit_id=4, queue_id=0, type=0, epoch_token=11,
                  scheduler_tick_begin=10, scheduler_tick_end=12,
                  admission_ns=10, process_ns=20, finish_slice_ns=60, close_ns=10,
                  finish_calls=2, finish_ns=50, native_wait_calls=2, native_wait_ns=40,
                  priority_wait_calls=1, priority_wait_ns=5, native_submits=2,
                  buffer_readback_bytes=4096, image_readback_bytes=8192, image_seed_bytes=1024,
                  queued_submissions=1, ready_queue_heads=1, pending_commands=0,
                  private_eligible=-1, complete=0)
    fields.update(changes)
    return "PS5_GPU_SLICE_TIMING_V1 " + " ".join(f"{key}={value}" for key, value in fields.items())


GPU_READY = "PS5_GPU_SLICE_TRACE_V1 enabled=1 limit=8192 clock=monotonic_ns coverage=prefix"


def present_join(sequence=1, accepted=1, stamp=2_500_000_050, **changes):
    fields = dict(sequence=sequence, accepted_id=accepted, request_id=100 + sequence,
                  slice_sequence=1, submit_id=4, epoch_token=11, producer_tick=11,
                  present_ns=stamp, present_tick=5, bus=0, outcome="accepted")
    fields.update(changes)
    return "PS5_GPU_PRESENT_JOIN_V1 " + " ".join(f"{key}={value}" for key, value in fields.items())


def hle_window(index=1, **changes):
    fields = dict(pid=10, run=2, thread=3, window=index, start_ns=2_100_000_000,
                  end_ns=3_900_000_000, attempts=3, finished=2, failed=1, pending=1,
                  overflow_calls=0, clock_valid=1, final=1, inclusive_thread_time=1, frame_aligned=0)
    fields.update(changes)
    return "FEX_HLE_WINDOW_V1 " + " ".join(f"{key}={value}" for key, value in fields.items())


def hle_target(index=1, **changes):
    fields = dict(pid=10, run=2, thread=3, window=index, start_ns=2_100_000_000,
                  end_ns=3_900_000_000, target="0x1234", calls=3, finished=2, failed=1,
                  short_calls=1, short_fallback=1, ordinary_calls=2,
                  admission_wall_ns=100, admission_cpu_ns=10, body_wall_ns=80, body_cpu_ns=40,
                  readmission_wall_ns=30, readmission_cpu_ns=3, clock_failures=0,
                  inclusive_thread_time=1, frame_aligned=0)
    fields.update(changes)
    return "FEX_HLE_TARGET_V1 " + " ".join(f"{key}={value}" for key, value in fields.items())


class PipelineReportTests(unittest.TestCase):
    def test_buffer_hash_legacy_complete_and_mixed_coverage(self):
        legacy = report.parse_gpu_lines([GPU_READY, gpu_slice()])
        old = report.gpu_report(legacy, report.parse_lines([window()])["windows"])["buffer_hash_revalidation"]
        self.assertFalse(old["available"])
        self.assertIsNone(old["totals"]["buffer_hash_copy_avoided_bytes"])
        self.assertIsNone(old["fallback_mask_or"])
        counters = {key: 0 for key in report.GPU_BUFFER_HASH}
        counters.update(buffer_hash_considered=3, buffer_hash_attempts=2, buffer_hash_pages=4,
                        buffer_hash_bytes=4096, buffer_hash_copy_avoided_bytes=4096,
                        buffer_hash_fallbacks=1, buffer_hash_fallback_mask=3,
                        buffer_revalidation_copy_bytes=1024)
        parsed = report.parse_gpu_lines([GPU_READY, gpu_slice(), gpu_slice(sequence=2, **counters),
                    gpu_slice(sequence=3, **dict(counters, buffer_hash_fallback_mask=5))])
        result = report.gpu_report(parsed, report.parse_lines([window()])["windows"])["buffer_hash_revalidation"]
        self.assertTrue(result["available"])
        self.assertEqual((result["covered_slices"], result["unavailable_slices"]), (2, 1))
        self.assertEqual(result["fallback_mask_or"], 7)
        self.assertNotIn("buffer_hash_fallback_mask", result["totals"])
        self.assertEqual(result["totals"]["buffer_hash_copy_avoided_bytes"], 8192)
        self.assertEqual(result["totals"]["buffer_revalidation_copy_bytes"], 2048)
        self.assertEqual(result["attempted_slices"], 2)
        self.assertEqual(result["copy_avoiding_slices"], 2)

    def test_partial_buffer_hash_counters_are_rejected(self):
        for missing in report.GPU_BUFFER_HASH:
            with self.subTest(missing=missing), self.assertRaisesRegex(report.ReportError, "buffer-hash counters"):
                report.parse_gpu_lines([GPU_READY, gpu_slice(**{key: 0 for key in report.GPU_BUFFER_HASH if key != missing})])
        with self.assertRaisesRegex(report.ReportError, "buffer-hash counters"):
            report.parse_gpu_lines([GPU_READY, gpu_slice(buffer_hash_considered=1)])

    def test_optional_import_batch_versions_preserve_unknown_and_mask_union(self):
        legacy = report.host_import_report(report.parse_gpu_lines([GPU_READY, gpu_slice()])["slices"])
        self.assertFalse(legacy["available"])
        counters = {key: 0 for key in report.GPU_HOST_IMPORT}
        counters.update(image_host_import_attempts=2, image_host_import_bytes=4096,
                        image_host_import_denial_mask=3)
        old = report.host_import_report(report.parse_gpu_lines([GPU_READY, gpu_slice(**counters)])["slices"])
        self.assertTrue(old["available"])
        self.assertIsNone(old["batch_counters"]["totals"]["image_host_import_batches"])
        batch = dict(image_host_import_batches=1, image_host_import_batch_owners=2,
                     image_host_import_batch_cap_stops=0)
        slices = report.parse_gpu_lines([GPU_READY, gpu_slice(**counters, **batch),
            gpu_slice(sequence=2, **dict(counters, image_host_import_denial_mask=5), **batch)])["slices"]
        result = report.host_import_report(slices)
        self.assertEqual(result["denial_mask_or"], 7)
        self.assertEqual(result["batch_counters"]["covered_slices"], 2)
        self.assertEqual(result["batch_counters"]["totals"]["image_host_import_batch_owners"], 4)

    def test_partial_batch_counters_or_batch_without_import_are_rejected(self):
        counters = {key: 0 for key in report.GPU_HOST_IMPORT}
        with self.assertRaises(report.ReportError):
            report.parse_gpu_lines([GPU_READY, gpu_slice(**counters, image_host_import_batches=1)])
        with self.assertRaises(report.ReportError):
            report.parse_gpu_lines([GPU_READY, gpu_slice(**{key: 0 for key in report.GPU_HOST_IMPORT_BATCH})])

    def test_separates_guest_flip_from_host_present_and_unknown_pixels(self):
        result = report.build_report(report.parse_lines([window(), window(2)]))
        totals = result["observed_complete_windows"]
        self.assertEqual(totals["accepted_presentations_per_second"], 24.0)
        self.assertEqual(totals["guest_flip_submissions_per_second"], 20.0)
        self.assertEqual(totals["host_repeats_per_second"], 3.0)
        self.assertEqual(totals["host_blanks_per_second"], 1.0)
        self.assertIsNone(totals["p95_ms"])
        self.assertFalse(result["coverage"]["pixel_unique_measured"])
        self.assertFalse(result["coverage"]["per_frame_dependencies_available"])

    def test_tick_selection_does_not_prorate_crossing_windows(self):
        parsed = report.parse_lines([window(), window(2), window(3)])
        result = report.build_report(parsed, start_ticks=3_000_000_000, end_ticks=7_000_000_000)
        self.assertEqual([r["window"] for r in result["windows"]], [2])
        self.assertEqual(result["observed_complete_windows"]["elapsed_seconds"], 2)

    def test_scene_markers_exclude_uncertain_first_completion(self):
        lines = [window(), "SCENE_READY", window(2), window(3), "SCENE_END", window(4)]
        parsed = report.parse_lines(lines)
        result = report.build_report(parsed, after_line=report.marker_line(lines, "SCENE_READY"),
                                     before_line=report.marker_line(lines, "SCENE_END"))
        self.assertEqual([r["window"] for r in result["windows"]], [3])
        self.assertEqual(result["selection"]["after_marker_crossing_window_excluded"], 2)

    def test_gap_is_reported_and_not_counted_as_observed_time(self):
        result = report.build_report(report.parse_lines([window(), window(3)]))
        self.assertFalse(result["coverage"]["contiguous_windows"])
        self.assertEqual(result["selection"]["gaps"][0]["missing_ticks"], 2_000_000_000)
        self.assertEqual(result["observed_complete_windows"]["elapsed_seconds"], 4.0)
        self.assertEqual(result["observed_complete_windows"]["present"], 96)

    def test_identical_duplicate_counted_once(self):
        result = report.build_report(report.parse_lines([window(), window(), window(2)]))
        self.assertEqual(result["coverage"]["duplicates_ignored"], 1)
        self.assertEqual(result["observed_complete_windows"]["present"], 96)

    def test_conflicting_or_restarted_session_rejected(self):
        for lines in ([window(), window(acquire_total_ms=9.0)],
                      ["PS5_FRAME_PIPELINE_TRACE_V1 enabled=1", window(),
                       "PS5_FRAME_PIPELINE_TRACE_V1 enabled=1", window(2)]):
            with self.subTest(lines=lines), self.assertRaises(report.ReportError):
                report.parse_lines(lines)

    def test_malformed_and_inconsistent_records_rejected(self):
        cases = [window(guest_submission=41), window(frequency=0), window(elapsed_ms=1900),
                 window(interval_count=49), window(avg_interval_ms=float("nan")),
                 window(min_interval_ms=42), window(cumulative_present=1),
                 window() + " window=1", window().replace(" failed=1", "")]
        for line in cases:
            with self.subTest(line=line), self.assertRaises(report.ReportError):
                report.parse_lines([line])

    def test_contiguous_cumulative_corruption_rejected(self):
        for line in (window(2, cumulative_present=97, cumulative_guest_submission=81),
                     window(2, cumulative_failed=1), window(2, cumulative_acquire_ticks=2_001_000),
                     window(2, frequency=2_000_000_000, elapsed_ms=1000)):
            with self.subTest(line=line), self.assertRaises(report.ReportError):
                report.parse_lines([window(), line])

    def test_selection_ambiguity_rejected(self):
        with self.assertRaises(report.ReportError):
            report.marker_line(["ready", "ready"], "ready")
        with self.assertRaises(report.ReportError):
            report.marker_line(["ready"], "missing")
        parsed = report.parse_lines([window()])
        for kwargs in ({"start_ticks": 10, "end_ticks": 1}, {"first_window": 3},
                       {"after_line": 2}, {"after_line": 3, "before_line": 2}):
            with self.subTest(kwargs=kwargs), self.assertRaises(report.ReportError):
                report.build_report(parsed, **kwargs)

    def test_empty_present_window_has_no_invented_intervals(self):
        parsed = report.parse_lines([window(present=0, guest=0, repeat=0, blank=0,
                                            interval_count=0, avg_interval_ms=0, min_interval_ms=0,
                                            max_interval_ms=0)])
        totals = report.build_report(parsed)["observed_complete_windows"]
        self.assertEqual(totals["accepted_presentations_per_second"], 0)
        self.assertIsNone(totals["reported_interval_weighted_mean_ms"])

    def test_cli_writes_json_without_overwriting_existing_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "input.log"
            output = Path(directory) / "report.json"
            log.write_text(window() + "\n" + window(2) + "\n")
            self.assertEqual(report.main([str(log), "--first-window", "2", "-o", str(output)]), 0)
            self.assertEqual(json.loads(output.read_text())["selection"]["first_window"], 2)
            before = output.read_bytes()
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                report.main([str(log), "-o", str(output)])
            self.assertEqual(output.read_bytes(), before)

    def test_critical_path_join_requires_explicit_matching_clock(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "input.log"
            log.write_text(window() + "\n")
            errors = io.StringIO()
            with contextlib.redirect_stderr(errors), self.assertRaises(SystemExit):
                report.main([str(log), "--critical-path-log", str(log)])
            self.assertIn("requires --monotonic-ns-clock", errors.getvalue())

    def test_gpu_overlap_excludes_partial_slice_and_keeps_nested_times_separate(self):
        pipeline = report.parse_lines([window()])["windows"]
        gpu = report.parse_gpu_lines([GPU_READY, gpu_slice(), gpu_slice(2, start=1_999_999_950),
                                      gpu_slice(3, start=5_000_000_000)])
        result = report.gpu_report(gpu, pipeline)
        self.assertEqual(result["fully_contained_slices"], 1)
        self.assertEqual(result["window_straddling_slices"], 1)
        self.assertEqual(result["outside_selection_slices"], 1)
        self.assertEqual(result["phase_wall_time"]["finish_slice_ns"]["sum_ns"], 60)
        self.assertEqual(result["nested_inclusive_wall_time"]["finish"]["sum_ns"], 50)
        self.assertEqual(result["scheduled_work"]["image_readback_bytes"], 8192)
        self.assertEqual(result["completed_command_slices"], 0)
        self.assertFalse(result["causal_frame_join"])

    def test_gpu_scope_across_adjacent_windows_is_included_once(self):
        pipeline = report.parse_lines([window(), window(2)])["windows"]
        gpu = report.parse_gpu_lines([GPU_READY, gpu_slice(start=3_999_999_950)])
        self.assertEqual(report.gpu_report(gpu, pipeline)["fully_contained_slices"], 1)

    def test_gpu_trace_loss_and_duplicates_are_visible(self):
        gpu = report.parse_gpu_lines([GPU_READY, gpu_slice(2), gpu_slice(2), gpu_slice(4),
                                     "PS5_GPU_SLICE_TRACE_V1 limit_reached=1 attempted_prefix=8192"])
        result = report.gpu_report(gpu, report.parse_lines([window()])["windows"])
        self.assertTrue(result["leading_prefix_missing"])
        self.assertTrue(result["trace_limit_reached"])
        self.assertEqual(result["duplicates_ignored"], 1)
        self.assertEqual(result["all_observed_sequence_gaps"], [{"after": 2, "before": 4}])

    def test_gpu_unknown_clock_and_corrupted_partition_rejected(self):
        for lines in ([GPU_READY, gpu_slice(close_ns=11)],
                      [GPU_READY, gpu_slice(), gpu_slice(submit_id=99)],
                      [GPU_READY, GPU_READY, gpu_slice()]):
            with self.subTest(lines=lines), self.assertRaises(report.ReportError):
                report.parse_gpu_lines(lines)
        gpu = report.parse_gpu_lines([GPU_READY.replace("monotonic_ns", "steady_ns"), gpu_slice()])
        with self.assertRaises(report.ReportError):
            report.gpu_report(gpu, report.parse_lines([window()])["windows"])

    def test_gpu_cli_produces_temporal_report(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "input.log"
            log.write_text(window() + "\n" + GPU_READY + "\n" + gpu_slice() + "\n")
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                report.main([str(log), "--gpu-slice-log", str(log), "--monotonic-ns-clock"])
            result = json.loads(output.getvalue())
            self.assertEqual(result["gpu_slice_temporal_overlap"]["fully_contained_slices"], 1)

    def test_exact_flip_join_accepts_callback_before_slice_closes(self):
        parsed = report.parse_gpu_lines([GPU_READY, present_join(), gpu_slice(complete=0)])
        result = report.gpu_report(parsed, report.parse_lines([window()])["windows"])
        self.assertTrue(result["causal_frame_join"])
        dependency = result["explicit_present_dependencies"]
        self.assertEqual(len(dependency["resolved_edges"]), 1)
        self.assertTrue(dependency["resolved_edges"][0]["presented_before_slice_closed"])
        self.assertFalse(dependency["full_guest_frame_critical_path_verified"])

    def test_exact_flip_join_requires_all_producer_identities(self):
        joins = [present_join(slice_sequence=9), present_join(2, submit_id=5),
                 present_join(3, epoch_token=12), present_join(4, producer_tick=99),
                 present_join(5, stamp=2_499_999_999)]
        parsed = report.parse_gpu_lines([GPU_READY, gpu_slice(), *joins])
        result = report.gpu_report(parsed, report.parse_lines([window()])["windows"])
        dependency = result["explicit_present_dependencies"]
        self.assertFalse(dependency["resolved_edges"])
        self.assertEqual(len(dependency["unresolved_edges"]), 5)
        self.assertFalse(result["causal_frame_join"])

    def test_multiple_layers_count_once_and_missing_guest_ids_exclude_intervals(self):
        joins = [present_join(), present_join(2, bus=1),
                 present_join(3, accepted=2, stamp=2_541_000_050),
                 present_join(4, accepted=4, stamp=2_641_000_050)]
        parsed = report.parse_gpu_lines([GPU_READY, gpu_slice(), *joins])
        result = report.gpu_report(parsed, report.parse_lines([window()])["windows"])["explicit_present_dependencies"]
        self.assertEqual(len(result["resolved_edges"]), 4)
        self.assertEqual(result["observed_accepted_guest_events"], 3)
        self.assertEqual(result["guest_accepted_interval_statistics"]["count"], 1)
        self.assertEqual(result["guest_accepted_interval_statistics"]["mean_ns"], 41_000_000)
        self.assertEqual(result["interval_gaps"], [{"after_accepted_id": 2, "before_accepted_id": 4}])

    def test_ambiguous_multilayer_stamps_do_not_fabricate_interval(self):
        parsed = report.parse_gpu_lines([GPU_READY, gpu_slice(), present_join(),
                                        present_join(2, stamp=2_500_000_060, bus=1),
                                        present_join(3, accepted=2, stamp=2_541_000_050)])
        result = report.gpu_report(parsed, report.parse_lines([window()])["windows"])["explicit_present_dependencies"]
        self.assertEqual(result["ambiguous_event_timestamps"], [1])
        self.assertFalse(result["intervals"])

    def test_join_duplicate_and_cap_are_explicit(self):
        parsed = report.parse_gpu_lines([GPU_READY, gpu_slice(), present_join(), present_join(),
                                        "PS5_GPU_PRESENT_JOIN_TRACE_V1 limit_reached=1 attempted_prefix=8192"])
        result = report.gpu_report(parsed, report.parse_lines([window()])["windows"])["explicit_present_dependencies"]
        self.assertEqual(result["duplicate_join_records_ignored"], 1)
        self.assertTrue(result["join_limit_reached"])
        with self.assertRaises(report.ReportError):
            report.parse_present_joins([present_join(outcome="failed")])
        with self.assertRaises(report.ReportError):
            report.parse_present_joins([present_join(), present_join(request_id=999)])

    def test_hle_complete_window_preserves_pending_and_phase_census(self):
        result = report.hle_report([hle_window(), hle_target()], report.parse_lines([window()])["windows"])
        self.assertEqual(result["fully_contained_windows"], 1)
        self.assertEqual(result["window_totals"]["pending"], 1)
        self.assertEqual(result["targets"][0]["target"], "0x1234")
        self.assertEqual(result["targets"][0]["ordinary_calls"], 2)
        self.assertEqual(result["targets"][0]["readmission_wall_ns"], 30)
        self.assertFalse(result["causal_frame_join"])

    def test_hle_truncated_target_census_is_excluded(self):
        result = report.hle_report([hle_window()], report.parse_lines([window()])["windows"])
        self.assertEqual(result["fully_contained_windows"], 0)
        self.assertEqual(result["incomplete_target_windows"], 1)
        self.assertEqual(result["window_totals"]["attempts"], 0)

    def test_hle_short_outcomes_preserve_legacy_and_mixed_coverage(self):
        windows = report.parse_lines([window()])["windows"]
        legacy = report.hle_report([hle_window(), hle_target()], windows)
        self.assertFalse(legacy["short_outcomes"]["available"])
        self.assertIsNone(legacy["targets"][0]["short_outcomes"]["totals"]["short_completed"])
        outcomes = dict(short_calls=2, ordinary_calls=1, short_fallback=1,
                        short_completed=1, short_demoted=1, short_calls_are_entries=1)
        result = report.hle_report([hle_window(), hle_target(),
                                   hle_window(2), hle_target(2, **outcomes)], windows)
        outcome = result["targets"][0]["short_outcomes"]
        self.assertEqual(outcome, result["short_outcomes"])
        self.assertEqual((outcome["covered_target_windows"], outcome["unavailable_target_windows"]), (1, 1))
        self.assertTrue(outcome["short_calls_are_entries"])
        self.assertEqual(outcome["totals"], {"short_calls": 2, "short_completed": 1, "short_demoted": 1})
        self.assertEqual(result["targets"][0]["ordinary_calls"], 3)
        self.assertEqual(result["targets"][0]["calls"], 6)

    def test_hle_short_outcome_partial_and_inconsistent_groups_rejected(self):
        windows = report.parse_lines([window()])["windows"]
        fields = dict(short_completed=0, short_demoted=0, short_calls_are_entries=1)
        for missing in fields:
            with self.subTest(missing=missing), self.assertRaisesRegex(report.ReportError, "short-outcome"):
                report.hle_report([hle_window(), hle_target(**{k: v for k, v in fields.items() if k != missing})], windows)
        for changes in (dict(short_completed=2), dict(short_completed=1, short_demoted=1),
                        dict(short_calls_are_entries=0), dict(short_calls=3, ordinary_calls=0,
                             short_fallback=0, short_completed=3)):
            with self.subTest(changes=changes), self.assertRaisesRegex(report.ReportError, "short-outcome"):
                report.hle_report([hle_window(), hle_target(**dict(fields, **changes))], windows)

    def test_hle_short_outcomes_exclude_boundary_and_truncated_census(self):
        outcomes = dict(short_completed=1, short_demoted=0, short_calls_are_entries=1)
        lines = [hle_window(), hle_target(**outcomes),
                 hle_window(2, start_ns=1_000_000_000), hle_target(2, start_ns=1_000_000_000, **outcomes),
                 hle_window(3, attempts=4, pending=2), hle_target(3, **outcomes)]
        result = report.hle_report(lines, report.parse_lines([window()])["windows"])
        self.assertEqual(result["short_outcomes"]["covered_target_windows"], 1)
        self.assertEqual(result["short_outcomes"]["totals"]["short_completed"], 1)
        self.assertEqual(result["window_straddling_windows"], 1)
        self.assertEqual(result["incomplete_target_windows"], 1)

    def test_hle_straddling_invalid_and_orphan_records_not_prorated(self):
        lines = [hle_window(start_ns=1_000_000_000), hle_target(start_ns=1_000_000_000),
                 hle_window(2, clock_valid=0), hle_target(2), hle_target(3)]
        result = report.hle_report(lines, report.parse_lines([window()])["windows"])
        self.assertEqual(result["window_straddling_windows"], 1)
        self.assertEqual(result["invalid_clock_windows"], 1)
        self.assertEqual(result["orphan_target_records"], 1)
        self.assertFalse(result["targets"])

    def test_hle_process_selection_and_overflow_bucket(self):
        lines = [hle_window(overflow_calls=3), hle_target(target="0"),
                 hle_window(pid=20), hle_target(pid=20)]
        windows = report.parse_lines([window()])["windows"]
        with self.assertRaises(report.ReportError):
            report.hle_report(lines, windows)
        result = report.hle_report(lines, windows, pid=10)
        self.assertEqual(result["window_totals"]["overflow_calls"], 3)
        self.assertTrue(result["targets"][0]["overflow_bucket"])

    def test_hle_census_corruption_rejected(self):
        for lines in ([hle_window(finished=4), hle_target()],
                      [hle_window(), hle_target(failed=0)],
                      [hle_window(), hle_target(calls=4)],
                      [hle_window(), hle_target(end_ns=4_000_000_000)],
                      [hle_window(), hle_target(short_fallback=3)]):
            with self.subTest(lines=lines), self.assertRaises(report.ReportError):
                report.hle_report(lines, report.parse_lines([window()])["windows"])

    def test_hle_duplicate_ignored_and_cli_integration(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "input.log"
            log.write_text("\n".join([window(), hle_window(), hle_target(), hle_target()]))
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                report.main([str(log), "--hle-log", str(log), "--monotonic-ns-clock"])
            result = json.loads(output.getvalue())["hle_temporal_overlap"]
            self.assertEqual(result["duplicates_ignored"], 1)
            self.assertEqual(result["window_totals"]["attempts"], 3)


if __name__ == "__main__":
    unittest.main()
