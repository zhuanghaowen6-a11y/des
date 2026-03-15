#!/usr/bin/env python3
import os
import sys
import re
import csv
import time
import importlib.util
from datetime import datetime


def load_analyze_module(project_root):
    path = os.path.join(project_root, "scripts", "analyze_bgp_logs.py")
    spec = importlib.util.spec_from_file_location("analyze_bgp_logs", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def extract_vt(line: str):
    m = re.search(r"\[VT=([\d.]+)\]", line)
    if not m:
        return None
    try:
        return float(m.group(1))
    except Exception:
        return None


def infer_t_fail_vt(result_dir: str, tor_id: int, keep_agg_id: int):
    log_path = os.path.join(result_dir, "logs", f"bird_r{tor_id}.log")
    if not os.path.exists(log_path):
        raise FileNotFoundError(f"TOR log not found: {log_path}")

    t_fail_epoch_path = os.path.join(result_dir, "meta", "t_fail_epoch.txt")
    t_fail_epoch = None
    if os.path.exists(t_fail_epoch_path):
        try:
            t_fail_epoch = float(open(t_fail_epoch_path).read().strip())
        except Exception:
            t_fail_epoch = None

    # Look for the first down/closed event for the target peer.
    # We prefer explicit BGP state messages.
    patterns = [
        re.compile(rf"<TRACE> r{keep_agg_id}: BGP session closed"),
        re.compile(rf"<TRACE> r{keep_agg_id}: State changed to down"),
        re.compile(rf"<TRACE> r{keep_agg_id}: State changed to stop"),
    ]

    def parse_wallclock_epoch(line: str):
        m = re.search(r"bird:\s+(\d{4}-\d{2}-\d{2}\s+\d{2}:\d{2}:\d{2}\.\d{3,6})", line)
        if not m:
            return None
        s = m.group(1)
        fmt = "%Y-%m-%d %H:%M:%S.%f"
        try:
            dt = datetime.strptime(s, fmt)
        except Exception:
            return None
        # Treat as local time (same as date(1) output on host)
        return time.mktime(dt.timetuple()) + (dt.microsecond / 1e6)

    last_vt = None
    with open(log_path, "r", errors="replace") as f:
        for line in f:
            vt = extract_vt(line)
            if vt is not None:
                last_vt = vt

            if any(p.search(line) for p in patterns):
                if vt is not None:
                    return vt

            if t_fail_epoch is not None:
                wc = parse_wallclock_epoch(line)
                if wc is not None and wc >= t_fail_epoch and vt is not None:
                    return vt

    if t_fail_epoch is not None and last_vt is not None:
        return last_vt

    raise RuntimeError(
        f"Failed to infer t_fail_vt from {log_path}. "
        f"No usable down/stop line for r{keep_agg_id} and no usable meta/t_fail_epoch.txt mapping were found."
    )


def main():
    if len(sys.argv) < 3:
        print("Usage: compute_convergence_curve_vt.py <RESULT_DIR> <target_prefix> [topology_mode] [num_routers] [tor_id] [keep_agg_id]")
        print("Example: compute_convergence_curve_vt.py results/... 192.168.41.0/24 fat-tree-k8-64 64 41 17")
        return 2

    result_dir = sys.argv[1]
    target_prefix = sys.argv[2]
    topology_mode = (sys.argv[3] if len(sys.argv) >= 4 else os.environ.get("TOPOLOGY_MODE") or "fat-tree-k8-64").lower()
    num_routers = int(sys.argv[4]) if len(sys.argv) >= 5 else int(os.environ.get("NUM_ROUTERS", "64"))
    tor_id = int(sys.argv[5]) if len(sys.argv) >= 6 else int(os.environ.get("TOR_ID", "41"))
    keep_agg_id = int(sys.argv[6]) if len(sys.argv) >= 7 else int(os.environ.get("KEEP_AGG_ID", "17"))

    log_dir = os.path.join(result_dir, "logs")
    meta_dir = os.path.join(result_dir, "meta")
    os.makedirs(meta_dir, exist_ok=True)

    if not os.path.isdir(log_dir):
        print(f"[ERROR] log dir not found: {log_dir}")
        return 1

    t_fail_vt_file = os.path.join(meta_dir, "t_fail_vt.txt")
    if os.path.exists(t_fail_vt_file):
        t_fail_vt = float(open(t_fail_vt_file).read().strip())
    else:
        t_fail_vt = infer_t_fail_vt(result_dir, tor_id, keep_agg_id)
        with open(t_fail_vt_file, "w") as f:
            f.write(f"{t_fail_vt:.6f}\n")

    project_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    abl = load_analyze_module(project_root)

    # Force VT parsing
    abl.TIME_MODE = "vt"

    per_router = []
    for rid in range(1, num_routers + 1):
        log_path = os.path.join(log_dir, f"bird_r{rid}.log")
        r = abl.analyze_route_convergence(log_path, rid, topology_mode=topology_mode)

        times = [t for (t, p, _a) in r["route_updates"] if p == target_prefix and t >= t_fail_vt]
        t_i_abs = max(times) if times else t_fail_vt
        per_router.append((rid, max(0.0, t_i_abs - t_fail_vt)))

    out1 = os.path.join(meta_dir, "per_router_convergence.csv")
    with open(out1, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["router_id", "t_converged_s"])
        for rid, t in per_router:
            w.writerow([rid, f"{t:.6f}"])

    times_sorted = sorted(t for _, t in per_router)
    n = len(times_sorted)
    curve = [(t, (i + 1) / n) for i, t in enumerate(times_sorted)]

    out2 = os.path.join(meta_dir, "convergence_curve.csv")
    with open(out2, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["t_s", "ratio"])
        for t, r in curve:
            w.writerow([f"{t:.6f}", f"{r:.6f}"])

    try:
        import matplotlib.pyplot as plt

        xs = [t for t, _ in curve]
        ys = [r for _, r in curve]
        plt.figure(figsize=(6, 3.5))
        plt.step(xs, ys, where="post")
        plt.xlabel("VT - t_fail (s)")
        plt.ylabel("Converged nodes ratio")
        plt.title(f"Convergence CDF (VT): {target_prefix}")
        plt.grid(True, alpha=0.3)
        plt.tight_layout()
        outpng = os.path.join(meta_dir, "convergence_curve.png")
        plt.savefig(outpng, dpi=200)
    except Exception:
        pass

    print(f"[OK] t_fail_vt={t_fail_vt:.6f} (meta/t_fail_vt.txt)")
    print(f"[OK] wrote {out1}")
    print(f"[OK] wrote {out2}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
