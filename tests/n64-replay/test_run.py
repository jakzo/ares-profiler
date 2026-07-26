import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path


RUN_PATH = Path(__file__).with_name("run.py")
SPEC = importlib.util.spec_from_file_location("n64_replay_run", RUN_PATH)
RUN = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = RUN
SPEC.loader.exec_module(RUN)


class ProfileValidationTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.prefix = Path(self.temporary.name) / "profile"
        self.write(
            "-summary.csv",
            """metric,value
stage,35
start_cycle,100
end_cycle,1000
total_cycles,900
frames,2
average_frame_delta,250
tlb_cache_hits,3
tlb_cache_misses,1
tlb_missing,1
""",
        )
        self.write(
            "-functions.csv",
            """address,size,name,calls,self_cycles,inclusive_cycles
0x80001000,64,"test_function",1,100,150
""",
        )
        self.write(
            "-tlb.csv",
            """page,accesses,loads,stores,cache_hits,cache_misses,missing
0x80000000,4,3,1,3,1,1
""",
        )
        self.write(
            "-frames.csv",
            """frame,start_cycle,end_cycle,delta_cycles
0,100,300,200
1,300,600,300
""",
        )
        self.write(
            "-game-frames.csv",
            """frame,tick_cycles,tlb_loads,start_cycle,end_cycle
0,100,2,100,300
1,150,3,300,600
""",
        )
        self.write(".folded", "test_function 100\n")

    def write(self, suffix, contents):
        RUN.profile_path(self.prefix, suffix).write_text(contents, encoding="utf-8")

    def test_accepts_valid_profiler_output(self):
        self.assertEqual(RUN.verify_profiles(self.prefix, 2), [])

    def test_rejects_inconsistent_profiler_output(self):
        corruptions = {
            "missing game frame": (
                "-game-frames.csv",
                """frame,tick_cycles,tlb_loads,start_cycle,end_cycle
0,100,2,100,300
""",
                "game frame profile has 1 rows, expected 2",
            ),
            "incorrect TLB totals": (
                "-tlb.csv",
                """page,accesses,loads,stores,cache_hits,cache_misses,missing
0x80000000,4,3,1,2,1,1
""",
                "cache results do not equal accesses",
            ),
            "malformed folded stack": (
                ".folded",
                "test_function nope\n",
                "folded row 0 is invalid",
            ),
        }
        for name, (suffix, contents, expected_error) in corruptions.items():
            with self.subTest(name=name):
                original = RUN.profile_path(self.prefix, suffix).read_text(
                    encoding="utf-8"
                )
                self.write(suffix, contents)
                errors = RUN.verify_profiles(self.prefix, 2)
                self.assertTrue(
                    any(expected_error in error for error in errors), errors
                )
                self.write(suffix, original)


if __name__ == "__main__":
    unittest.main()
