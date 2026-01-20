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


def analyze_bird_log(log_path, router_id, expected_peers):
    """
    分析单个 BIRD 日志，检查 BGP 会话状态。
    
    参数:
        log_path: 日志文件路径
        router_id: 当前路由器 ID
        expected_peers: 期望的邻居列表，如 ['r1', 'r2'] (不包括自己)
    
    返回:
        {
            'established': set(),  # 成功建立的会话
            'closed': set(),       # 被关闭的会话（协议层主动断开）
            'never_established': set(),  # 从未建立的会话
            'desd_disconnect_line': int or None,  # DESD 断开的行号
            'errors': []           # 其他错误信息
        }
    """
    result = {
        'established': set(),
        'closed': set(),
        'never_established': set(expected_peers),
        'desd_disconnect_line': None,
        'errors': []
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
    
    for i, line in enumerate(lines[:analyze_until]):
        # BGP session established
        match = re.search(r'<TRACE> (r\d+): BGP session established', line)
        if match:
            peer = match.group(1)
            peer_states[peer].append((i, 'established'))
            result['established'].add(peer)
            result['never_established'].discard(peer)
            continue
        
        # State changed to up
        match = re.search(r'<TRACE> (r\d+): State changed to up', line)
        if match:
            peer = match.group(1)
            if peer not in result['established']:
                peer_states[peer].append((i, 'up'))
                result['established'].add(peer)
                result['never_established'].discard(peer)
            continue
        
        # BGP session closed (协议层主动断开)
        match = re.search(r'<TRACE> (r\d+): BGP session closed', line)
        if match:
            peer = match.group(1)
            peer_states[peer].append((i, 'closed'))
            result['closed'].add(peer)
            continue
        
        # State changed to down (协议层降级)
        match = re.search(r'<TRACE> (r\d+): State changed to down', line)
        if match:
            peer = match.group(1)
            # 只有在之前是 established/up 的情况下才算 closed
            if peer in result['established']:
                peer_states[peer].append((i, 'down'))
                result['closed'].add(peer)
            continue
        
        # 其他可能的断开信号
        if 'Connection reset' in line or 'Connection closed' in line:
            match = re.search(r'<TRACE> (r\d+):', line)
            if match:
                peer = match.group(1)
                if peer in result['established']:
                    peer_states[peer].append((i, 'connection_error'))
                    result['closed'].add(peer)
    
    return result


def analyze_all_logs(num_routers, log_dir):
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
    
    # 构建期望的 peer 列表
    all_peers = [f"r{i}" for i in range(1, num_routers + 1)]
    
    # 统计
    total_expected = num_routers * (num_routers - 1)  # full-mesh
    total_established = 0
    total_closed = 0
    total_never_established = 0
    
    router_results = {}
    
    for router_id in range(1, num_routers + 1):
        router_name = f"r{router_id}"
        expected_peers = [p for p in all_peers if p != router_name]
        
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
        
        # 被关闭的会话（关键！）
        if result['closed']:
            closed_list = ', '.join(sorted(result['closed']))
            print_fail(f"异常断开: {closed_list}")
            total_closed += len(result['closed'])
            all_pass = False
            issues.append(f"R{router_id}: 会话异常断开 - {closed_list}")
        
        # DESD 断开信息
        if result['desd_disconnect_line']:
            print_info(f"DESD 断开位置: 第 {result['desd_disconnect_line'] + 1} 行 (之后的错误已忽略)")
    
    # 3. 总结
    print_header("3. 测试结果总结")
    
    print(f"\n{Colors.BOLD}会话统计 (Full-Mesh):{Colors.END}")
    print(f"  期望会话数: {total_expected}")
    print(f"  成功建立数: {total_established}")
    print(f"  从未建立数: {total_never_established}")
    print(f"  异常断开数: {total_closed}")
    
    print(f"\n{Colors.BOLD}最终判定:{Colors.END}")
    
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
        print(f"用法: {sys.argv[0]} <num_routers> <log_dir>")
        sys.exit(1)
    
    try:
        num_routers = int(sys.argv[1])
    except ValueError:
        print(f"错误: num_routers 必须是整数，收到: {sys.argv[1]}")
        sys.exit(1)
    
    log_dir = sys.argv[2]
    
    if not os.path.isdir(log_dir):
        print(f"错误: 日志目录不存在: {log_dir}")
        sys.exit(1)
    
    exit_code = analyze_all_logs(num_routers, log_dir)
    sys.exit(exit_code)


if __name__ == "__main__":
    main()
