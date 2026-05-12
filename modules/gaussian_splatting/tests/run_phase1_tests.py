#!/usr/bin/env python3
"""
Phase 1 Test Runner for Gaussian Splatting Module
Automates building, testing, benchmarking, and reporting.
"""

import os
import sys
import subprocess
import json
import time
import argparse
import platform
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Tuple, Optional
import multiprocessing


class TestRunner:
    """Main test runner for Phase 1 tests."""

    def __init__(self, godot_path: str, project_path: str):
        self.godot_path = Path(godot_path)
        self.project_path = Path(project_path)
        self.module_path = self.project_path / "modules" / "gaussian_splatting"
        self.test_output_dir = self.module_path / "test_results"
        self.test_output_dir.mkdir(exist_ok=True)

        self.results = {
            "timestamp": datetime.now().isoformat(),
            "platform": platform.system(),
            "cpu": platform.processor(),
            "cpu_count": multiprocessing.cpu_count(),
            "tests": {}
        }

    def run_command(
        self,
        cmd: List[str],
        cwd: Optional[Path] = None,
        env: Optional[Dict[str, str]] = None,
    ) -> Tuple[int, str, str]:
        """Run a command and return exit code, stdout, and stderr."""
        print(f"Running: {' '.join(cmd)}")

        process = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            cwd=cwd or self.project_path,
            env=env,
            universal_newlines=True
        )

        stdout, stderr = process.communicate()
        return process.returncode, stdout, stderr

    def build_module(self, config: str = "debug") -> bool:
        """Build the Gaussian Splatting module."""
        print("\n=== Building Gaussian Splatting Module ===")

        scons_cmd = ["scons"]

        # Platform-specific build options
        if platform.system() == "Windows":
            scons_cmd.extend(["platform=windows", "tools=yes"])
        elif platform.system() == "Linux":
            scons_cmd.extend(["platform=linux", "tools=yes"])
        elif platform.system() == "Darwin":
            scons_cmd.extend(["platform=macos", "tools=yes"])

        # Build configuration
        if config == "debug":
            scons_cmd.extend(["debug_symbols=yes", "optimize=debug"])
        elif config == "release":
            scons_cmd.extend(["optimize=speed"])
        elif config == "profile":
            scons_cmd.extend(["debug_symbols=yes", "optimize=speed"])

        # Parallel build
        scons_cmd.append(f"-j{multiprocessing.cpu_count()}")

        start_time = time.time()
        exit_code, stdout, stderr = self.run_command(scons_cmd, self.godot_path)
        build_time = time.time() - start_time

        self.results["build"] = {
            "success": exit_code == 0,
            "time_seconds": build_time,
            "config": config
        }

        if exit_code != 0:
            print(f"Build failed:\n{stderr}")
            return False

        print(f"Build completed in {build_time:.2f} seconds")
        return True

    def run_unit_tests(self) -> bool:
        """Run unit tests using Godot's test framework."""
        print("\n=== Running Unit Tests ===")

        test_categories = [
            "test_gpu_streaming",
            "test_gpu_sorting",
            "test_phase1_integration"
        ]

        all_passed = True
        unit_test_results = {}

        for test_category in test_categories:
            print(f"\nRunning {test_category}...")

            cmd = [
                str(self.godot_path / "bin" / self._get_godot_binary()),
                "--test",
                f"--test-suite={test_category}",
                "--headless"
            ]

            start_time = time.time()
            exit_code, stdout, stderr = self.run_command(cmd)
            test_time = time.time() - start_time

            # Parse test output
            passed = 0
            failed = 0
            for line in stdout.split('\n'):
                if "PASSED" in line:
                    passed += 1
                elif "FAILED" in line:
                    failed += 1

            test_passed = exit_code == 0 and failed == 0
            all_passed = all_passed and test_passed

            unit_test_results[test_category] = {
                "passed": test_passed,
                "tests_passed": passed,
                "tests_failed": failed,
                "time_seconds": test_time
            }

            print(f"  Result: {'PASSED' if test_passed else 'FAILED'}")
            print(f"  Tests: {passed} passed, {failed} failed")
            print(f"  Time: {test_time:.2f}s")

        self.results["tests"]["unit_tests"] = unit_test_results
        return all_passed

    def run_integration_tests(self) -> bool:
        """Run integration tests."""
        print("\n=== Running Integration Tests ===")

        integration_script = """
extends Node

func _ready():
    var tests = preload("res://modules/gaussian_splatting/tests/test_phase1_integration.cpp")
    var runner = tests.new()
    runner.run_all_tests()
    get_tree().quit(0 if runner.all_passed() else 1)
"""

        # Write temporary test script
        test_script_path = self.test_output_dir / "integration_test.gd"
        test_script_path.write_text(integration_script)

        cmd = [
            str(self.godot_path / "bin" / self._get_godot_binary()),
            "--script",
            str(test_script_path),
            "--headless"
        ]

        start_time = time.time()
        exit_code, stdout, stderr = self.run_command(cmd)
        test_time = time.time() - start_time

        self.results["tests"]["integration"] = {
            "passed": exit_code == 0,
            "time_seconds": test_time,
            "output": stdout
        }

        print(f"Integration tests: {'PASSED' if exit_code == 0 else 'FAILED'}")
        print(f"Time: {test_time:.2f}s")

        return exit_code == 0

    def run_performance_benchmarks(self) -> bool:
        """Run performance benchmarks.

        The legacy C++ ``PerformanceBenchmark`` harness was removed (issue #289)
        because it never linked: ``GaussianMemoryStream::get_sort_keys_buffer``
        was declared but never defined, and the previous loader tried to
        ``preload`` a header file as if it were a script. Delegate to the live
        integration benchmark runner so this phase still executes real workload.
        """
        print("\n=== Performance Benchmarks ===")

        integration_runner = self.module_path / "tests" / "run_integration_tests.py"
        if not integration_runner.exists():
            self.results["tests"]["benchmarks"] = []
            self.results["tests"]["benchmarks_runner"] = {
                "passed": False,
                "error": f"Benchmark runner missing: {integration_runner}",
            }
            print(f"Benchmark runner missing: {integration_runner}")
            return False

        reports_dir = self.module_path / "tests"
        reports_before = {
            path: path.stat().st_mtime_ns
            for path in reports_dir.glob("integration_report_*.json")
        }
        env = os.environ.copy()
        godot_binary = self.godot_path / "bin" / self._get_godot_binary()
        if godot_binary.exists():
            env["GODOT_BINARY"] = str(godot_binary)

        cmd = [sys.executable, str(integration_runner), "--benchmarks-only"]
        start_time = time.time()
        exit_code, stdout, stderr = self.run_command(cmd, self.project_path, env=env)
        benchmark_time = time.time() - start_time

        latest_report = None
        candidate_reports = [
            path
            for path in reports_dir.glob("integration_report_*.json")
            if path not in reports_before or path.stat().st_mtime_ns > reports_before[path]
        ]
        if candidate_reports:
            latest_report = max(candidate_reports, key=lambda path: path.stat().st_mtime)

        benchmark_results = []
        report_error = None
        try:
            if latest_report is None:
                raise FileNotFoundError("No integration benchmark report generated")
            with open(latest_report, "r") as f:
                integration_results = json.load(f)

            performance_suite = integration_results.get("test_suites", {}).get("performance", {})
            for benchmark in performance_suite.get("benchmarks", []):
                benchmark_results.append(self._normalize_benchmark_result(benchmark))
        except (OSError, json.JSONDecodeError, KeyError) as exc:
            report_error = str(exc)
            print(f"Failed to read benchmark report: {report_error}")

        self.results["tests"]["benchmarks"] = benchmark_results
        self.results["tests"]["benchmarks_runner"] = {
            "passed": exit_code == 0 and report_error is None,
            "time_seconds": benchmark_time,
            "report": str(latest_report) if latest_report else None,
            "stdout_tail": stdout.splitlines()[-20:],
            "stderr_tail": stderr.splitlines()[-20:] if stderr else [],
            "error": report_error,
        }

        if exit_code != 0:
            print("Benchmark runner failed")
            if stderr:
                print(stderr.strip())
            return False

        if report_error:
            return False

        if not benchmark_results:
            print("Benchmark runner produced no benchmark results")
            return False

        failed_benchmarks = [b for b in benchmark_results if not b.get("passed")]
        if failed_benchmarks:
            print("Performance benchmarks failed:")
            for benchmark in failed_benchmarks:
                print(f"  {benchmark.get('name', 'unknown')}: {benchmark.get('error', 'Unknown error')}")
            return False

        print(f"Performance benchmarks: PASSED ({len(benchmark_results)} configurations)")
        return True

    def _normalize_benchmark_result(self, benchmark: dict) -> dict:
        """Convert integration-runner benchmark output into phase1 result schema."""
        config = benchmark.get("config", {})
        metrics = benchmark.get("metrics", {})
        requested_count = config.get("count", 0)
        measured_count = metrics.get("total_splats")
        output_matches_config = measured_count == requested_count
        meets_absolute_target, target_error = self._benchmark_meets_absolute_targets(
            requested_count, metrics)
        error = benchmark.get("error")

        if benchmark.get("completed", False) and not output_matches_config:
            error = (
                f"Benchmark output mismatch: expected {requested_count} splats, "
                f"got {measured_count}"
            )
        elif benchmark.get("completed", False) and not meets_absolute_target:
            error = target_error

        return {
            "name": benchmark.get("name"),
            "passed": benchmark.get("completed", False) and output_matches_config and meets_absolute_target,
            "error": error,
            "config": {
                "splat_count": requested_count,
                "frame_count": config.get("frames", 0),
            },
            "metrics": metrics,
        }

    def _benchmark_meets_absolute_targets(self, splat_count: int, metrics: dict) -> Tuple[bool, Optional[str]]:
        """Enforce a floor when no baseline is available for small benchmark configs."""
        if splat_count > 100000:
            return True, None

        avg_fps = metrics.get("avg_fps", 0)
        if avg_fps:
            if avg_fps >= 60:
                return True, None
            return False, f"{splat_count} splats only achieved {avg_fps:.1f} FPS; target is 60 FPS"

        populate_ms = metrics.get("populate_time_ms")
        octree_ms = metrics.get("octree_build_time_ms")
        if populate_ms is None or octree_ms is None:
            return False, "Benchmark result lacks FPS or setup timing metrics for absolute target check"

        setup_ms = populate_ms + octree_ms
        if setup_ms <= 30000:
            return True, None
        return False, f"{splat_count} splat setup took {setup_ms:.1f}ms; target is <= 30000ms"

    def run_memory_validation(self) -> bool:
        """Run memory leak detection tests."""
        print("\n=== Running Memory Validation ===")

        memory_script = """
extends Node

func _ready():
    var validator = preload("res://modules/gaussian_splatting/tests/memory_validator.h")
    var mem_validator = validator.MemoryValidator.new()

    # Run memory tests
    mem_validator.stress_test_allocation_patterns(1000)
    mem_validator.stress_test_fragmentation(100)

    # Check for leaks
    var has_leaks = not mem_validator.validate_no_leaks()

    # Generate report
    var report = mem_validator.get_report_dict()
    print(JSON.stringify(report))

    get_tree().quit(1 if has_leaks else 0)
"""

        # Write temporary memory test script
        mem_script_path = self.test_output_dir / "memory_test.gd"
        mem_script_path.write_text(memory_script)

        cmd = [
            str(self.godot_path / "bin" / self._get_godot_binary()),
            "--script",
            str(mem_script_path),
            "--headless"
        ]

        start_time = time.time()
        exit_code, stdout, stderr = self.run_command(cmd)
        test_time = time.time() - start_time

        # Parse memory report
        memory_report = {}
        try:
            json_start = stdout.find('{')
            json_end = stdout.rfind('}') + 1
            if json_start >= 0 and json_end > json_start:
                report_json = stdout[json_start:json_end]
                memory_report = json.loads(report_json)
        except json.JSONDecodeError:
            print("Failed to parse memory report")

        self.results["tests"]["memory"] = {
            "passed": exit_code == 0,
            "time_seconds": test_time,
            "report": memory_report
        }

        if exit_code == 0:
            print("Memory validation: PASSED (no leaks detected)")
        else:
            print("Memory validation: FAILED (leaks detected)")
            if memory_report:
                leaked = memory_report.get('stats', {}).get('leaked_allocations', 0)
                leaked_bytes = memory_report.get('stats', {}).get('leaked_bytes', 0)
                print(f"  Leaked allocations: {leaked}")
                print(f"  Leaked bytes: {leaked_bytes}")

        return exit_code == 0

    def run_visual_validation(self) -> bool:
        """Run visual validation tests."""
        print("\n=== Running Visual Validation ===")

        # For now, just check that visual validation compiles
        # Actual visual tests would require a display

        self.results["tests"]["visual"] = {
            "skipped": True,
            "reason": "Visual tests require display"
        }

        print("Visual validation: SKIPPED (headless mode)")
        return True

    def run_synthetic_baseline_validation(self) -> bool:
        """Validate deterministic synthetic splat baseline artifacts."""
        print("\n=== Running Synthetic Baseline Validation ===")

        script_path = self.module_path / "tests" / "generate_synthetic_splat_baselines.py"
        if not script_path.exists():
            self.results["tests"]["synthetic_baselines"] = {
                "passed": False,
                "time_seconds": 0.0,
                "error": f"Baseline generator script missing: {script_path}"
            }
            print("Synthetic baseline validation: FAILED (script missing)")
            return False

        python_cmd = [sys.executable, str(script_path), "--check"]
        python_start_time = time.time()
        python_exit_code, python_stdout, python_stderr = self.run_command(python_cmd, self.project_path)
        python_test_time = time.time() - python_start_time

        cpp_cmd = [
            str(self.godot_path / "bin" / self._get_godot_binary()),
            "--test",
            "--test-case=[GaussianSplatting][Synthetic]*",
            "--headless",
        ]
        cpp_start_time = time.time()
        cpp_exit_code, cpp_stdout, cpp_stderr = self.run_command(cpp_cmd)
        cpp_test_time = time.time() - cpp_start_time

        python_passed = python_exit_code == 0
        cpp_passed = cpp_exit_code == 0
        validation_passed = python_passed and cpp_passed

        self.results["tests"]["synthetic_baselines"] = {
            "passed": validation_passed,
            "time_seconds": python_test_time + cpp_test_time,
            "python_check": {
                "passed": python_passed,
                "time_seconds": python_test_time,
                "stdout_tail": python_stdout.splitlines()[-20:],
                "stderr_tail": python_stderr.splitlines()[-20:] if python_stderr else [],
            },
            "cpp_generator_check": {
                "passed": cpp_passed,
                "time_seconds": cpp_test_time,
                "suite": "test_synthetic_splat_generators",
                "stdout_tail": cpp_stdout.splitlines()[-20:],
                "stderr_tail": cpp_stderr.splitlines()[-20:] if cpp_stderr else [],
            },
        }

        if validation_passed:
            print("Synthetic baseline validation: PASSED")
            return True

        print("Synthetic baseline validation: FAILED")
        if not python_passed and python_stderr:
            print(python_stderr.strip())
        if not cpp_passed and cpp_stderr:
            print(cpp_stderr.strip())
        return False

    def check_regression(self) -> bool:
        """Check for performance regressions against baseline."""
        print("\n=== Checking for Regressions ===")

        baseline_file = self.test_output_dir / "baseline.json"

        if not baseline_file.exists():
            print("No baseline found, skipping regression check")
            self.results["regression"] = {"skipped": True}
            return True

        with open(baseline_file, 'r') as f:
            baseline = json.load(f)

        regressions, incompatible = self._collect_benchmark_regressions(baseline)

        self.results["regression"] = {
            "has_regression": len(regressions) > 0,
            "regressions": regressions,
            "incompatible_benchmarks": incompatible,
        }

        if regressions:
            print("Performance regressions detected:")
            for reg in regressions:
                print(f"  {reg['test']} {reg['metric']}: {reg['regression_percent']:.1f}% slower")
            return False
        else:
            for item in incompatible:
                print(f"  Skipping incompatible benchmark baseline: {item['test']} ({item['reason']})")
            print("No regressions detected")
            return True

    def _collect_benchmark_regressions(self, baseline: dict) -> Tuple[List[dict], List[dict]]:
        """Compare benchmark results against baseline benchmark entries."""
        current_benchmarks = self.results.get("tests", {}).get("benchmarks", [])
        baseline_benchmarks = baseline.get("tests", {}).get("benchmarks", [])
        regressions = []
        incompatible = []

        for current in current_benchmarks:
            splat_count = current.get("config", {}).get("splat_count", 0)
            base = self._find_baseline_benchmark(baseline_benchmarks, splat_count)
            if base is not None:
                result = self._compare_benchmark_metrics(current, base, splat_count)
                regressions.extend(result[0])
                incompatible.extend(result[1])

        return regressions, incompatible

    def _find_baseline_benchmark(self, benchmarks: List[dict], splat_count: int) -> Optional[dict]:
        """Find the baseline entry matching a benchmark splat count."""
        for benchmark in benchmarks:
            if benchmark.get("config", {}).get("splat_count") == splat_count:
                return benchmark
        return None

    def _compare_benchmark_metrics(self, current: dict, base: dict, splat_count: int) -> Tuple[List[dict], List[dict]]:
        """Return regression records and incompatible schema notices."""
        regressions = []
        incompatible = []
        comparable_metrics = False
        current_metrics = current.get("metrics", {})
        base_metrics = base.get("metrics", {})

        comparable_metrics |= self._append_fps_regression(
            regressions, current_metrics, base_metrics, splat_count)

        for metric_name in ("populate_time_ms", "octree_build_time_ms"):
            comparable_metrics |= self._append_timing_regression(
                regressions, current_metrics, base_metrics, splat_count, metric_name)

        if not comparable_metrics:
            incompatible.append({
                "test": f"benchmark_{splat_count}",
                "metric": "benchmark_metrics",
                "baseline": "present",
                "current": "not comparable",
                "reason": "No shared benchmark metrics between current result and baseline",
            })

        return regressions, incompatible

    def _append_fps_regression(
        self,
        regressions: List[dict],
        current_metrics: dict,
        base_metrics: dict,
        splat_count: int,
    ) -> bool:
        """Append an FPS regression if both benchmark entries are comparable."""
        current_fps = current_metrics.get("avg_fps", 0)
        base_fps = base_metrics.get("avg_fps", 0)
        if base_fps <= 0 or current_fps <= 0:
            return False

        regression = (base_fps - current_fps) / base_fps
        if regression > 0.1:
            regressions.append({
                "test": f"benchmark_{splat_count}",
                "metric": "avg_fps",
                "baseline": base_fps,
                "current": current_fps,
                "regression_percent": regression * 100
            })
        return True

    def _append_timing_regression(
        self,
        regressions: List[dict],
        current_metrics: dict,
        base_metrics: dict,
        splat_count: int,
        metric_name: str,
    ) -> bool:
        """Append a timing regression if both benchmark entries are comparable."""
        current_value = current_metrics.get(metric_name, 0)
        base_value = base_metrics.get(metric_name, 0)
        if base_value <= 0 or current_value <= 0:
            return False

        regression = (current_value - base_value) / base_value
        if regression > 0.1:
            regressions.append({
                "test": f"benchmark_{splat_count}",
                "metric": metric_name,
                "baseline": base_value,
                "current": current_value,
                "regression_percent": regression * 100
            })
        return True

    def generate_report(self):
        """Generate test report in multiple formats."""
        print("\n=== Generating Test Report ===")

        # JSON report
        json_report_path = self.test_output_dir / f"test_report_{datetime.now().strftime('%Y%m%d_%H%M%S')}.json"
        with open(json_report_path, 'w') as f:
            json.dump(self.results, f, indent=2)
        print(f"JSON report: {json_report_path}")

        # Markdown report
        md_report = self._generate_markdown_report()
        md_report_path = self.test_output_dir / f"test_report_{datetime.now().strftime('%Y%m%d_%H%M%S')}.md"
        md_report_path.write_text(md_report)
        print(f"Markdown report: {md_report_path}")

        # Console summary
        self._print_summary()

    def _generate_markdown_report(self) -> str:
        """Generate markdown formatted report."""
        report = []
        report.append("# Phase 1 Test Report\n")
        report.append(f"**Date**: {self.results['timestamp']}\n")
        report.append(f"**Platform**: {self.results['platform']}\n")
        report.append(f"**CPU**: {self.results['cpu']} ({self.results['cpu_count']} cores)\n")

        self._append_markdown_build_results(report)
        self._append_markdown_test_results(report)
        self._append_markdown_regression(report)

        return ''.join(report)

    def _append_markdown_build_results(self, report: List[str]) -> None:
        """Append build results to the markdown report."""
        if "build" in self.results:
            report.append("\n## Build Results\n")
            build = self.results["build"]
            report.append(f"- **Status**: {'✅ PASSED' if build['success'] else '❌ FAILED'}\n")
            report.append(f"- **Configuration**: {build['config']}\n")
            report.append(f"- **Time**: {build['time_seconds']:.2f} seconds\n")

    def _append_markdown_test_results(self, report: List[str]) -> None:
        """Append test result sections to the markdown report."""
        tests = self.results.get("tests")
        if not tests:
            return

        report.append("\n## Test Results\n")
        self._append_markdown_unit_tests(report, tests)
        self._append_markdown_integration(report, tests)
        self._append_markdown_synthetic_baselines(report, tests)
        self._append_markdown_benchmarks(report, tests)
        self._append_markdown_memory(report, tests)

    def _append_markdown_unit_tests(self, report: List[str], tests: dict) -> None:
        """Append unit test result rows to the markdown report."""
        unit_tests = tests.get("unit_tests")
        if not unit_tests:
            return

        report.append("\n### Unit Tests\n")
        for name, result in unit_tests.items():
            status = "✅" if result["passed"] else "❌"
            report.append(f"- **{name}**: {status} ")
            report.append(f"({result['tests_passed']} passed, {result['tests_failed']} failed, ")
            report.append(f"{result['time_seconds']:.2f}s)\n")

    def _append_markdown_integration(self, report: List[str], tests: dict) -> None:
        """Append integration test results to the markdown report."""
        result = tests.get("integration")
        if not result:
            return

        status = "✅ PASSED" if result["passed"] else "❌ FAILED"
        report.append("\n### Integration Tests\n")
        report.append(f"- **Status**: {status}\n")
        report.append(f"- **Time**: {result['time_seconds']:.2f} seconds\n")

    def _append_markdown_synthetic_baselines(self, report: List[str], tests: dict) -> None:
        """Append synthetic baseline validation results to the markdown report."""
        synthetic = tests.get("synthetic_baselines")
        if not synthetic:
            return

        status = "✅ PASSED" if synthetic.get("passed") else "❌ FAILED"
        report.append("\n### Synthetic Baseline Validation\n")
        report.append(f"- **Status**: {status}\n")
        report.append(f"- **Time**: {synthetic.get('time_seconds', 0.0):.2f} seconds\n")

    def _append_markdown_benchmarks(self, report: List[str], tests: dict) -> None:
        """Append benchmark table rows to the markdown report."""
        benchmarks = tests.get("benchmarks")
        if not benchmarks:
            return

        report.append("\n### Performance Benchmarks\n")
        report.append("\n| Splat Count | Avg FPS | Frame Time (ms) | GPU Memory (MB) |\n")
        report.append("|-------------|---------|-----------------|------------------|\n")

        for bench in benchmarks:
            config = bench.get("config", {})
            metrics = bench.get("metrics", {})
            report.append(f"| {config.get('splat_count', 0):,} | ")
            report.append(f"{metrics.get('avg_fps', 0):.1f} | ")
            report.append(f"{metrics.get('avg_frame_time_ms', 0):.2f} | ")
            report.append(f"{metrics.get('peak_gpu_memory_mb', 0):.1f} |\n")

    def _append_markdown_memory(self, report: List[str], tests: dict) -> None:
        """Append memory validation results to the markdown report."""
        mem = tests.get("memory")
        if not mem:
            return

        status = "✅ PASSED" if mem["passed"] else "❌ FAILED"
        report.append("\n### Memory Validation\n")
        report.append(f"- **Status**: {status}\n")
        self._append_markdown_memory_stats(report, mem.get("report"))

    def _append_markdown_memory_stats(self, report: List[str], memory_report: Optional[dict]) -> None:
        """Append memory statistics when the memory validation produced a report."""
        if not memory_report:
            return

        stats = memory_report.get("stats", {})
        report.append(f"- **Peak CPU Memory**: {stats.get('peak_cpu_bytes', 0) / (1024*1024):.2f} MB\n")
        report.append(f"- **Peak GPU Memory**: {stats.get('peak_gpu_bytes', 0) / (1024*1024):.2f} MB\n")

        if stats.get('leaked_allocations', 0) > 0:
            report.append(f"- **⚠️ Leaks**: {stats['leaked_allocations']} allocations ")
            report.append(f"({stats.get('leaked_bytes', 0) / 1024:.2f} KB)\n")

    def _append_markdown_regression(self, report: List[str]) -> None:
        """Append regression analysis to the markdown report."""
        if "regression" in self.results:
            report.append("\n## Regression Analysis\n")
            reg = self.results["regression"]

            if reg.get("skipped"):
                report.append("- Regression check skipped (no baseline)\n")
            elif reg.get("has_regression"):
                report.append("- **⚠️ Regressions detected**:\n")
                for r in reg.get("regressions", []):
                    report.append(f"  - {r['test']}: {r['regression_percent']:.1f}% slower\n")
            else:
                report.append("- ✅ No regressions detected\n")

    def _print_summary(self):
        """Print test summary to console."""
        print("\n" + "="*60)
        print("TEST SUMMARY")
        print("="*60)

        total_tests = 0
        passed_tests = 0

        # Count test results
        if "tests" in self.results:
            # Unit tests
            if "unit_tests" in self.results["tests"]:
                for result in self.results["tests"]["unit_tests"].values():
                    total_tests += 1
                    if result["passed"]:
                        passed_tests += 1

            # Other tests
            for test_type in ["integration", "memory", "synthetic_baselines"]:
                if test_type in self.results["tests"]:
                    total_tests += 1
                    if self.results["tests"][test_type].get("passed"):
                        passed_tests += 1

        print(f"Tests Passed: {passed_tests}/{total_tests}")

        # Performance summary
        if "benchmarks" in self.results.get("tests", {}):
            print("\nPerformance Summary:")
            for bench in self.results["tests"]["benchmarks"]:
                config = bench.get("config", {})
                metrics = bench.get("metrics", {})
                splats = config.get("splat_count", 0)
                fps = metrics.get("avg_fps", 0)

                status = "✅" if (splats <= 100000 and fps >= 60) or splats > 100000 else "❌"
                print(f"  {splats:,} splats: {fps:.1f} FPS {status}")

        # Overall result
        all_passed = passed_tests == total_tests and total_tests > 0

        if "regression" in self.results:
            if self.results["regression"].get("has_regression"):
                all_passed = False
                print("\n⚠️ Performance regressions detected!")

        print("\n" + "="*60)
        if all_passed:
            print("✅ ALL TESTS PASSED")
        else:
            print("❌ SOME TESTS FAILED")
        print("="*60)

    def save_baseline(self):
        """Save current results as baseline for future regression checks."""
        baseline_file = self.test_output_dir / "baseline.json"
        with open(baseline_file, 'w') as f:
            json.dump(self.results, f, indent=2)
        print(f"Baseline saved to: {baseline_file}")

    def _get_godot_binary(self) -> str:
        """Get the Godot binary name based on platform."""
        system = platform.system()
        if system == "Windows":
            return "godot.windows.editor.x86_64.exe"
        elif system == "Linux":
            candidates = [
                "godot.linuxbsd.editor.x86_64",
                "godot.linux.editor.x86_64",
            ]
            for candidate in candidates:
                if (self.godot_path / "bin" / candidate).exists():
                    return candidate
            return candidates[0]
        elif system == "Darwin":
            return "godot.macos.editor.universal"
        else:
            return "godot"

    def run_all(self, config: str = "debug") -> bool:
        """Run all tests in sequence."""
        print("="*60)
        print("PHASE 1 TEST RUNNER")
        print("="*60)

        all_passed = True

        # Build
        if not self.build_module(config):
            print("Build failed, aborting tests")
            return False

        # Run tests
        all_passed = all_passed and self.run_synthetic_baseline_validation()
        all_passed = all_passed and self.run_unit_tests()
        all_passed = all_passed and self.run_integration_tests()
        all_passed = all_passed and self.run_performance_benchmarks()
        all_passed = all_passed and self.run_memory_validation()
        all_passed = all_passed and self.run_visual_validation()

        # Check regression
        all_passed = all_passed and self.check_regression()

        # Generate report
        self.generate_report()

        return all_passed


def main():
    """Main entry point."""
    parser = argparse.ArgumentParser(description="Phase 1 Test Runner for Gaussian Splatting")
    parser.add_argument("--godot-path", required=True, help="Path to Godot source directory")
    parser.add_argument("--project-path", default=".", help="Path to project directory")
    parser.add_argument("--config", choices=["debug", "release", "profile"], default="debug",
                       help="Build configuration")
    parser.add_argument("--save-baseline", action="store_true",
                       help="Save results as baseline for regression tests")
    parser.add_argument("--skip-build", action="store_true",
                       help="Skip building and run tests only")
    parser.add_argument("--test-only", choices=["synthetic", "unit", "integration", "benchmark", "memory"],
                       help="Run only specific test category")

    args = parser.parse_args()

    # Create test runner
    runner = TestRunner(args.godot_path, args.project_path)

    # Run tests
    if args.test_only:
        # Run specific test category
        if args.test_only == "unit":
            success = runner.run_unit_tests()
        elif args.test_only == "synthetic":
            success = runner.run_synthetic_baseline_validation()
        elif args.test_only == "integration":
            success = runner.run_integration_tests()
        elif args.test_only == "benchmark":
            success = runner.run_performance_benchmarks()
        elif args.test_only == "memory":
            success = runner.run_memory_validation()

        runner.generate_report()
    else:
        # Run all tests
        success = runner.run_all(args.config)

    # Save baseline if requested
    if args.save_baseline and success:
        runner.save_baseline()

    # Exit with appropriate code
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
