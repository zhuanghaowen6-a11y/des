#!/usr/bin/env python3
import os
import sys
import csv
import importlib.util
import re


def load_analyze_module(project_root):
    path = os.path.join(project_root, "scripts", "analyze_bgp_logs.py")
    spec = importlib.util.spec_from_file_location("analyze_bgp_logs", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def build_curve_rows(per_router):
    per_router_sorted = sorted(per_router, key=lambda item: (item[1], item[0]))
    total = len(per_router_sorted)
    cumulative_router_ids = []
    curve_rows = []
    idx = 0

    while idx < total:
        t_bucket_str = f"{per_router_sorted[idx][1]:.6f}"
        t_bucket = float(t_bucket_str)
        newly_converged_router_ids = []

        while idx < total and f"{per_router_sorted[idx][1]:.6f}" == t_bucket_str:
            newly_converged_router_ids.append(per_router_sorted[idx][0])
            idx += 1

        cumulative_router_ids.extend(newly_converged_router_ids)
        curve_rows.append(
            (
                t_bucket,
                len(cumulative_router_ids) / total,
                newly_converged_router_ids[:],
                cumulative_router_ids[:],
            )
        )

    return curve_rows


def extract_log_line_epoch(line, abl):
    ts_match = re.match(r'^(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?Z?)\s', line)
    if not ts_match:
        return None
    return abl.parse_rfc3339_timestamp(ts_match.group(1))


def infer_t_fail_epoch(result_dir, tor_id, keep_agg_id, abl):
    log_path = os.path.join(result_dir, "logs", f"bird_r{tor_id}.log")
    if not os.path.exists(log_path):
        return None

    pattern = re.compile(rf"<INFO> Disabling protocol r{keep_agg_id}\b")
    with open(log_path, "r", errors="replace") as f:
        for line in f:
            if not pattern.search(line):
                continue
            return extract_log_line_epoch(line, abl)

    return None


def main():
    if len(sys.argv) < 3:
        print("Usage: compute_convergence_curve.py <RESULT_DIR> <target_prefix> [topology_mode] [num_routers] [tor_id] [keep_agg_id]")
        print("Example: compute_convergence_curve.py results/... 192.168.41.0/24 fat-tree-k8-64 64 41 17")
        return 2

    result_dir = sys.argv[1]
    target_prefix = sys.argv[2]
    topology_mode = (sys.argv[3] if len(sys.argv) >= 4 else os.environ.get("TOPOLOGY_MODE") or "fat-tree-k8-64").lower()
    num_routers = int(sys.argv[4]) if len(sys.argv) >= 5 else int(os.environ.get("NUM_ROUTERS", "64"))
    tor_id = int(sys.argv[5]) if len(sys.argv) >= 6 else int(os.environ.get("TOR_ID", "41"))
    keep_agg_id = int(sys.argv[6]) if len(sys.argv) >= 7 else int(os.environ.get("KEEP_AGG_ID", "17"))

    log_dir = os.path.join(result_dir, "logs")
    meta_dir = os.path.join(result_dir, "meta")
    t_fail_file = os.path.join(meta_dir, "t_fail_epoch.txt")
    effective_t_fail_file = os.path.join(meta_dir, "t_fail_epoch_effective.txt")

    if not os.path.isdir(log_dir):
        print(f"[ERROR] log dir not found: {log_dir}")
        return 1
    if not os.path.exists(t_fail_file):
        print(f"[ERROR] t_fail file not found: {t_fail_file}")
        return 1

    project_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    abl = load_analyze_module(project_root)

    inferred_t_fail_epoch = infer_t_fail_epoch(result_dir, tor_id, keep_agg_id, abl)
    if inferred_t_fail_epoch is not None:
        os.makedirs(meta_dir, exist_ok=True)
        with open(effective_t_fail_file, "w") as f:
            f.write(f"{inferred_t_fail_epoch:.9f}\n")
        t0_file = effective_t_fail_file
        print(f"[OK] inferred t_fail_epoch={inferred_t_fail_epoch:.9f} from logs/bird_r{tor_id}.log")
    else:
        t0_file = t_fail_file
        print(f"[WARN] failed to infer t_fail from logs/bird_r{tor_id}.log, falling back to {t_fail_file}")

    # Use wallclock mode, and set t0 to t_fail to get relative time since fault.
    os.environ["TIME_MODE"] = "wallclock"
    os.environ["T0_FILE"] = t0_file
    abl.TIME_MODE = "wallclock"
    abl.T0_FILE = t0_file
    abl.load_t0()

    per_router = []  # (router_id, t_converged_s)
    for rid in range(1, num_routers + 1):
        log_path = os.path.join(log_dir, f"bird_r{rid}.log")
        r = abl.analyze_route_convergence(log_path, rid, topology_mode=topology_mode)

        times = [t for (t, p, _a) in r["route_updates"] if p == target_prefix and t >= 0.0]
        t_i = max(times) if times else 0.0
        per_router.append((rid, t_i))

    os.makedirs(meta_dir, exist_ok=True)

    out1 = os.path.join(meta_dir, "per_router_convergence.csv")
    with open(out1, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["router_id", "t_converged_s"])
        for rid, t in per_router:
            w.writerow([rid, f"{t:.6f}"])

    curve = build_curve_rows(per_router)

    out2 = os.path.join(meta_dir, "convergence_curve.csv")
    with open(out2, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow([
            "t_s",
            "ratio",
            "newly_converged_count",
            "newly_converged_router_ids",
            "cumulative_converged_count",
            "cumulative_converged_router_ids",
        ])
        for t, r, new_ids, cumulative_ids in curve:
            w.writerow([
                f"{t:.6f}",
                f"{r:.6f}",
                len(new_ids),
                ";".join(f"r{rid}" for rid in new_ids),
                len(cumulative_ids),
                ";".join(f"r{rid}" for rid in cumulative_ids),
            ])

    # Optional plot
    try:
        import matplotlib.pyplot as plt

        xs = [t for t, _, _, _ in curve]
        ys = [r for _, r, _, _ in curve]
        plt.figure(figsize=(6, 3.5))
        plt.step(xs, ys, where="post")
        plt.xlabel("t - t_fail (s)")
        plt.ylabel("Converged nodes ratio")
        plt.title(f"Convergence CDF: {target_prefix}")
        plt.grid(True, alpha=0.3)
        plt.tight_layout()
        outpng = os.path.join(meta_dir, "convergence_curve.png")
        plt.savefig(outpng, dpi=200)
    except Exception:
        pass

    print(f"[OK] wrote {out1}")
    print(f"[OK] wrote {out2}")
    print("[OK] done")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
