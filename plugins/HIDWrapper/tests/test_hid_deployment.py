"""Integrated HID/CI migration tests; temporary directories only."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / 'tools/deploy_hid.ps1'
MIGRATION = SCRIPT.with_name('remove_standalone_injector.ps1')


@unittest.skipUnless(os.name == 'nt' and shutil.which('pwsh'), 'Windows PowerShell fixture')
class HidDeploymentTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='hid-deployment-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.game = self.root / 'game'
        self.legacy = self.game / 'reframework/plugins/ClassInjector.dll'
        self.legacy.parent.mkdir(parents=True)
        self.legacy.write_bytes(b'legacy')
        self.target = self.game / 'hid.dll'
        self.target.write_bytes(b'old-wrapper')
        self.source = self.root / 'build/hid.dll'
        self.source.parent.mkdir()
        self.source.write_bytes(b'integrated')

    def deploy(self):
        return subprocess.run(['pwsh', '-NoLogo', '-NoProfile', '-File', str(SCRIPT),
                               '-Source', str(self.source), '-GameRoot', str(self.game)],
                              capture_output=True, text=True, encoding='utf-8')

    def test_deployment_only_updates_hid(self):
        other = self.legacy.with_name('Other.dll'); other.write_bytes(b'unrelated')
        result = self.deploy()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.target.read_bytes(), b'integrated')
        self.assertEqual(self.legacy.read_bytes(), b'legacy')
        self.assertEqual(other.read_bytes(), b'unrelated')
        self.assertEqual(self.deploy().returncode, 0, 'repeat deployment is valid')

    def migrate(self):
        return subprocess.run(['pwsh', '-NoLogo', '-NoProfile', '-File', str(MIGRATION),
                               '-GameRoot', str(self.game)], capture_output=True, text=True, encoding='utf-8')

    def test_explicit_migration_only_removes_standalone_host(self):
        other = self.legacy.with_name('Other.dll'); other.write_bytes(b'unrelated')
        self.assertEqual(self.deploy().returncode, 0)
        result = self.migrate()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.legacy.exists())
        self.assertEqual(other.read_bytes(), b'unrelated')
        self.assertEqual(self.target.read_bytes(), b'integrated')
        self.assertEqual(self.migrate().returncode, 0, 'migration is idempotent')

    def test_missing_build_preserves_both_old_files(self):
        self.source.unlink()
        result = self.deploy()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.target.read_bytes(), b'old-wrapper')
        self.assertEqual(self.legacy.read_bytes(), b'legacy')

    def test_locked_hid_is_checked_before_copy(self):
        with self.target.open('rb'):
            result = self.deploy()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.target.read_bytes(), b'old-wrapper')
        self.assertEqual(self.legacy.read_bytes(), b'legacy')

    def test_locked_standalone_host_is_preserved_by_migration(self):
        with self.legacy.open('rb'):
            result = self.migrate()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.target.read_bytes(), b'old-wrapper')
        self.assertEqual(self.legacy.read_bytes(), b'legacy')


if __name__ == '__main__':
    unittest.main()
