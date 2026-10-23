"""Fast contract tests; subprocess fixtures never run the real benchmark."""
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).with_name('run-matrix.py')


class MatrixTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not SCRIPT.exists():
            return
        spec = importlib.util.spec_from_file_location('history_run_matrix', SCRIPT)
        cls.runner = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.runner)

    def setUp(self):
        self.assertTrue(SCRIPT.exists(), 'portable Python matrix runner is missing')
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.exe = self.directory / 'benchmark with spaces'
        self.exe.write_bytes(b'fixture executable')
        self.output = self.directory / 'nested' / 'report.json'
        self.commands = []
        self.mutate = lambda sample, number: None

    def fixture_process(self, command, **kwargs):
        if command[0] == 'git':
            self.assertEqual(command[command.index('-C') + 1], str(SCRIPT.resolve().parents[4]))
            return subprocess.CompletedProcess(command, 0, 'fixture-revision\n' if command[-1] == 'HEAD' else ' M tracked.py\n', '')
        self.commands.append(command)
        _, corpus, rows, budget, mode, pace, reads = command
        sample = {
            'schema_version': 2, 'verified': True, 'build_type': 'Release',
            'corpus': corpus, 'rows': int(rows), 'budget': int(budget), 'mode': mode.upper(),
            'pace_ms_per_64_rows': int(pace), 'admitted': int(mode == 'on'),
            'hash_fnv1a64': 'input-hash', 'retained_hash': 'retained-hash',
            'expected_retained_hash': 'retained-hash', 'input_cells': int(rows) * 80,
            'verified_cells': int(rows) * 80, 'retained_lines': int(rows),
            'cold_samples': min(2, int(reads)), 'cold_encoded_samples': int(mode == 'on'),
            'resident_charge': 100 if mode == 'off' else 50,
            'rss': 1000, 'peak_rss': 1100, 'append_ms': 2, 'append_wall_ms': 3,
            'ingest_cpu_ms': 4, 'ingest_wall_ms': 5, 'append_cell_mib_per_s': 100,
            'batch_p95_us': 10, 'batch_p99_us': 20, 'settle_ms': 1,
            'scan_ms': 1, 'scan_cpu_ms': 1, 'scan_cell_mib_per_s': 100,
            'read_p95_us': 10, 'read_p99_us': 20, 'cold_p95_us': 10, 'cold_p99_us': 20,
            'cold_encoded_p95_us': 10, 'cold_encoded_p99_us': 20,
            'hot_p95_us': 1, 'hot_p99_us': 2, 'index_ms': 1,
            'idle_store_close_ms': 1, 'idle_service_stop_ms': 1, 'compressed_blocks': int(mode == 'on'),
        }
        self.mutate(sample, len(self.commands))
        return subprocess.CompletedProcess(command, 0, json.dumps(sample), '')

    def run_matrix(self, *extra):
        argv = ['--executable', str(self.exe), '--output', str(self.output),
                '--rows', '10', '--long-rows', '3', '--runs', '3',
                '--budget', '1000', '--read-samples', '2', '--corpora', 'repeat']
        with patch.object(self.runner.subprocess, 'run', side_effect=self.fixture_process), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return self.runner.main(argv + list(extra))

    def report(self):
        return json.loads(self.output.read_text(encoding='utf-8'))

    def test_rotates_modes_and_saves_schema_hashes_and_all_metrics(self):
        self.assertEqual(self.run_matrix('--pace-milliseconds', '5'), 0)
        self.assertEqual([c[4] for c in self.commands], ['off', 'on', 'on', 'off', 'off', 'on'])
        report = self.report()
        self.assertEqual(report['schema_version'], 2)
        self.assertEqual(report['source_revision'], 'fixture-revision')
        self.assertTrue(report['working_tree_dirty'])
        self.assertEqual(report['executable_sha256'].lower(), hashlib.sha256(b'fixture executable').hexdigest())
        self.assertEqual({s['file'] for s in report['benchmark_sources']},
                         {'HistoryBenchmark.cpp', 'HistoryBenchmarkSupport.h', 'run-matrix.py'})
        self.assertEqual(len(report['samples']), 6)
        self.assertEqual([s['mode'] for s in report['summaries']], ['OFF', 'ON'])
        self.assertEqual(len(report['summaries'][0]['metrics']), 28)
        self.assertEqual(report['comparisons'][0]['charge_saving_fraction'], 0.5)
        self.assertTrue(report['synthetic_gates_passed'])

    def test_default_corpora_includes_long_and_rewrite_and_long_rows_override(self):
        self.assertEqual(self.run_matrix('--corpora', 'repeat', 'variable', 'mixed', 'entropy', 'long', 'rewrite'), 0)
        self.assertEqual(len(self.commands), 36)
        self.assertEqual({c[2] for c in self.commands if c[1] == 'long'}, {'3'})
        self.assertEqual({c[2] for c in self.commands if c[1] != 'long'}, {'10'})

    def test_worst_run_cold_percentile_fails_even_when_median_passes_and_report_is_saved(self):
        def slow_one(sample, number):
            if number == 2:
                sample['cold_encoded_p95_us'] = 50001
                sample['cold_encoded_p99_us'] = 100001
        self.mutate = slow_one
        self.assertNotEqual(self.run_matrix('--enforce-gates'), 0)
        report = self.report()
        self.assertFalse(report['synthetic_gates_passed'])
        on = report['summaries'][1]['metrics']
        self.assertEqual(on['cold_encoded_p95_us'], {'median': 10, 'min': 10, 'max': 50001})
        self.assertFalse(report['comparisons'][0]['gates']['cold_p99_le_100ms'])

    def test_failed_gates_are_advisory_without_enforcement(self):
        self.mutate = lambda sample, _: sample.update(append_cell_mib_per_s=50 if sample['mode'] == 'ON' else 100)
        self.assertEqual(self.run_matrix(), 0)
        self.assertFalse(self.report()['synthetic_gates_passed'])

    def test_equal_budget_omits_same_content_gates_and_allows_retained_hashes_to_differ(self):
        def evict(sample, _):
            sample.update(retained_lines=5, verified_cells=400)
            if sample['mode'] == 'ON':
                sample.update(retained_hash='another', expected_retained_hash='another')
        self.mutate = evict
        self.assertEqual(self.run_matrix('--scenario', 'EqualBudget', '--enforce-gates'), 0)
        comparison = self.report()['comparisons'][0]
        self.assertFalse(comparison['full_content'])
        self.assertEqual(set(comparison['gates']), {'cold_p95_le_50ms', 'cold_p99_le_100ms'})

    def test_absent_encoded_samples_omit_cold_gates(self):
        self.mutate = lambda sample, _: sample.update(cold_encoded_samples=0)
        self.assertEqual(self.run_matrix(), 0)
        self.assertFalse(self.report()['comparisons'][0]['cold_compressed_measured'])
        self.assertNotIn('cold_p95_le_50ms', self.report()['comparisons'][0]['gates'])

    def test_rejects_invalid_samples_before_reporting(self):
        bad = [('schema_version', 1), ('verified', False), ('build_type', 'Debug'),
               ('mode', 'on'), ('corpus', 'entropy'), ('rows', 11), ('budget', 99),
               ('pace_ms_per_64_rows', 3), ('cold_samples', 3), ('cold_encoded_samples', 3),
               ('read_samples', 3), ('retained_hash', 'wrong'), ('admitted', 0),
               ('append_ms', float('nan'))]
        for key, value in bad:
            with self.subTest(key=key):
                self.commands.clear()
                self.mutate = lambda sample, number: sample.update({key: value}) if number == 2 else None
                with self.assertRaises((ValueError, RuntimeError)):
                    self.run_matrix()
                self.assertFalse(self.output.exists())

    def test_rejects_input_and_full_content_hash_disagreement(self):
        for key in ('hash_fnv1a64', 'retained_hash'):
            with self.subTest(key=key):
                self.commands.clear()
                def mismatch(sample, number):
                    if number == 2:
                        sample[key] = 'different'
                        if key == 'retained_hash':
                            sample['expected_retained_hash'] = 'different'
                self.mutate = mismatch
                with self.assertRaises(ValueError):
                    self.run_matrix()

    def test_subprocess_failure_is_an_error_without_report(self):
        with patch.object(self.runner.subprocess, 'run', side_effect=subprocess.CalledProcessError(2, ['benchmark'])):
            with self.assertRaises((RuntimeError, subprocess.CalledProcessError)):
                self.runner.main(['--executable', str(self.exe), '--output', str(self.output)])
        self.assertFalse(self.output.exists())

    def test_cli_rejects_invalid_ranges_before_launch(self):
        invalid = [('rows', '0'), ('long-rows', '10000001'), ('budget', '0'),
                   ('pace-milliseconds', '1001'), ('runs', '2'), ('read-samples', '0')]
        for name, value in invalid:
            with self.subTest(name=name), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):
                    self.run_matrix('--' + name, value)
        self.assertEqual(self.commands, [])

    def test_legacy_argument_names_remain_usable(self):
        self.assertEqual(self.run_matrix('-Rows', '12', '-LongRows', '4', '-Runs', '3',
                                         '-PaceMilliseconds', '0', '-ReadSamples', '2', '-Scenario', 'SameContent'), 0)
        self.assertEqual({c[2] for c in self.commands}, {'12'})


if __name__ == '__main__':
    unittest.main()
