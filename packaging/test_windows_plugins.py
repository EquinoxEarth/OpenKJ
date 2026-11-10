"""Check runtime selection without requiring a Windows SDK on the test host."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    "package_windows", Path(__file__).parent / "package_windows.py")
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


class WindowsPluginSelectionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.prefix = Path(self.temporary.name)
        self.plugins = self.prefix / "lib/gstreamer-1.0"
        self.plugins.mkdir(parents=True)
        for name in package.GSTREAMER_PLUGINS:
            (self.plugins / f"libgst{name}.dll").touch()

    def test_excludes_unrelated_plugins_and_development_files(self):
        for name in ("libgstgtk4.dll", "libgstwebrtc.dll", "libgstcdg.dll.a"):
            (self.plugins / name).touch()
        selected = package.gstreamer_plugins(self.prefix)
        self.assertIn(self.plugins / "libgstcdg.dll", selected)
        self.assertIn(self.plugins / "libgstsoundtouch.dll", selected)
        self.assertNotIn(self.plugins / "libgstgtk4.dll", selected)
        self.assertNotIn(self.plugins / "libgstwebrtc.dll", selected)
        self.assertNotIn(self.plugins / "libgstcdg.dll.a", selected)

    def test_missing_required_plugins_fail_with_names(self):
        for name in ("soundtouch", "cdg", "wasapi2"):
            (self.plugins / f"libgst{name}.dll").unlink()
        with self.assertRaises(RuntimeError) as error:
            package.gstreamer_plugins(self.prefix)
        for name in ("libgstsoundtouch.dll", "libgstcdg.dll", "libgstwasapi2.dll"):
            self.assertIn(name, str(error.exception))


if __name__ == "__main__":
    unittest.main()
