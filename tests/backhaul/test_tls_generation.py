import importlib.util
from pathlib import Path
import shutil
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('tls_generator', ROOT / 'tools/backhaul/prepare-peer-tls.py')
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)


class GeneratedTlsInputs(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.output = Path(self.directory.name) / 'generated'

    def snapshot(self):
        return {p.relative_to(self.output): (p.read_bytes(), p.stat().st_mtime_ns)
                for p in self.output.rglob('*') if p.is_file()}

    def test_repeat_build_preserves_files_and_clean_build_recreates_them(self):
        generator.prepare(self.output, debug=True)
        before = self.snapshot()
        generator.prepare(self.output, debug=True)
        self.assertEqual(self.snapshot(), before)
        shutil.rmtree(self.output)
        generator.prepare(self.output, debug=True)
        self.assertEqual({p: v[0] for p, v in self.snapshot().items()},
                         {p: v[0] for p, v in before.items()})

    def test_production_reuse_removes_debug_profile_and_stale_sources(self):
        generator.prepare(self.output, debug=True)
        self.assertTrue((self.output / 'admin/engine.c').is_file())
        stale = self.output / 'crypto/stale.c'
        stale.write_text('#error stale build input')
        generator.prepare(self.output, debug=False)
        self.assertFalse(list((self.output / 'admin').glob('*.c')))
        self.assertFalse(stale.exists())
        self.assertTrue((self.output / 'peer/engine.c').is_file())
        self.assertTrue((self.output / 'crypto/sha256.c').is_file())

    def test_source_tree_cannot_be_used_as_build_output(self):
        for path in (generator.OUT, generator.OUT / 'src', generator.SOURCE):
            with self.assertRaises(ValueError):
                generator.prepare(path)


if __name__ == '__main__':
    unittest.main()
