import hashlib
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location(
    "build_qt_lto", Path(__file__).resolve().parents[1] / "scripts/build-qt-lto.py")
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


class SourceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        data = io.BytesIO()
        with tarfile.open(fileobj=data, mode="w:gz") as archive:
            info = tarfile.TarInfo("module/source.txt")
            info.size = 6
            archive.addfile(info, io.BytesIO(b"source"))
        self.archive = data.getvalue()
        self.checksum = hashlib.sha256(self.archive).hexdigest()

    def download(self, *args):
        Path(args[-1]).write_bytes(self.archive)

    def source(self, **kwargs):
        return builder.source(self.work, "module.tar.gz", "primary", self.checksum,
                              "module", **kwargs)

    def test_primary_and_cached_source(self):
        with patch.object(builder, "run", side_effect=self.download) as run:
            source = self.source(fallback_url="archive")
            self.assertEqual((source / "source.txt").read_text(), "source")
            self.assertEqual(self.source(), source)
            run.assert_called_once()
        self.assertFalse((self.work / "module.tar.gz.part").exists())

    def test_archived_release_fallback(self):
        def download(*args):
            if args[-3] == "primary":
                Path(args[-1]).write_bytes(b"incomplete download")
                raise subprocess.CalledProcessError(22, args)
            self.download(*args)

        with patch.object(builder, "run", side_effect=download) as run:
            source = self.source(fallback_url="archive")
            self.assertEqual((source / "source.txt").read_text(), "source")
            self.assertEqual([call.args[-3] for call in run.call_args_list], ["primary", "archive"])

    def test_failed_download_is_not_cached(self):
        def fail(*args):
            Path(args[-1]).write_bytes(b"incomplete download")
            raise subprocess.CalledProcessError(22, args)

        for options in ({}, {"fallback_url": "archive"}):
            with self.subTest(options=options), patch.object(builder, "run", side_effect=fail):
                with self.assertRaises(subprocess.CalledProcessError):
                    self.source(**options)
                self.assertEqual(list(self.work.iterdir()), [])

    def test_checksum_mismatch_is_not_cached_or_extracted(self):
        self.checksum = "0" * 64
        with patch.object(builder, "run", side_effect=self.download):
            with self.assertRaisesRegex(RuntimeError, "checksum mismatch"):
                self.source(fallback_url="archive")
        self.assertEqual(list(self.work.iterdir()), [])

    def test_cached_archive_is_verified(self):
        (self.work / "module.tar.gz").write_bytes(b"corrupt cached source")
        with patch.object(builder, "run") as run:
            with self.assertRaisesRegex(RuntimeError, "checksum mismatch"):
                self.source()
            run.assert_not_called()
        self.assertFalse((self.work / "src").exists())


class BuildEnvironmentTests(unittest.TestCase):
    def test_seed_paths_cannot_override_build_tree_libraries_or_tools(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            seed, install = root / "seed", root / "lto"
            other = root / "seed-other"
            environment = {
                "PATH": os.pathsep.join(map(str, [seed / "bin", other / "bin"])),
                "LD_LIBRARY_PATH": os.pathsep.join(map(str, [seed / "lib", other / "lib"])),
                "DYLD_LIBRARY_PATH": str(seed / "lib"),
                "DYLD_FRAMEWORK_PATH": str(seed / "lib"),
                "PKG_CONFIG_PATH": os.pathsep.join(map(str, [seed / "lib/pkgconfig", other])),
                "QML2_IMPORT_PATH": str(seed / "qml"),
                "QML_IMPORT_PATH": str(seed / "qml"),
                "QT_PLUGIN_PATH": str(seed / "plugins"),
                "QT_QPA_PLATFORM_PLUGIN_PATH": str(seed / "plugins/platforms"),
                "CMAKE_PREFIX_PATH": str(seed), "Qt6_DIR": str(seed / "lib/cmake/Qt6"),
                "QTDIR": str(seed), "QT_ROOT_DIR": str(seed),
                "CC": "custom-compiler",
            }
            with patch.dict(os.environ, environment, clear=True):
                builder.prepare_build_environment(seed, install)
                self.assertEqual(dict(os.environ), {
                    "PATH": os.pathsep.join(map(str, [install / "bin", other / "bin"])),
                    "LD_LIBRARY_PATH": str(other / "lib"),
                    "PKG_CONFIG_PATH": str(other), "CC": "custom-compiler",
                })

    def test_environment_without_seed_paths_is_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            with patch.dict(os.environ, {"PATH": str(root / "tools")}, clear=True):
                builder.prepare_build_environment(root / "seed", root / "lto")
                self.assertEqual(dict(os.environ), {
                    "PATH": os.pathsep.join(map(str, [root / "lto/bin", root / "tools"])),
                })

    def test_seed_only_path_does_not_leave_current_directory_search(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            with patch.dict(os.environ, {"PATH": str(root / "seed/bin")}, clear=True):
                builder.prepare_build_environment(root / "seed", root / "lto")
                self.assertEqual(os.environ["PATH"], str(root / "lto/bin"))


class MsvcMultimediaPatchTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.src = Path(self.temp.name)
        self.header = self.src / "src/multimedia/playback/qplaybackoptions.h"

    def test_exports_destructor_and_preserves_other_declarations(self):
        self.header.parent.mkdir(parents=True)
        before = ("class QPlaybackOptionsPrivate;\n"
                  "QT_DECLARE_QESDP_SPECIALIZATION_DTOR(QPlaybackOptionsPrivate)\n"
                  "class QPlaybackOptions {};\n")
        self.header.write_text(before)
        builder.patch_msvc_multimedia(self.src)
        expected = before.replace(
            "QT_DECLARE_QESDP_SPECIALIZATION_DTOR(QPlaybackOptionsPrivate)",
            "QT_DECLARE_QESDP_SPECIALIZATION_DTOR_WITH_EXPORT("
            "QPlaybackOptionsPrivate, Q_MULTIMEDIA_EXPORT)")
        self.assertEqual(self.header.read_text(), expected)
        builder.patch_msvc_multimedia(self.src)
        self.assertEqual(self.header.read_text(), expected)

    def test_qt_without_playback_options(self):
        builder.patch_msvc_multimedia(self.src)
        self.assertEqual(list(self.src.iterdir()), [])

    def test_unrecognized_declaration_fails_without_modifying_header(self):
        self.header.parent.mkdir(parents=True)
        self.header.write_text("unexpected upstream header\n")
        with self.assertRaisesRegex(RuntimeError, "Unrecognized QPlaybackOptions"):
            builder.patch_msvc_multimedia(self.src)
        self.assertEqual(self.header.read_text(), "unexpected upstream header\n")


if __name__ == "__main__":
    unittest.main()
