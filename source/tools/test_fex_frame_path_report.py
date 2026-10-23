#!/usr/bin/env python3
"""Contract fixtures for offline completed-span/frame reports."""

from __future__ import annotations

import contextlib
import io
import json
import pathlib
import tempfile
import unittest

import fex_frame_path_report as report


def complete(sequence: int, reason: str, start: int, end: int | None = None,
             *, kind: str = "event", tid: int = 7, pid: int = 10, object: int = 0,
             dependency: int = 0, frame: int = 0, hint: int = 0,
             cpu: int = 0, **extra: int) -> str:
    if end is None:
        end = start
    fields = dict(pid=pid, tid=tid, sequence=sequence, kind=kind, reason=reason,
                  object=object, dependency=dependency, value=0,
                  start_ns=start, end_ns=end, wall_ns=end - start,
                  thread_cpu_start_ns=100, thread_cpu_end_ns=100 + cpu,
                  thread_cpu_ns=cpu, frame_event_id=frame,
                  frame_hint_start=hint, frame_hint_end=hint,
                  frame_hint_noncausal=1, inclusive=1, completed_only=1,
                  census_complete=0, long_threshold_ns=1000000,
                  detail_per_second=64, detail_per_thread=4096,
                  dropped_spans=0, dropped_events=0, dropped_rate=0,
                  dropped_lifetime=0, diagnostic_only=1)
    fields.update(extra)
    return "10-06 22:00:00.000 10 7 I LumenCriticalPath: LUMEN_CRITICAL_PATH_COMPLETE_V1 " + " ".join(f"{key}={value}" for key, value in fields.items())


def summary(index: int, timestamp: int, **extra: int) -> str:
    fields = dict(pid=10, tid=7, report=index, monotonic_ns=timestamp,
                  target_capacity=64, target_overflow_calls=0,
                  invalid_intervals=0, invalid_reasons=0, detail_emitted=2,
                  dropped_spans=0, dropped_events=0, dropped_rate=0,
                  dropped_lifetime=0, write_failures=0, reports_suppressed=0,
                  frame_overflow=0, report_period_ns=5000000000,
                  reports_per_thread=128, final=0, completed_only=1,
                  census_complete=0, frame_hint_noncausal=1, diagnostic_only=1)
    fields.update(extra)
    return "LUMEN_CRITICAL_PATH_SUMMARY_V1 " + " ".join(f"{key}={value}" for key, value in fields.items())


def total(index: int, calls: int, wall: int, cpu: int, maximum: int,
          *, kind: str = "reason", target: int = 0, **extra: int) -> str:
    fields = dict(pid=10, tid=7, report=index, kind=kind, reason="hle_call",
                  target=target, calls=calls, thread_wall_ns=wall,
                  thread_cpu_ns=cpu, max_wall_ns=maximum,
                  hist_lt1us=calls, hist_lt10us=0, hist_lt100us=0,
                  hist_lt1ms=0, hist_lt10ms=0, hist_lt100ms=0,
                  hist_lt1s=0, hist_ge1s=0, counter_overflow=0,
                  cumulative=1, inclusive=1, thread_time_sum=1,
                  census_complete=0, diagnostic_only=1)
    fields.update(extra)
    return "LUMEN_CRITICAL_PATH_TOTAL_V1 " + " ".join(f"{key}={value}" for key, value in fields.items())


def analyze(lines: list[str], start: int = 0, end: int = 1000,
            pid: int | None = None) -> dict:
    return report.build_report(report.parse_lines(lines), start, end, pid)


class FramePathTests(unittest.TestCase):
    def test_p99_uses_nearest_rank_and_empty_intervals_remain_unknown(self):
        self.assertEqual(report.interval_statistics(list(range(1, 102)))["p99_ns"], 100)
        self.assertEqual(report.interval_statistics([7])["p99_ns"], 7)
        self.assertIsNone(report.interval_statistics([])["p99_ns"])

    def test_raw_intervals_and_percentiles_use_accepted_exact_events(self):
        result = analyze([complete(1, "accepted_present", 100, frame=1),
                          complete(2, "accepted_present", 110, frame=2),
                          complete(3, "accepted_present", 130, frame=3),
                          complete(4, "guest_flip", 135, frame=0)])
        frames = result["accepted_presents"]
        self.assertEqual(frames["observed_count"], 3)
        self.assertEqual(frames["interval_statistics"]["mean_ns"], 15)
        self.assertEqual(frames["interval_statistics"]["p50_ns"], 15)
        self.assertEqual(frames["interval_statistics"]["p95_ns"], 20)
        self.assertFalse(frames["distinct_images_verified"])
        self.assertFalse(result["coverage"]["critical_path_complete"])

    def test_missing_frame_and_legacy_hint_do_not_invent_intervals(self):
        result = analyze([complete(1, "accepted_present", 100, frame=1, hint=1),
                          complete(2, "accepted_present", 110, frame=3, hint=2),
                          complete(3, "accepted_present", 130, hint=3)])
        self.assertEqual(result["accepted_presents"]["interval_statistics"]["count"], 0)
        self.assertIsNone(result["accepted_presents"]["interval_statistics"]["p95_ns"])
        self.assertEqual(len(result["accepted_presents"]["gaps"]), 2)

    def test_half_open_window_does_not_use_boundary_present_interval(self):
        result = analyze([complete(i, "accepted_present", t, frame=i)
                          for i, t in enumerate([90, 100, 110, 120], 1)], 100, 120)
        self.assertEqual(result["accepted_presents"]["observed_count"], 2)
        self.assertEqual(result["accepted_presents"]["interval_statistics"]["count"], 1)

    def test_straddling_span_keeps_full_duration_but_does_not_allocate_cpu(self):
        result = analyze([complete(1, "hle_call", 50, 250, kind="span", object=42, cpu=100),
                          complete(2, "hle_call", 150, 180, kind="span", object=42, cpu=10)], 100, 200)
        metric = result["detail_spans"]["by_reason"]["hle_call"]
        self.assertEqual(metric["overlap_inclusive_wall_ns_sum"], 130)
        self.assertEqual(metric["fully_contained_inclusive_wall_ns_sum"], 30)
        self.assertEqual(metric["fully_contained_thread_cpu_ns_sum"], 10)
        self.assertIsNone(metric["straddling_thread_cpu_ns"])
        self.assertEqual(metric["longest_observed_full_wall_ns"], 200)
        self.assertEqual(result["detail_spans"]["hle_targets"][0]["target"], 42)

    def test_nested_and_concurrent_thread_sums_are_not_frame_percentages(self):
        result = analyze([complete(1, "gpu_command_run", 10, 100, kind="span", cpu=60),
                          complete(2, "gpu_fence_wait", 20, 80, kind="span", cpu=5),
                          complete(1, "gpu_command_run", 10, 100, kind="span", tid=8, cpu=50)])
        self.assertEqual(result["detail_spans"]["by_reason"]["gpu_command_run"]["fully_contained_inclusive_wall_ns_sum"], 180)
        self.assertTrue(result["detail_spans"]["by_reason"]["gpu_command_run"]["inclusive_thread_sums_not_frame_budget"])
        self.assertFalse(result["coverage"]["inclusive_thread_sums_are_frame_percentages"])
        self.assertNotIn('"frame_percent":', json.dumps(result))

    def test_frame_hint_and_untyped_objects_do_not_create_edges(self):
        result = analyze([complete(1, "accepted_present", 100, frame=1, hint=9),
                          complete(2, "hle_call", 90, 105, kind="span", object=55, dependency=55, hint=9),
                          complete(3, "gpu_ce_progress", 95, object=55, hint=9)])
        self.assertEqual(result["explicit_gpu_dependency_edges"], [])
        self.assertFalse(result["coverage"]["frame_hint_used_for_causal_join"])

    def test_explicit_submission_wait_and_successful_fence_edges(self):
        result = analyze([complete(1, "gpu_submission_accepted", 50, object=99),
                          complete(2, "gpu_fence_wait", 110, 200, kind="span", object=1, dependency=99),
                          complete(3, "gpu_fence_completed", 201, object=99)], 100, 300)
        edges = result["explicit_gpu_dependency_edges"]
        self.assertEqual(len(edges), 2)
        self.assertEqual(edges[0]["recording_id"], 99)
        self.assertTrue(edges[0]["producer_outside_window"])
        self.assertTrue(edges[0]["submission_acceptance_is_not_gpu_completion"])

    def test_missing_and_ambiguous_producers_are_not_edges(self):
        result = analyze([complete(1, "gpu_submission_accepted", 10, object=9),
                          complete(2, "gpu_submission_accepted", 20, object=9),
                          complete(3, "gpu_fence_wait", 30, 40, kind="span", dependency=9),
                          complete(4, "gpu_fence_completed", 50, object=77)])
        self.assertEqual(result["explicit_gpu_dependency_edges"], [])
        self.assertEqual([r["reason"] for r in result["unresolved_explicit_gpu_dependencies"]],
                         ["ambiguous_producer_identity", "missing_producer"])

    def test_present_edges_use_explicit_blit_identity_across_threads_and_window(self):
        result = analyze([complete(1, "gpu_submission_accepted", 50, object=99, tid=8),
                          complete(1, "vulkan_present", 110, 200, kind="span", dependency=99),
                          complete(2, "accepted_present", 201, dependency=99, frame=1)], 100, 300)
        edges = result["explicit_gpu_dependency_edges"]
        self.assertEqual([r["relation"] for r in edges],
                         ["submission_to_explicit_present_wait", "submission_to_accepted_present"])
        for edge in edges:
            self.assertEqual(edge["producer"]["tid"], 8)
            self.assertTrue(edge["producer_outside_window"])
            self.assertTrue(edge["host_accepted_present_is_not_gpu_completion"])
            self.assertFalse(edge["distinct_guest_images_verified"])
            self.assertFalse(edge["preceding_guest_gpu_hle_dependencies_verified"])
        self.assertFalse(result["coverage"]["critical_path_complete"])
        self.assertFalse(result["coverage"]["inclusive_thread_sums_are_frame_percentages"])

    def test_present_missing_and_ambiguous_producers_remain_unresolved(self):
        for reason, kind, end, relation in [("vulkan_present", "span", 40, "submission_to_explicit_present_wait"),
                                             ("accepted_present", "event", 30, "submission_to_accepted_present")]:
            with self.subTest(reason=reason):
                result = analyze([complete(1, "gpu_submission_accepted", 10, object=9),
                                  complete(2, "gpu_submission_accepted", 20, object=9),
                                  complete(3, reason, 30, end, kind=kind, dependency=9),
                                  complete(4, reason, 50, 60 if kind == "span" else 50,
                                           kind=kind, dependency=77)])
                self.assertEqual(result["explicit_gpu_dependency_edges"], [])
                unresolved = result["unresolved_explicit_gpu_dependencies"]
                self.assertEqual([r["reason"] for r in unresolved],
                                 ["ambiguous_producer_identity", "missing_producer"])
                self.assertEqual([r["relation"] for r in unresolved], [relation, relation])

    def test_present_wait_rejects_submission_after_wait_started(self):
        result = analyze([complete(1, "vulkan_present", 100, 200, kind="span", dependency=9),
                          complete(2, "gpu_submission_accepted", 150, object=9)])
        self.assertEqual(result["explicit_gpu_dependency_edges"], [])
        self.assertEqual(result["unresolved_explicit_gpu_dependencies"][0]["reason"], "missing_producer")
        self.assertEqual(result["accepted_presents"]["observed_count"], 0)

    def test_accepted_present_rejects_submission_after_acceptance(self):
        result = analyze([complete(1, "accepted_present", 100, dependency=9, frame=1),
                          complete(2, "gpu_submission_accepted", 150, object=9)])
        self.assertEqual(result["explicit_gpu_dependency_edges"], [])
        self.assertEqual(result["unresolved_explicit_gpu_dependencies"][0]["reason"], "missing_producer")
        self.assertEqual(result["accepted_presents"]["observed_count"], 1)

    def test_present_deadline_equality_is_valid_without_guest_frame_hint_join(self):
        result = analyze([complete(1, "gpu_submission_accepted", 100, object=9, hint=77),
                          complete(2, "vulkan_present", 100, 200, kind="span", dependency=9, hint=88),
                          complete(3, "accepted_present", 100, dependency=9, hint=99, frame=1),
                          complete(4, "hle_call", 100, 200, kind="span", dependency=9, hint=99)])
        self.assertEqual(len(result["explicit_gpu_dependency_edges"]), 2)
        self.assertFalse(result["coverage"]["frame_hint_used_for_causal_join"])
        self.assertNotIn('"frame_percent":', json.dumps(result))

    def test_present_span_never_invents_an_accepted_event_or_frame_interval(self):
        result = analyze([complete(1, "gpu_submission_accepted", 90, object=9),
                          complete(2, "accepted_present", 100, dependency=9, frame=1),
                          complete(3, "vulkan_present", 110, 120, kind="span", dependency=9),
                          complete(4, "accepted_present", 130, dependency=9, frame=3)])
        self.assertEqual(result["accepted_presents"]["observed_count"], 2)
        self.assertEqual(result["accepted_presents"]["interval_statistics"]["count"], 0)
        self.assertEqual(len(result["accepted_presents"]["gaps"]), 1)

    def test_cumulative_deltas_use_actual_snapshot_bounds_not_window_max(self):
        result = analyze([summary(1, 100), total(1, 10, 1000, 100, 500),
                          total(1, 10, 1000, 100, 500, kind="hle_target", target=42),
                          summary(2, 200), total(2, 15, 1200, 150, 500),
                          total(2, 15, 1200, 150, 500, kind="hle_target", target=42)], 150, 250)
        thread = result["cumulative_thread_snapshots"][0]
        self.assertFalse(thread["requested_window_exact"])
        self.assertEqual(len(thread["deltas"]), 2)
        delta = thread["deltas"][0]
        self.assertEqual(delta["calls"], 5)
        self.assertEqual(delta["thread_wall_ns"], 200)
        self.assertEqual((delta["start_ns"], delta["end_ns"]), (100, 200))
        self.assertIsNone(delta["delta_longest_wall_ns"])
        self.assertEqual(delta["end_cumulative_max_wall_ns"], 500)

    def test_missing_start_snapshot_and_capture_inflight_remain_unknown(self):
        result = analyze([summary(1, 150), total(1, 10, 1000, 100, 500)], 100, 200)
        self.assertFalse(result["cumulative_thread_snapshots"][0]["delta_available"])
        self.assertTrue(result["coverage"]["inflight_scopes_at_capture_end_unknown"])

    def test_loss_counters_are_max_perthread_not_sum_of_snapshots(self):
        result = analyze([summary(1, 100, dropped_spans=3, dropped_rate=3),
                          summary(2, 200, dropped_spans=4, dropped_rate=4, reports_suppressed=2),
                          summary(3, 400, dropped_spans=100)], 100, 300)
        counters = result["loss_counters_by_thread"][0]
        self.assertEqual(counters["dropped_spans"], 4)
        self.assertEqual(counters["reports_suppressed"], 2)
        self.assertTrue(result["coverage"]["any_reported_loss_before_window_end"])

    def test_total_overflow_is_loss_and_unpaired_totals_do_not_get_fake_bounds(self):
        result = analyze([summary(1, 100), total(1, 10, 1000, 100, 500, counter_overflow=2),
                          total(2, 12, 1200, 110, 500, counter_overflow=3)], 50, 150)
        self.assertEqual(result["loss_counters_by_thread"][0]["counter_overflow"], 2)
        self.assertEqual(len(result["cumulative_thread_snapshots"][0]["end_cumulative_totals"]), 1)
        self.assertFalse(result["cumulative_thread_snapshots"][0]["snapshot_total_completeness_verified"])
        self.assertEqual(len(result["unpaired_cumulative_totals_without_timestamp"]), 1)

    def test_emitted_sequence_gap_exposes_log_loss_without_inventing_missing_scopes(self):
        result = analyze([complete(4, "hle_call", 100, 150, kind="span"),
                          complete(7, "hle_call", 200, 250, kind="span")])
        sequence = result["emitted_record_sequence_coverage"][0]
        self.assertTrue(sequence["leading_emitted_history_not_captured"])
        self.assertEqual(sequence["missing_emitted_record_ranges"][0]["missing_records"], 2)
        self.assertEqual(result["detail_spans"]["by_reason"]["hle_call"]["completed_records_observed"], 2)

    def test_pipeline_mean_never_supplies_quantiles_or_monotonic_window(self):
        result = analyze(["[FRAME_PIPELINE] present=120 avg_interval=250.000 ms avg_latency=500.000 ms pending=0 index=0 arg=0"])
        row = result["frame_pipeline_unattributed_to_monotonic_window"][0]
        self.assertEqual(row["fps_from_mean_interval"], 4)
        self.assertIsNone(row["p50_ns"])
        self.assertEqual(result["accepted_presents"]["interval_statistics"]["count"], 0)

    def test_duplicate_logcat_records_are_deduplicated_conflicts_fail(self):
        line = complete(1, "accepted_present", 100, frame=1)
        result = analyze([line, line])
        self.assertEqual(result["duplicate_records_ignored"], 1)
        self.assertEqual(result["accepted_presents"]["observed_count"], 1)
        with self.assertRaises(report.ReportError):
            analyze([line, complete(1, "accepted_present", 101, frame=1)])

    def test_bad_duration_and_counter_regression_fail_closed(self):
        line = complete(1, "hle_call", 10, 20, kind="span").replace("wall_ns=10", "wall_ns=11")
        with self.assertRaises(report.ReportError):
            analyze([line])
        with self.assertRaises(report.ReportError):
            analyze([summary(1, 100), total(1, 10, 1000, 100, 500),
                     summary(2, 200), total(2, 9, 1000, 100, 500)], 100, 200)

    def test_process_identity_is_required_and_never_cross_joins(self):
        lines = [complete(1, "gpu_submission_accepted", 10, object=9),
                 complete(1, "gpu_fence_completed", 20, object=9, pid=11)]
        with self.assertRaises(report.ReportError):
            analyze(lines)
        result = analyze(lines, pid=11)
        self.assertEqual(result["explicit_gpu_dependency_edges"], [])

    def test_cli_writes_json_offline(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory)
            (path / "trace.log").write_text(complete(1, "accepted_present", 100, frame=1) + "\n")
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(report.main([str(path / "trace.log"), "--start-ns", "0", "--end-ns", "200"]), 0)
            self.assertEqual(json.loads(output.getvalue())["accepted_presents"]["observed_count"], 1)

    def test_large_span_set_partitions_without_quadratic_record_membership(self):
        class CountedRecord(dict):
            comparisons = 0

            def __eq__(self, other):
                CountedRecord.comparisons += 1
                return super().__eq__(other)

        records = [CountedRecord(pid=10, tid=7, sequence=i + 1, start_ns=100,
                                 end_ns=200 if i % 5 else 300,
                                 wall_ns=100 if i % 5 else 200, thread_cpu_ns=50)
                   for i in range(20000)]
        result = report.span_metrics(records, 100, 250)
        self.assertEqual(result["fully_contained_count"], 16000)
        self.assertEqual(result["window_straddling_count"], 4000)
        self.assertEqual(result["fully_contained_inclusive_wall_ns_sum"], 1600000)
        self.assertEqual(result["overlap_inclusive_wall_ns_sum"], 2200000)
        self.assertEqual(CountedRecord.comparisons, 0)


if __name__ == "__main__":
    unittest.main()
