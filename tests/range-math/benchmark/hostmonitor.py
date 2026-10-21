"""Wrap one guest phase and stream bounded, read-only QEMU/cgroup telemetry."""

import argparse
import json
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

MAX_TRACE_BYTES = 16 * 1024 * 1024


def parse_cpu_list(text):
    cpus = set()
    for piece in text.strip().split(","):
        if "-" in piece:
            first, last = map(int, piece.split("-", 1))
            if last < first:
                raise ValueError("reversed CPU range")
            cpus.update(range(first, last + 1))
        elif piece:
            cpus.add(int(piece))
    if not cpus:
        raise ValueError("empty CPU list")
    return sorted(cpus)


def siblings(cpu, sysfs=Path("/sys")):
    path = sysfs / f"devices/system/cpu/cpu{cpu}/topology/thread_siblings_list"
    related = parse_cpu_list(path.read_text())
    if cpu not in related:
        raise ValueError("CPU missing from own sibling topology")
    return [sibling for sibling in related if sibling != cpu]


def monitor_cpus(allowed, expected_cpu, sibling_cpus):
    remaining = set(allowed) - {expected_cpu, *sibling_cpus}
    if not remaining:
        raise ValueError("no host CPU remains for monitor and SSH")
    return remaining


def key_values(path):
    values = {}
    for line in path.read_text().splitlines():
        name, value = line.split(None, 1)
        values[name] = int(value)
    return values


def proc_stat(path):
    # The comm field is parenthesized and may itself contain spaces.
    fields = path.read_text().rsplit(") ", 1)[1].split()
    return {"starttime": int(fields[19]), "cpu_ticks": int(fields[11]) + int(fields[12]),
            "processor": int(fields[36])}


def status_value(path, key):
    for line in path.read_text().splitlines():
        if line.startswith(key + ":"):
            return line.split(":", 1)[1].strip()
    raise ValueError(f"{key} missing from {path}")


def parse_qmp_cpus(cpus):
    if not isinstance(cpus, list) or len(cpus) != 2:
        raise ValueError("expected two QMP vCPUs")
    mapping = {}
    for cpu in cpus:
        index, tid = cpu.get("cpu-index"), cpu.get("thread-id")
        if type(index) is not int or type(tid) is not int or index in mapping or tid <= 0:
            raise ValueError("invalid QMP vCPU identity")
        mapping[index] = tid
    if set(mapping) != {0, 1} or len(set(mapping.values())) != 2:
        raise ValueError("QMP vCPU mapping incomplete or duplicated")
    return mapping


def query_qmp_cpus(path):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(3)
        connection.connect(str(path))
        stream = connection.makefile("rwb", buffering=0)
        greeting = json.loads(stream.readline(65536))
        if "QMP" not in greeting:
            raise ValueError("QMP greeting missing")

        def command(name, identifier):
            connection.sendall((json.dumps({"execute": name, "id": identifier}) + "\n").encode())
            for _ in range(20):
                response = json.loads(stream.readline(65536))
                if response.get("id") == identifier:
                    if "error" in response:
                        raise ValueError(f"QMP {name} failed: {response['error']}")
                    return response["return"]
            raise ValueError(f"QMP {name} response missing")

        command("qmp_capabilities", "caps")
        return parse_qmp_cpus(command("query-cpus-fast", "cpus"))


def snapshot(pid, expected_cpu, qmp_cpus, proc=Path("/proc"), cgroup=Path("/sys/fs/cgroup"),
             sysfs=Path("/sys")):
    p = proc / str(pid)
    membership = (p / "cgroup").read_text().splitlines()
    unified = [line[3:] for line in membership if line.startswith("0::")]
    if len(unified) != 1 or not unified[0].startswith("/"):
        raise ValueError("QEMU unified cgroup membership missing")
    group = cgroup / unified[0].lstrip("/")
    threads = []
    for task in sorted((p / "task").iterdir(), key=lambda entry: int(entry.name)):
        tid = int(task.name)
        try:
            stat = proc_stat(task / "stat")
            role = next((f"CPU {index}/KVM" for index, cpu_tid in qmp_cpus.items()
                         if cpu_tid == tid), "emulator")
            thread = {"tid": tid, "role": role,
                      "comm": (task / "comm").read_text().strip(),
                      "affinity": parse_cpu_list(status_value(task / "status", "Cpus_allowed_list")),
                      "processor": stat["processor"], "starttime": stat["starttime"],
                      "schedstat": [int(v) for v in (task / "schedstat").read_text().split()]}
        except (FileNotFoundError, ProcessLookupError) as exc:
            if tid in qmp_cpus.values():
                raise ValueError(f"QMP vCPU task {tid} disappeared") from exc
            continue  # QEMU may retire an emulator worker during /proc reads.
        threads.append(thread)
    if not threads:
        raise ValueError("QEMU threads missing")
    if not set(qmp_cpus.values()).issubset({thread["tid"] for thread in threads}):
        raise ValueError("QMP vCPU TID missing from QEMU process")
    events = key_values(group / "memory.events.local")
    stat = key_values(group / "memory.stat")
    rss_line = status_value(p / "status", "VmRSS")
    rss_bytes = int(rss_line.split()[0]) * 1024
    process_state = proc_stat(p / "stat")
    return {"pid": pid, "starttime": process_state["starttime"],
            "cgroup": unified[0], "expected_cpu": expected_cpu,
            "qmp_cpus": {str(index): tid for index, tid in qmp_cpus.items()},
            "sibling_cpus": siblings(expected_cpu, sysfs),
            "memory": {"current": int((group / "memory.current").read_text()),
                       "events": events, "stat": stat},
            "pressure": {"memory": (group / "memory.pressure").read_text().strip(),
                         "cpu": (group / "cpu.pressure").read_text().strip()},
            "rss_bytes": rss_bytes, "cpu_ticks": process_state["cpu_ticks"],
            "threads": threads}


def validate_placement(row):
    cpu = row["expected_cpu"]
    excluded = {cpu, *row["sibling_cpus"]}
    benchmark = [thread for thread in row["threads"] if thread["role"] == "CPU 0/KVM"]
    if len(benchmark) != 1 or benchmark[0]["affinity"] != [cpu]:
        raise ValueError("benchmark vCPU not exclusively pinned to expected CPU")
    for thread in row["threads"]:
        if thread is not benchmark[0] and excluded.intersection(thread["affinity"]):
            raise ValueError("other QEMU thread can run on benchmark core or SMT sibling")


def validate_snapshot_transition(previous, current):
    for key in ("pid", "starttime", "cgroup", "expected_cpu", "sibling_cpus", "qmp_cpus"):
        if previous[key] != current[key]:
            raise ValueError(f"QEMU identity changed: {key}")
    before = {(t["tid"], t["starttime"]): t for t in previous["threads"]}
    after = {(t["tid"], t["starttime"]): t for t in current["threads"]}
    for tid in previous["qmp_cpus"].values():
        old = [item for key, item in before.items() if key[0] == tid]
        new = [item for key, item in after.items() if key[0] == tid]
        if len(old) != 1 or len(new) != 1 or old[0]["starttime"] != new[0]["starttime"]:
            raise ValueError("QMP vCPU identity changed")
    for identity in before.keys() & after.keys():
        old, new = before[identity], after[identity]
        if old["role"] != new["role"] or old["affinity"] != new["affinity"]:
            raise ValueError("QEMU surviving thread role or affinity changed")


def monitor(output_dir, pid, expected_cpu, qmp_socket, session_id, phase, command, deadline):
    if output_dir.exists():
        raise ValueError(f"output directory already exists: {output_dir}")
    output_dir.mkdir(parents=True)
    trace_path = output_dir / "host.jsonl"
    result_path = output_dir / "host-result.json"
    started = time.monotonic()
    result = {"schema": 1, "phase": phase, "session_id": session_id,
              "command": command, "status": "running", "exit_code": None, "error": None,
              "child_pid": None, "started_monotonic": started, "ended_monotonic": None}
    result_path.write_text(json.dumps(result, indent=2) + "\n")
    child = None
    previous = None
    bytes_written = 0
    try:
        os.sched_setaffinity(0, monitor_cpus(os.sched_getaffinity(0), expected_cpu,
                                             siblings(expected_cpu)))
        result["monitor_affinity"] = sorted(os.sched_getaffinity(0))
        with trace_path.open("w", encoding="utf-8") as out:
            def sample():
                nonlocal previous, bytes_written
                row = snapshot(pid, expected_cpu, query_qmp_cpus(qmp_socket))
                row.update({"t": time.monotonic(), "phase": phase, "session_id": session_id})
                validate_placement(row)
                if previous is not None:
                    validate_snapshot_transition(previous, row)
                previous = row
                line = json.dumps(row, separators=(",", ":")) + "\n"
                bytes_written += len(line.encode())
                if bytes_written > MAX_TRACE_BYTES:
                    raise ValueError("host trace exceeds 16 MiB")
                out.write(line)
                out.flush()
                os.fsync(out.fileno())

            sample()  # Before starting SSH.
            child = subprocess.Popen(command)
            result["child_pid"] = child.pid
            next_sample = time.monotonic() + 1
            while child.poll() is None:
                if time.monotonic() - started > deadline:
                    raise TimeoutError("host phase deadline exceeded")
                time.sleep(min(.1, max(0, next_sample - time.monotonic())))
                if time.monotonic() >= next_sample:
                    sample()
                    next_sample += 1
            sample()  # After SSH ends, including failing commands.
            result["exit_code"] = child.returncode
            if child.returncode != 0:
                raise ValueError(f"guest command exited {child.returncode}")
            result["status"] = "complete"
    except BaseException as exc:
        result["status"] = "failed"
        result["error"] = str(exc) or type(exc).__name__
    finally:
        if child is not None:
            if child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()
            else:
                child.wait()
            result["exit_code"] = child.returncode
        result["elapsed_seconds"] = time.monotonic() - started
        result["ended_monotonic"] = time.monotonic()
        result_path.write_text(json.dumps(result, indent=2) + "\n")
    return 0 if result["status"] == "complete" else 2


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--expected-cpu", type=int, required=True)
    parser.add_argument("--qmp-socket", type=Path, required=True)
    parser.add_argument("--session-id", required=True)
    parser.add_argument("--phase", choices=("calibration", "comparison"), required=True)
    parser.add_argument("--deadline-seconds", type=int, default=5700)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    parser.error("command required after --") if not command else None
    return monitor(args.output_dir, args.pid, args.expected_cpu, args.qmp_socket, args.session_id,
                   args.phase, command, args.deadline_seconds)


if __name__ == "__main__":
    sys.exit(main())
