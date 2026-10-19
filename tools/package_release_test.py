"""Tests of package_release.py's shader key list release gate (stdlib only): python tools/package_release_test.py"""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import package_release as release  # noqa: E402

VERSION = "0123456789abcdef"
SUMMARY = ("[precompile] C:\\game\\shader-keys.txt (shipped, list hash {hash}): {records} records (0 unreadable or oversized "
           "lines, 0 duplicates, 0 format 1 or 2), 2 codes: 2 by locator (0 stale), 0 by scan (0 files, 0.0 MiB, 0.0 s), 0 "
           "not found; {records} compiled ({records} by inputs digest, 0 unverified), 0 present, 0 diverged, 0 unverifiable "
           "skipped, 0 failed in 1.0 s on 4 threads (2.0 s compiling); 0 entry names as recorded, {same} stored with the "
           "recorded SPIR-V; total 1.1 s\n")


class Gate(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        header = self.root / "build" / "generated" / "ShaderCacheVersion.hpp"
        header.parent.mkdir(parents=True)
        header.write_text(f"inline constexpr std::uint64_t SourceVersion = 0x{VERSION}ull;\n", encoding="utf-8")
        self.keys = b"k3 sample\r\n" * 3
        self.listed = f"{release.hash_bytes(self.keys):016x}"

    def tearDown(self):
        self.directory.cleanup()

    def check(self, text, keys=None, version=VERSION):
        path = self.root / "run.err"
        path.write_text(f"[shader-disk-cache] C:\\cache\\{version} (source version {version}, format 7)\n" + text,
                        encoding="utf-8")
        return release.check_precompile_log(path, self.root / "build", 3,
                                            release.hash_bytes(self.keys if keys is None else keys))

    def test_hash_matches_the_cpp_hash(self):
        # The values tests/ShaderKeyList.cpp pins for ShaderRecompiler::HashBytes.
        self.assertEqual(release.hash_bytes(b"k3 shader-keys sample\r\n"), 0xd9d5f7fe862de207)
        self.assertEqual(release.hash_bytes(b""), 0)

    def test_passes_for_the_packaged_list(self):
        self.assertEqual(self.check(SUMMARY.format(hash=self.listed, records=3, same=3)), 3)

    def test_refuses_another_list_of_the_same_size(self):
        with self.assertRaisesRegex(release.PackagingError, "list with hash"):
            self.check(SUMMARY.format(hash=self.listed, records=3, same=3), keys=b"k3 other1\r\n" * 3)

    def test_refuses_a_log_without_the_list_hash(self):
        old = SUMMARY.format(hash="x", records=3, same=3).replace(", list hash x", "")
        with self.assertRaisesRegex(release.PackagingError, "none logged"):
            self.check(old)

    def test_refuses_a_spirv_mismatch(self):
        with self.assertRaisesRegex(release.PackagingError, "recorded SPIR-V"):
            self.check(SUMMARY.format(hash=self.listed, records=3, same=2))

    def test_refuses_another_source_version(self):
        with self.assertRaisesRegex(release.PackagingError, "source version"):
            self.check(SUMMARY.format(hash=self.listed, records=3, same=3), version="fedcba9876543210")


if __name__ == "__main__":
    unittest.main()
