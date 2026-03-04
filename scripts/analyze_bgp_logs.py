#!/usr/bin/env python3
"""
BGP 日志分析脚本

分析 DESD 仿真的日志，判断 BGP 会话是否在整个仿真期间保持健康：
1. DESD 是否正常退出（达到事件上限）
2. 所有 BGP 会话是否成功建立
3. 在 DESD 退出前，是否有任何 BGP 会话被协议层主动断开

用法:
    python3 analyze_bgp_logs.py <num_routers> <log_dir>

示例:
    python3 analyze_bgp_logs.py 3 logs
"""

import sys
import os
import re
from collections import defaultdict

# 颜色输出
class Colors:
    GREEN = '\033[92m'
    RED = '\033[91m'
    YELLOW = '\033[93m'
    BLUE = '\033[94m'
    BOLD = '\033[1m'
    END = '\033[0m'

def print_ok(msg):
    print(f"{Colors.GREEN}✓ {msg}{Colors.END}")

def print_fail(msg):
    print(f"{Colors.RED}✗ {msg}{Colors.END}")

def print_warn(msg):
    print(f"{Colors.YELLOW}⚠ {msg}{Colors.END}")

def print_info(msg):
    print(f"{Colors.BLUE}ℹ {msg}{Colors.END}")

def print_header(msg):
    print(f"\n{Colors.BOLD}{'='*60}{Colors.END}")
    print(f"{Colors.BOLD} {msg}{Colors.END}")
    print(f"{Colors.BOLD}{'='*60}{Colors.END}")


def analyze_desd_log(log_path):
    """
    分析 DESD 日志，判断是否正常退出。
    
    返回: (success: bool, reason: str, final_vt: str, event_count: str)
    """
    if not os.path.exists(log_path):
        return False, f"DESD 日志不存在: {log_path}", "N/A", "0"
    
    with open(log_path, 'r', errors='replace') as f:
        content = f.read()
    
    # 查找退出日志
    exit_match = re.search(r'\[DESD-EXIT\] Reason: (.+?)\. VT=([0-9.]+), ProcessedEvents=(\d+)\. Code=(\d+)', content)
    
    if not exit_match:
        # 没有找到正常退出日志，检查是否还在运行或崩溃
        if '[DESD-STOP]' in content:
            return False, "DESD 停止但未找到完整退出日志", "N/A", "0"
        return False, "DESD 日志中未找到退出信息，可能仍在运行或异常终止", "N/A", "0"
    
    reason = exit_match.group(1)
    final_vt = exit_match.group(2)
    processed_events = exit_match.group(3)
    exit_code = exit_match.group(4)
    
    if exit_code == "0" and "Event limit reached" in reason:
        return True, f"DESD 正常退出：{reason}", final_vt, processed_events
    else:
        return False, f"DESD 异常退出 (code={exit_code}): {reason}", final_vt, processed_events


# Flap 阈值：超过此次数的 close/down 视为抖动严重
FLAP_THRESHOLD = 3

def analyze_route_convergence(log_path, router_id, stability_window=0.0):
    """
    分析单个 BIRD 日志，提取路由收敛相关信息。
    
    返回:
        {
            'last_best_change_vt': float,  # 最后一次 [best] 路由变更的 VT
            'last_update_vt': float,        # 最后一次路由更新事件的 VT
            'best_changes': list,           # [(vt, prefix, action)] 所有 best 路由变更
            'route_updates': list,          # [(vt, prefix, action)] 所有路由更新
        }
    """
    result = {
        'last_best_change_vt': 0.0,
        'last_update_vt': 0.0,
        'best_changes': [],
        'route_updates': [],
    }
    
    if not os.path.exists(log_path):
        return result
    
    with open(log_path, 'r', errors='replace') as f:
        lines = f.readlines()
    
    # ToR 前缀模式: 192.168.X.0/24 (X = 28..45)
    tor_prefix_pattern = re.compile(r'192\.168\.(2[89]|3[0-9]|4[0-5])\.0/24')
    
    for line in lines:
        # 提取 VT
        vt_match = re.search(r'\[VT=([\d.]+)\]', line)
        if not vt_match:
            continue
        vt = float(vt_match.group(1))
        
        # 匹配 [best] 路由变更: "r*.ipv4 > added [best]" 或 "r*.ipv4 > replaced [best]"
        # 格式: <TRACE> rX.ipv4 > added [best] 192.168.Y.0/24 ...
        best_match = re.search(r'<TRACE> (r\d+|static4)\.ipv4 > (added|replaced) \[best\] (\S+)', line)
        if best_match:
            protocol = best_match.group(1)
            action = best_match.group(2)
            prefix = best_match.group(3)
            # 只关注 ToR 前缀
            if tor_prefix_pattern.match(prefix):
                result['best_changes'].append((vt, prefix, action))
                if vt > result['last_best_change_vt']:
                    result['last_best_change_vt'] = vt
        
        # 匹配所有路由更新事件 (包括 added, replaced, removed, withdraw 等)
        # 格式: <TRACE> rX.ipv4 > ... 或 <TRACE> rX.ipv4 < ...
        update_match = re.search(r'<TRACE> (r\d+|static4)\.ipv4 [><] (added|replaced|removed|idempotent withdraw)', line)
        if update_match:
            # 提取前缀
            prefix_match = re.search(r'(\d+\.\d+\.\d+\.\d+/\d+)', line)
            if prefix_match:
                prefix = prefix_match.group(1)
                if tor_prefix_pattern.match(prefix):
                    action = update_match.group(2)
                    result['route_updates'].append((vt, prefix, action))
                    if vt > result['last_update_vt']:
                        result['last_update_vt'] = vt
    
    return result


def analyze_bird_log(log_path, router_id, expected_peers):
    """
    分析单个 BIRD 日志，检查 BGP 会话状态。
    
    采用两段式逻辑：
    1. 第一段：收集每个 peer 的状态时间线
    2. 第二段：根据最终状态判定会话健康性
    
    判定规则：
    - 规则1：从未建立 → never_established
    - 规则2：曾经建立，最终状态是 up/established → healthy（不计入 closed）
    - 规则3：曾经建立，最终状态是 closed/down → 异常断开
    - 规则4：flap 次数过多（>= FLAP_THRESHOLD）→ 给 warning
    
    参数:
        log_path: 日志文件路径
        router_id: 当前路由器 ID
        expected_peers: 期望的邻居列表，如 ['r1', 'r2'] (不包括自己)
    
    返回:
        {
            'established': set(),  # 最终状态为 up/established 的会话
            'closed': set(),       # 最终状态为 closed/down 的会话（真正异常断开）
            'never_established': set(),  # 从未建立的会话
            'flapping': dict(),    # 抖动严重的会话 {peer: close_count}
            'desd_disconnect_line': int or None,  # DESD 断开的行号
            'errors': [],          # 其他错误信息
            'session_vt': dict(),  # 会话建立时的VT {peer: vt}
            'max_session_vt': float  # 最大会话建立VT (T_session)
        }
    """
    result = {
        'established': set(),
        'closed': set(),
        'never_established': set(expected_peers),
        'flapping': {},  # {peer: close_count}
        'desd_disconnect_line': None,
        'errors': [],
        'session_vt': {},  # 会话建立时的VT
        'max_session_vt': 0.0
    }
    
    if not os.path.exists(log_path):
        result['errors'].append(f"日志文件不存在: {log_path}")
        return result
    
    with open(log_path, 'r', errors='replace') as f:
        lines = f.readlines()
    
    # 找到 DESD 断开连接的位置（这之后的错误应该忽略）
    desd_disconnect_line = None
    for i, line in enumerate(lines):
        if 'DESD disconnected' in line or 'poll in' in line and 'Input/output error' in line:
            desd_disconnect_line = i
            break
    
    result['desd_disconnect_line'] = desd_disconnect_line
    
    # 只分析 DESD 断开前的内容
    analyze_until = desd_disconnect_line if desd_disconnect_line else len(lines)
    
    # 跟踪每个 peer 的状态变化
    peer_states = defaultdict(list)  # peer -> [(line_num, state)]
    
    # Helper: 从行中提取 VT 标签 [VT=x.xxx]
    def extract_vt(line):
        vt_match = re.search(r'\[VT=([\d.]+)\]', line)
        if vt_match:
            return float(vt_match.group(1))
        return None
    
    # ========== 第一段：收集状态时间线 ==========
    for i, line in enumerate(lines[:analyze_until]):
        # BGP session established
        match = re.search(r'<TRACE> (r\d+): BGP session established', line)
        if match:
            peer = match.group(1)
            peer_states[peer].append((i, 'established'))
            # 提取 VT 用于收敛时间计算
            vt = extract_vt(line)
            if vt is not None:
                result['session_vt'][peer] = vt
                if vt > result['max_session_vt']:
                    result['max_session_vt'] = vt
            continue
        
        # State changed to up
        match = re.search(r'<TRACE> (r\d+): State changed to up', line)
        if match:
            peer = match.group(1)
            peer_states[peer].append((i, 'up'))
            continue
        
        # BGP session closed (协议层主动断开)
        match = re.search(r'<TRACE> (r\d+): BGP session closed', line)
        if match:
            peer = match.group(1)
            peer_states[peer].append((i, 'closed'))
            continue
        
        # State changed to down (协议层降级)
        match = re.search(r'<TRACE> (r\d+): State changed to down', line)
        if match:
            peer = match.group(1)
            peer_states[peer].append((i, 'down'))
            continue
        
        # 其他可能的断开信号
        if 'Connection reset' in line or 'Connection closed' in line:
            match = re.search(r'<TRACE> (r\d+):', line)
            if match:
                peer = match.group(1)
                peer_states[peer].append((i, 'connection_error'))
    
    # ========== 第二段：根据时间线判定最终状态 ==========
    for peer in expected_peers:
        states = peer_states.get(peer, [])
        
        if not states:
            # 规则1：从未有任何状态记录 → never_established
            # (已经在初始化时加入 never_established)
            continue
        
        # 按行号排序（虽然理论上已经是顺序的）
        states.sort(key=lambda x: x[0])
        
        # 统计
        ever_established = any(s[1] in ('established', 'up') for s in states)
        close_count = sum(1 for s in states if s[1] in ('closed', 'down', 'connection_error'))
        
        if not ever_established:
            # 从未成功建立过（只有 close/down/connection_error 记录，无 established/up）
            # 保持在 never_established
            continue
        
        # 曾经建立过
        result['never_established'].discard(peer)
        
        # 规则4：检测 flap（仍然统计 connection_error）
        if close_count >= FLAP_THRESHOLD:
            result['flapping'][peer] = close_count
        
        # 规则2 & 规则3：根据最终 BGP 状态判定
        # 只使用明确的 BGP 状态（established/up/closed/down）来决定最终是否异常断开，
        # 避免单纯因为 Connection closed/reset（connection_error）就判定为异常。
        bgp_states = [s for s in states if s[1] in ('established', 'up', 'closed', 'down')]
        if not bgp_states:
            # 理论上 ever_established=True 时一定会有至少一个 established/up；
            # 为稳妥起见，如果不存在明确的 BGP 状态，则只认为其曾经建立过，不判为异常断开。
            result['established'].add(peer)
            continue
        
        last_bgp_state = bgp_states[-1][1]
        if last_bgp_state in ('established', 'up'):
            # 规则2：最终状态是 up/established → healthy
            result['established'].add(peer)
            # 不加入 closed，即使期间有过 close/connection_error
        else:
            # 规则3：最终 BGP 状态是 closed/down → 异常断开
            result['established'].add(peer)  # 仍然记录曾经建立过
            result['closed'].add(peer)
    
    return result


def get_fat_tree_k6_neighbors(router_id):
    """获取 fat-tree k=6 拓扑中某路由器的邻居列表。
    
    Fat-tree k=6 拓扑（45 台路由器）:
      Core:  R1..R9   (9 台，分为 3 组，每组 3 台)
      Agg:   R10..R27 (18 台，6 个 pod，每 pod 3 台)
      ToR:   R28..R45 (18 台，6 个 pod，每 pod 3 台)
    
    编号规则:
      core_id(group=g, idx=x) = 1 + g*3 + x  (g,x in 0..2)
      agg_id(pod=p, a=0..2)   = 10 + p*3 + a (p in 0..5)
      tor_id(pod=p, e=0..2)   = 28 + p*3 + e (p in 0..5)
    
    连接规则:
      Pod 内: ToR(e) <-> 该 pod 的全部 Agg(0..2)
      Pod 上行: Agg(a) <-> Core group=a 的全部 3 台
    """
    neighbors = []
    
    if router_id <= 9:
        # Core router: R1..R9
        # core_id = 1 + group*3 + idx => group = (router_id - 1) // 3
        group = (router_id - 1) // 3
        # Core(group) connects to Agg(a=group) in each pod
        # agg_id(pod=p, a=group) = 10 + p*3 + group
        for pod in range(6):
            agg_id = 10 + pod * 3 + group
            neighbors.append(agg_id)
    
    elif router_id <= 27:
        # Aggregation router: R10..R27
        # agg_id = 10 + pod*3 + a => pod = (router_id - 10) // 3, a = (router_id - 10) % 3
        pod = (router_id - 10) // 3
        a = (router_id - 10) % 3
        
        # Connect to Core group=a (all 3 cores in that group)
        for idx in range(3):
            core_id = 1 + a * 3 + idx
            neighbors.append(core_id)
        
        # Connect to all ToRs in the same pod
        for e in range(3):
            tor_id = 28 + pod * 3 + e
            neighbors.append(tor_id)
    
    else:
        # ToR router: R28..R45
        # tor_id = 28 + pod*3 + e => pod = (router_id - 28) // 3
        pod = (router_id - 28) // 3
        
        # Connect to all Aggs in the same pod
        for a in range(3):
            agg_id = 10 + pod * 3 + a
            neighbors.append(agg_id)
    
    return neighbors


def build_expected_peers(num_routers, topology_mode):
    """根据拓扑模式构造每个路由器的期望邻居列表。

    拓扑模式支持：
    - 'full-mesh': 每个路由器与其他所有路由器建立会话
    - 'ring': 每个路由器只与前后两个邻居建立会话（环形），NUM_ROUTERS=2 时只有一个邻居
    - 'fat-tree-k6': fat-tree k=6 拓扑（45 台路由器）
    """

    peers_per_router = {}

    if topology_mode == "ring":
        for router_id in range(1, num_routers + 1):
            left = router_id - 1
            right = router_id + 1
            if left < 1:
                left = num_routers
            if right > num_routers:
                right = 1

            if left == right:
                neighbors = [f"r{left}"]
            else:
                neighbors = [f"r{left}", f"r{right}"]

            peers_per_router[router_id] = neighbors

    elif topology_mode == "fat-tree-k6":
        # Fat-tree k=6: 45 routers
        for router_id in range(1, 46):
            neighbor_ids = get_fat_tree_k6_neighbors(router_id)
            peers_per_router[router_id] = [f"r{n}" for n in neighbor_ids]

    else:  # 默认 full-mesh
        all_peers = [f"r{i}" for i in range(1, num_routers + 1)]
        for router_id in range(1, num_routers + 1):
            router_name = f"r{router_id}"
            peers_per_router[router_id] = [p for p in all_peers if p != router_name]

    return peers_per_router


def analyze_all_logs(num_routers, log_dir, topology_mode="full-mesh"):
    """
    分析所有日志，综合判断测试结果。
    """
    print_header("BGP 日志分析报告")
    
    all_pass = True
    issues = []
    
    # 1. 分析 DESD 日志
    print_header("1. DESD 状态检查")
    desd_log = os.path.join(log_dir, f"desd_n{num_routers}.log")
    desd_ok, desd_reason, final_vt, event_count = analyze_desd_log(desd_log)
    
    if desd_ok:
        print_ok(desd_reason)
        print_info(f"最终虚拟时间: {final_vt}s, 处理事件数: {event_count}")
    else:
        print_fail(desd_reason)
        all_pass = False
        issues.append(f"DESD: {desd_reason}")
    
    # 2. 分析 BIRD 日志
    print_header("2. BGP 会话状态检查")

    topology_mode = topology_mode or "full-mesh"
    print_info(f"拓扑模式: {topology_mode}")

    # 构建期望的 peer 列表（支持 full-mesh / ring）
    peers_per_router = build_expected_peers(num_routers, topology_mode)

    # 统计
    if topology_mode == "ring":
        # 每条会话在两个方向都会被统计一次，这里保持和 full-mesh 一致：按有向会话计数
        if num_routers == 2:
            total_expected = 2  # r1<->r2，各算一次
        else:
            total_expected = num_routers * 2  # 每个路由器有两个邻居
    elif topology_mode == "fat-tree-k6":
        # Fat-tree k=6: Core有6邻居, Agg有6邻居, ToR有3邻居
        # Core: 9 * 6 = 54, Agg: 18 * 6 = 108, ToR: 18 * 3 = 54
        # 总有向会话数 = 54 + 108 + 54 = 216
        total_expected = 9 * 6 + 18 * 6 + 18 * 3  # 216
    else:
        total_expected = num_routers * (num_routers - 1)  # full-mesh
    total_established = 0
    total_closed = 0
    total_never_established = 0
    total_flapping = 0
    
    router_results = {}
    flapping_warnings = []  # 用于最终汇总
    
    # 路由收敛统计
    route_convergence_results = {}
    
    for router_id in range(1, num_routers + 1):
        expected_peers = peers_per_router.get(router_id, [])
        
        # 尝试多个可能的日志路径
        possible_paths = [
            os.path.join(log_dir, f"bird_r{router_id}.log"),
            os.path.join(log_dir, "container_bird_logs", f"bird_r{router_id}.log"),
        ]
        
        log_path = None
        for p in possible_paths:
            if os.path.exists(p):
                log_path = p
                break
        
        if not log_path:
            print_fail(f"R{router_id}: 未找到日志文件")
            all_pass = False
            issues.append(f"R{router_id}: 日志文件缺失")
            continue
        
        result = analyze_bird_log(log_path, router_id, expected_peers)
        router_results[router_id] = result
        
        # 分析路由收敛
        route_result = analyze_route_convergence(log_path, router_id)
        route_convergence_results[router_id] = route_result
        
        # 报告
        print(f"\n{Colors.BOLD}R{router_id} ({log_path}):{Colors.END}")
        
        if result['errors']:
            for err in result['errors']:
                print_fail(err)
                issues.append(f"R{router_id}: {err}")
                all_pass = False
        
        # 建立的会话
        if result['established']:
            established_list = ', '.join(sorted(result['established']))
            print_ok(f"已建立会话: {established_list}")
            total_established += len(result['established'])
        
        # 从未建立的会话
        if result['never_established']:
            never_list = ', '.join(sorted(result['never_established']))
            print_fail(f"从未建立: {never_list}")
            total_never_established += len(result['never_established'])
            all_pass = False
            issues.append(f"R{router_id}: 会话从未建立 - {never_list}")
        
        # 被关闭的会话（关键！最终状态为 down/closed 才算异常）
        if result['closed']:
            closed_list = ', '.join(sorted(result['closed']))
            print_fail(f"异常断开: {closed_list}")
            total_closed += len(result['closed'])
            all_pass = False
            issues.append(f"R{router_id}: 会话异常断开 - {closed_list}")
        
        # 抖动严重的会话（warning，不影响 PASS/FAIL）
        if result['flapping']:
            for peer, count in sorted(result['flapping'].items()):
                print_warn(f"会话抖动: {peer} (close/down {count} 次，最终状态: {'up' if peer not in result['closed'] else 'down'})")
                flapping_warnings.append(f"R{router_id}-{peer}: {count} 次 close/down")
            total_flapping += len(result['flapping'])
        
        # DESD 断开信息
        if result['desd_disconnect_line']:
            print_info(f"DESD 断开位置: 第 {result['desd_disconnect_line'] + 1} 行 (之后的错误已忽略)")
    
    # 3. 总结
    print_header("3. 测试结果总结")
    
    topo_labels = {"full-mesh": "Full-Mesh", "ring": "Ring", "fat-tree-k6": "Fat-Tree k=6"}
    topo_label = topo_labels.get(topology_mode, topology_mode)
    print(f"\n{Colors.BOLD}会话统计 ({topo_label}):{Colors.END}")
    print(f"  期望会话数: {total_expected}")
    print(f"  成功建立数: {total_established}")
    print(f"  从未建立数: {total_never_established}")
    print(f"  异常断开数: {total_closed} (最终状态为 down)")
    print(f"  抖动会话数: {total_flapping} (>= {FLAP_THRESHOLD} 次 close/down)")
    
    # 计算并报告收敛时间 T_session
    global_max_session_vt = 0.0
    sessions_with_vt = 0
    for router_id, result in router_results.items():
        if result['max_session_vt'] > 0:
            sessions_with_vt += len(result['session_vt'])
            if result['max_session_vt'] > global_max_session_vt:
                global_max_session_vt = result['max_session_vt']
    
    # 计算 T_route_rib 和 T_update_quiescence
    global_max_best_change_vt = 0.0
    global_max_update_vt = 0.0
    total_best_changes = 0
    total_route_updates = 0
    
    for router_id, route_result in route_convergence_results.items():
        if route_result['last_best_change_vt'] > global_max_best_change_vt:
            global_max_best_change_vt = route_result['last_best_change_vt']
        if route_result['last_update_vt'] > global_max_update_vt:
            global_max_update_vt = route_result['last_update_vt']
        total_best_changes += len(route_result['best_changes'])
        total_route_updates += len(route_result['route_updates'])
    
    print(f"\n{Colors.BOLD}收敛时间 (基于 VT 标签):{Colors.END}")
    if global_max_session_vt > 0:
        print(f"  T_session (最后一个会话建立): {Colors.GREEN}{global_max_session_vt:.3f}s VT{Colors.END}")
        print(f"  带 VT 标签的会话数: {sessions_with_vt}/{total_established}")
    else:
        print(f"  {Colors.YELLOW}未检测到 VT 标签 (确保 DES_LOG_VT_PREFIX=1){Colors.END}")
    
    # 输出路由收敛时间
    print(f"\n{Colors.BOLD}路由收敛时间 (基于 VT 标签):{Colors.END}")
    if global_max_best_change_vt > 0:
        print(f"  T_route_rib (最后一次 best 路由变更): {Colors.GREEN}{global_max_best_change_vt:.3f}s VT{Colors.END}")
        print(f"  检测到的 [best] 路由变更数: {total_best_changes}")
    else:
        print(f"  {Colors.YELLOW}T_route_rib: 未检测到 ToR 前缀的 [best] 路由变更{Colors.END}")
    
    if global_max_update_vt > 0:
        print(f"  T_update_quiescence (最后一次路由更新): {Colors.GREEN}{global_max_update_vt:.3f}s VT{Colors.END}")
        print(f"  检测到的路由更新事件数: {total_route_updates}")
    else:
        print(f"  {Colors.YELLOW}T_update_quiescence: 未检测到 ToR 前缀的路由更新{Colors.END}")
    
    print(f"\n{Colors.BOLD}最终判定:{Colors.END}")
    
    # 显示 flapping warnings（不影响 PASS/FAIL）
    if flapping_warnings:
        print(f"\n{Colors.YELLOW}{Colors.BOLD}⚠ 会话抖动警告 (不影响测试结果):{Colors.END}")
        for warn in flapping_warnings:
            print(f"{Colors.YELLOW}  - {warn}{Colors.END}")
    
    if all_pass:
        print(f"\n{Colors.GREEN}{Colors.BOLD}{'='*60}{Colors.END}")
        print(f"{Colors.GREEN}{Colors.BOLD} ✓ PASS: 所有 BGP 会话在 DESD 运行期间保持健康{Colors.END}")
        print(f"{Colors.GREEN}{Colors.BOLD}{'='*60}{Colors.END}\n")
        return 0
    else:
        print(f"\n{Colors.RED}{Colors.BOLD}{'='*60}{Colors.END}")
        print(f"{Colors.RED}{Colors.BOLD} ✗ FAIL: 检测到以下问题{Colors.END}")
        print(f"{Colors.RED}{Colors.BOLD}{'='*60}{Colors.END}")
        for issue in issues:
            print(f"{Colors.RED}  - {issue}{Colors.END}")
        print()
        return 1


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        print(f"用法: {sys.argv[0]} <num_routers> <log_dir> [topology_mode]")
        sys.exit(1)
    
    try:
        num_routers = int(sys.argv[1])
    except ValueError:
        print(f"错误: num_routers 必须是整数，收到: {sys.argv[1]}")
        sys.exit(1)
    
    log_dir = sys.argv[2]

    # 拓扑模式：可通过第 3 个参数或环境变量 TOPOLOGY_MODE 指定
    topo_from_arg = sys.argv[3] if len(sys.argv) >= 4 else None
    topo_from_env = os.environ.get("TOPOLOGY_MODE")
    topology_mode = (topo_from_arg or topo_from_env or "full-mesh").lower()
    if topology_mode not in ("full-mesh", "ring", "fat-tree-k6"):
        print(f"错误: 不支持的拓扑模式: {topology_mode} (期望: full-mesh, ring 或 fat-tree-k6)")
        sys.exit(1)
    
    if not os.path.isdir(log_dir):
        print(f"错误: 日志目录不存在: {log_dir}")
        sys.exit(1)
    
    exit_code = analyze_all_logs(num_routers, log_dir, topology_mode=topology_mode)
    sys.exit(exit_code)


if __name__ == "__main__":
    main()
