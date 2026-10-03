"""Test runner isolation with synthetic sources and a fake compiler; no hardware."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

RUNNER = Path(__file__).with_name('test_native.sh')


class NativeRunnerTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='hermes-runner-test-')
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        (self.root / 'scripts').mkdir()
        (self.root / 'test').mkdir()
        self.tmp = self.root / 'temporary binaries'
        self.tmp.mkdir()
        shutil.copyfile(RUNNER, self.root / 'scripts/test_native.sh')
        (self.root / 'test/example_test.cpp').write_text('// synthetic fixture\n')
        self.compiler = self.root / 'fake compiler'
        self.compiler.write_text('''#!/bin/sh
set -eu
while [ "$1" != -o ]; do shift; done
shift
printf '%s\\n' "$1" >> "$COMPILER_LOG"
printf '#!/bin/sh\\nsleep 0.1\\nexit %s\\n' "${TEST_EXIT:-0}" > "$1"
chmod +x "$1"
exit "${COMPILE_EXIT:-0}"
''')
        self.compiler.chmod(0o700)
        self.env = {**os.environ, 'TMPDIR': str(self.tmp), 'CXX': str(self.compiler),
                    'COMPILER_LOG': str(self.root / 'compiler.log')}

    def start(self, **overrides):
        return subprocess.Popen(['sh', 'scripts/test_native.sh'], cwd=self.root,
                                env={**self.env, **overrides},
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def test_concurrent_runs_use_distinct_directories_and_cleanup(self):
        first, second = self.start(), self.start()
        for process in (first, second):
            output, error = process.communicate(timeout=10)
            self.assertEqual(process.returncode, 0, (output, error))
        paths = (self.root / 'compiler.log').read_text().splitlines()
        self.assertEqual(len(paths), 2)
        self.assertNotEqual(Path(paths[0]).parent, Path(paths[1]).parent)
        self.assertEqual(list(self.tmp.iterdir()), [])

    def test_compile_and_test_failures_cleanup_and_preserve_exit_status(self):
        for failure in ('COMPILE_EXIT', 'TEST_EXIT'):
            with self.subTest(failure=failure):
                process = self.start(**{failure: '42'})
                process.communicate(timeout=10)
                self.assertEqual(process.returncode, 42)
                self.assertEqual(list(self.tmp.iterdir()), [])


if __name__ == '__main__':
    unittest.main()
