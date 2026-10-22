"""Host monitor checks that protect QEMU placement and evidence continuity."""

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).parent))
import hostmonitor


class HostMonitorTests(unittest.TestCase):
    def test_cpu_list_and_topology_derive_real_siblings(self):
        with tempfile.TemporaryDirectory() as temp:
            sysfs = Path(temp)
            path = sysfs / "devices/system/cpu/cpu2/topology/thread_siblings_list"
            path.parent.mkdir(parents=True)
            path.write_text("2-3\n")
            self.assertEqual(hostmonitor.parse_cpu_list("0-2,5"), [0, 1, 2, 5])
            self.assertEqual(hostmonitor.siblings(2, sysfs), [3])

    def test_snapshot_reads_cgroup_local_events_and_thread_masks(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            proc = root / "proc"
            cg = root / "cg"
            sysfs = root / "sys"
            pid = 42
            task = proc / "42/task"
            for tid, role, mask, processor in ((42, "qemu-system-x86", "4-5", 4),
                                               (43, "qemu-system-x86", "2", 2),
                                               (44, "qemu-system-x86", "4", 4)):
                d = task / str(tid)
                d.mkdir(parents=True)
                (d / "comm").write_text(role + "\n")
                (d / "status").write_text(f"Cpus_allowed_list:\t{mask}\n")
                # Fields 3..39 after pid and comm: state, ..., starttime 22, processor 39.
                fields = ["0"] * 37
                fields[0] = "S"; fields[11] = "10"; fields[12] = "20"
                fields[19] = "1001"; fields[36] = str(processor)
                (d / "stat").write_text(f"{tid} ({role}) " + " ".join(fields))
                (d / "schedstat").write_text("100 20 3\n")
            d = proc / "42"
            (d / "cgroup").write_text("0::/bench\n")
            (d / "status").write_text("VmRSS:\t2048 kB\n")
            (d / "stat").write_text((task / "42/stat").read_text())
            c = cg / "bench"
            c.mkdir(parents=True)
            (c / "memory.current").write_text("1048576\n")
            (c / "memory.stat").write_text("anon 500000\nfile 548576\n")
            (c / "memory.events.local").write_text("max 7\noom 0\noom_kill 0\n")
            (c / "memory.pressure").write_text("some avg10=0.00\nfull avg10=0.00\n")
            (c / "cpu.pressure").write_text("some avg10=0.00\nfull avg10=0.00\n")
            sibling = sysfs / "devices/system/cpu/cpu2/topology/thread_siblings_list"
            sibling.parent.mkdir(parents=True)
            sibling.write_text("2-3\n")
            row = hostmonitor.snapshot(pid, 2, {0: 43, 1: 44}, proc, cg, sysfs)
            self.assertEqual(row["sibling_cpus"], [3])
            self.assertEqual(row["memory"]["events"]["max"], 7)
            self.assertEqual(row["threads"][1]["affinity"], [2])
            self.assertEqual(row["threads"][1]["role"], "CPU 0/KVM")
            self.assertEqual(row["threads"][2]["role"], "CPU 1/KVM")
            self.assertEqual(row["starttime"], 1001)
            worker = task / "45"
            worker.mkdir()
            (worker / "comm").write_text("qemu-system-x86\n")
            (worker / "status").write_text("Cpus_allowed_list:\t4\n")
            (worker / "schedstat").write_text("100 20 3\n")
            worker_fields = fields.copy(); worker_fields[11] = "100"; worker_fields[12] = "20"
            (worker / "stat").write_text("45 (qemu-system-x86) " + " ".join(worker_fields))
            process_fields = fields.copy(); process_fields[11] = "130"; process_fields[12] = "20"
            (d / "stat").write_text("42 (qemu-system-x86) " + " ".join(process_fields))
            before_retirement = hostmonitor.snapshot(pid, 2, {0: 43, 1: 44}, proc, cg, sysfs)
            self.assertEqual(before_retirement["cpu_ticks"], 150)
            for item in worker.iterdir(): item.unlink()
            worker.rmdir()
            process_fields[11] = "140"
            (d / "stat").write_text("42 (qemu-system-x86) " + " ".join(process_fields))
            after_retirement = hostmonitor.snapshot(pid, 2, {0: 43, 1: 44}, proc, cg, sysfs)
            self.assertEqual(after_retirement["cpu_ticks"], 160)
            (task / "45").mkdir()  # Enumerated worker retired before its stat read.
            row = hostmonitor.snapshot(pid, 2, {0: 43, 1: 44}, proc, cg, sysfs)
            self.assertNotIn(45, [thread["tid"] for thread in row["threads"]])
            (task / "43/stat").unlink()
            with self.assertRaisesRegex(ValueError, "vCPU"):
                hostmonitor.snapshot(pid, 2, {0: 43, 1: 44}, proc, cg, sysfs)

    def test_qmp_cpu_mapping_requires_unique_indices_and_threads(self):
        self.assertEqual(hostmonitor.parse_qmp_cpus([
            {"cpu-index": 0, "thread-id": 43}, {"cpu-index": 1, "thread-id": 44}]),
            {0: 43, 1: 44})
        with self.assertRaises(ValueError):
            hostmonitor.parse_qmp_cpus([{"cpu-index": 0, "thread-id": 43},
                                        {"cpu-index": 1, "thread-id": 43}])

    def test_monitor_and_ssh_affinity_excludes_benchmark_core(self):
        self.assertEqual(hostmonitor.monitor_cpus({2, 3, 4, 5}, 2, [3]), {4, 5})
        with self.assertRaises(ValueError):
            hostmonitor.monitor_cpus({2, 3}, 2, [3])

    def test_interrupt_marks_failure_and_reaps_owned_command(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "phase"
            allowed = sorted(os.sched_getaffinity(0))
            expected_cpu = allowed[0]
            if len(allowed) < 2:
                self.skipTest("requires two available CPUs")
            fake = {"pid": 42, "starttime": 1001, "cgroup": "/bench",
                    "sibling_cpus": [], "expected_cpu": expected_cpu,
                    "threads": [{"tid": 43, "starttime": 1001, "role": "CPU 0/KVM",
                                 "affinity": [expected_cpu]}]}
            try:
                with mock.patch.object(hostmonitor, "snapshot", side_effect=[fake, KeyboardInterrupt]), \
                     mock.patch.object(hostmonitor, "query_qmp_cpus", return_value={0: 43, 1: 44}), \
                     mock.patch.object(hostmonitor, "siblings", return_value=[]):
                    code = hostmonitor.monitor(root, 42, expected_cpu, Path("unused"), "session",
                        "calibration", [sys.executable, "-c", "import time; time.sleep(30)"], 5)
            finally:
                os.sched_setaffinity(0, allowed)
            self.assertNotEqual(code, 0)
            result = json.loads((root / "host-result.json").read_text())
            self.assertEqual(result["status"], "failed")
            self.assertIsNotNone(result["child_pid"])
            with self.assertRaises(ChildProcessError):
                os.waitpid(result["child_pid"], os.WNOHANG)

    def test_dynamic_emulator_threads_preserve_vcpu_identity_and_survivor_masks(self):
        first = {"pid": 42, "starttime": 1001, "cgroup": "/bench", "expected_cpu": 2,
                 "sibling_cpus": [3], "qmp_cpus": {"0": 43, "1": 44},
                 "threads": [
                     {"tid": 43, "starttime": 1001, "role": "CPU 0/KVM", "affinity": [2]},
                     {"tid": 44, "starttime": 1001, "role": "CPU 1/KVM", "affinity": [4]},
                     {"tid": 45, "starttime": 1001, "role": "emulator", "affinity": [4]}]}
        second = {**first, "threads": [first["threads"][0], first["threads"][1],
                    {"tid": 46, "starttime": 1002, "role": "emulator", "affinity": [4]}]}
        hostmonitor.validate_snapshot_transition(first, second)
        changed = {**second, "threads": [first["threads"][0],
                    {**first["threads"][1], "affinity": [5]}, second["threads"][2]]}
        with self.assertRaisesRegex(ValueError, "affinity"):
            hostmonitor.validate_snapshot_transition(first, changed)
        changed_vcpu = {**second, "threads": [{**first["threads"][0], "starttime": 2000},
                        first["threads"][1], second["threads"][2]]}
        with self.assertRaisesRegex(ValueError, "vCPU"):
            hostmonitor.validate_snapshot_transition(first, changed_vcpu)


if __name__ == "__main__":
    unittest.main()
