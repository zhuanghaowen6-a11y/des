# AI Context: DES + BGP Convergence Time Analysis

本文档用于在 **更换对话上下文/丢失记忆** 时，让 AI 能在最短时间内理解本项目在做什么、当前进度与当前问题，从而继续协作开发与实验。

## 1. 项目目标（一句话）

本项目实现并维护一个 **DES（Distributed Event Simulator，分布式事件模拟器）** 框架，用于在 Docker 容器中运行真实路由器软件（例如 BIRD），并通过 **虚拟时间（VT, Virtual Time）** 控制和重放网络事件，从而对控制面/路由面的收敛行为进行可重复、可控的测量与对比。

当前研究主线是：在 **fat-tree-k6（45 路由器）** 与 **fat-tree-k8-64（64 路由器，非标准）** 等拓扑下测量 BGP 收敛时间，并在不同运行环境中对比：

- DES（VT 标签）
- container-only（无 DES，wall-clock 时间）
- ns-3（不使用 DCE，使用协议模型；仍在设计阶段）

## 2. 仓库与关键路径

- **项目根目录**：`/home/hwzhuang/hwzhuang/desTest/des_design`

核心目录：

- `src/`
  - `desd.c`：DESD（事件模拟守护进程），负责事件队列、线程阻塞/唤醒、虚拟网络收发等
  - `libdeshook.c`：通过 `LD_PRELOAD` hook 系统调用，把路由器进程的 socket/时间等行为接入 DESD
  - `common.c/.h`：与 DESD/libdeshook 共享的数据结构与工具
- `scripts/`
  - `test_n_bird_docker.sh`：DES 模式下的大规模实验驱动（生成拓扑+配置，启动 DESD+路由器容器，收集日志，调用分析脚本）
  - `test_container_only.sh`：container-only 实验驱动（不使用 DES；使用 `docker logs -t` 的 wall-clock 时间戳）
  - `analyze_bgp_logs.py`：离线分析脚本；计算三种收敛时间指标；支持 VT 与 wall-clock 两种时间模式
- `results/`
  - 实验输出目录（container-only 的结果会写到 `results/container_only/...`）

## 3. DES 框架（必须理解的最小模型）

### 3.1 DES 的基本思想

- 路由器（BIRD）运行在 Docker 容器内，属于真实用户态进程。
- 通过 `LD_PRELOAD=/usr/local/lib/libdeshook.so` 注入 hook：
  - 拦截 socket API（connect/accept/send/recv/select/poll 等）
  - 拦截时间 API（如 `clock_gettime` 等，具体以实现为准）
- 路由器的网络与时间行为不直接由内核推进，而是转为与 DESD 的 RPC/消息交互。
- DESD 维护全局事件队列，推进 **虚拟时间 VT**，并决定何时唤醒哪个线程继续执行。

### 3.2 日志与 VT 标签

- DES 模式下，BIRD 输出日志会带 `VT` 前缀（由 hook/环境变量控制）。
- 典型格式：

```
[VT=5.255] bird: <TRACE> r12: BGP session established
```

分析脚本在 VT 模式下依赖 `\[VT=...\]` 提取相对时间。

## 4. BGP 收敛时间三指标（本项目当前的核心输出）

本项目当前输出三类时间，统一以相对起点 `t0` 表示（VT 模式下直接使用 VT；wall-clock 模式下为 epoch 减 `t0_epoch`）。

### 4.1 `T_session`

- 定义：**最后一个 BGP session 建立的时间**（全局最大）
- 在 BIRD 日志中通过事件识别：
  - `<TRACE> (r\d+): BGP session established`

### 4.2 `T_route_rib`

- 定义：**最后一次 ToR 前缀的 best 路由变更时间**（全局最大）
- 事件识别：
  - `<TRACE> (r\d+|static4)\.ipv4 > (added|replaced) \[best\] PREFIX`
- 只关注 ToR 前缀：`192.168.28.0/24` 到 `192.168.45.0/24`

### 4.3 `T_update_quiescence`

- 定义：**最后一次 ToR 前缀的路由更新事件时间**（全局最大）
- 事件识别：
  - `<TRACE> (r\d+|static4)\.ipv4 [><] (added|replaced|removed|idempotent withdraw)`

### 4.4 分析脚本位置

- 实现文件：`scripts/analyze_bgp_logs.py`
- 会话收敛：`analyze_bird_log()`
- 路由收敛：`analyze_route_convergence()`
- 全局汇总：`analyze_all_logs()`

## 5. 拓扑与实验规模

### 5.1 fat-tree-k6

- 路由器数量：`45`
  - Core：`R1-R9`
  - Agg：`R10-R27`
  - ToR：`R28-R45`
- ToR 前缀：每个 ToR 通过 `static4` 起源：`192.168.<router_id>.0/24`

### 5.2 fat-tree-k8-64（非标准）

- 路由器数量：`64`
  - Core：`R1-R16`
  - Agg：`R17-R40`（6 pod × 4）
  - ToR：`R41-R64`（6 pod × 4）
- ToR 前缀：每个 ToR 通过 `static4` 起源：`192.168.<router_id>.0/24`（即 `192.168.41.0/24` 到 `192.168.64.0/24`）
- 邻居规则：
  - Core：每台 core 连接到每个 pod 的某一个 agg（6 邻居）
  - Agg：连接同组的 4 个 core + 同 pod 的 4 个 ToR（8 邻居）
  - ToR：连接同 pod 的 4 个 agg（4 邻居）
- 期望有向会话数：`384`

## 6. container-only 模式（无 DES）

### 6.1 为什么需要 container-only

用于对比：

- DES 的“仿真时间”（VT）结果
- 真实系统开销下的 wall-clock 收敛时间（包含调度、IO、docker、真实 TCP 等）

### 6.2 时间戳来源

- 使用 `docker logs -t` 获取 RFC3339 时间戳，典型：

```
2026-03-04T11:49:45.858143907Z bird: <TRACE> ...
```

`analyze_bgp_logs.py` 在 `TIME_MODE=wallclock` 下解析该时间戳并转换为 epoch 秒。

### 6.3 当前关键问题：t0 定义导致“容器创建时间”污染

早期 container-only 的 `t0` 记录在容器创建之前，导致最终 `T_session` 可能包含“45 个容器串行启动耗时”。

### 6.4 当前进展：已实现方案 2A（更准确的 t0）

已在 `scripts/test_container_only.sh` 实现如下流程：

1. 创建 docker network
2. 创建全部容器，但容器内先运行等待进程（`tail -f /dev/null`）
3. 所有容器创建完毕后记录 `t0_epoch.txt`
4. 并行 `docker exec` 启动 BIRD，并将 BIRD 的 stdout/stderr 重定向到 `/proc/1/fd/1`、`/proc/1/fd/2`，以便 `docker logs -t` 仍可采集
5. sleep 固定时长后收集日志并分析

注意：该脚本曾出现一次 `unexpected end of file` 的语法错误，已修复（发生在 `for pid in ...; do ... done` 后的换行/结构问题）。

## 7. 实验对比现象（已观测）

### 7.1 fat-tree-k6

同一拓扑下，曾观测到：

- DES（VT）：`T_session ≈ 5.255s VT`，`T_route_rib ≈ 5.689s VT`，`T_update_quiescence ≈ 5.794s VT`
- container-only（wall-clock）：`T_session ≈ 74s`（显著更大）

解释要点：

- VT 表示仿真时间，不包含真实 CPU/IO/docker 开销。
- wall-clock 包含真实执行开销。
- container-only 的 `t0` 若定义过早会把容器创建串行耗时算入收敛时间。

### 7.2 fat-tree-k8-64（非标准，64 路由器）

在 container-only 模式（方案 2A，准确 `t0`）下，已成功运行并观测到：

- `T_session ≈ 32.724s`
- `T_route_rib ≈ 32.745s`
- `T_update_quiescence ≈ 32.750s`
- 所有 384 个有向 BGP 会话均成功建立，无异常断开或抖动。

说明：采用先创建全部容器、记录 `t0`、再并行 `docker exec` 启动 BIRD 的方案后，收敛时间进入协议合理时间尺度，不再被容器创建串行耗时污染。

## 8. ns-3 方向（不使用 DCE）

- 当前阶段：**设计中**，尚未实现。
- 目标：在 ns-3 中用协议模型实现/简化 BGP（可控 trace 点），输出事件日志（JSONL/CSV），复用现有离线分析逻辑计算同样三指标。

## 9. 当前状态与下一步（面向继续协作）

### 9.1 当前状态

- DES（VT）测量链路已跑通（支持 fat-tree-k6 与 fat-tree-k8-64）。
- container-only（wall-clock）测量链路已跑通，方案 2A（准确 `t0`）已实现并在 fat-tree-k8-64（64 路由器）上成功验证收敛时间约 32s，不再被容器创建耗时污染。
- ns-3 测量方案仍在设计阶段（未实现）。

### 9.2 可选后续方向

- 在 fat-tree-k8-64 上运行 DES 模式，与 container-only 进行直接对比。
- 在 container-only 模式下尝试 CPU pinning（每路由器绑定 1 核）以评估调度开销对收敛时间的影响。
- 继续推进 ns-3 方向的设计与实现。
