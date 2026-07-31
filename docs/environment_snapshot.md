# JetEdge-Agent 环境快照

> 记录时间：2026-07-31（本地时区）
> 采集方式：只读命令核查，未安装、未修改任何系统配置
> 所有信息来自当前 Jetson 实机，任何与本快照不符的假设必须重新核查

---

## 1. 硬件

| 项目 | 实测值 |
|---|---|
| 设备型号 | NVIDIA Jetson Orin Nano Engineering Reference Developer Kit Super |
| 内存 | 7.4 GiB 总量（核查时已用约 3.2~3.4 GiB，可用约 4.0 GiB） |
| Swap | 3.7 GiB（未使用） |
| 存储 | NVMe `/dev/nvme0n1p1`，233G 总量，约 81G 可用（64% 已用） |
| CPU | 6 核 Arm Cortex-A78（`lscpu` 架构 aarch64），核查时全部 6 核在线 |
| GPU 频率上限 | 1020 MHz（MAXN_SUPER 模式，见功耗模式节） |

注意：设备型号含 "Super"，与普通 Orin Nano 8GB 的 MAXN 功耗模式不同，基准测试时必须记录功耗模式。

## 2. 操作系统与 L4T

| 项目 | 实测值 |
|---|---|
| 发行版 | Ubuntu 22.04.5 LTS (Jammy Jellyfish) |
| 内核 | Linux 5.15.148-tegra `#1 SMP PREEMPT Tue Jan 7 17:14:38 PST 2025` aarch64 |
| L4T | R36 (release), REVISION: 4.3, GCID: 38968081, DATE: Wed Jan 8 01:49:37 UTC 2025 |
| 内核变体 | oot |

## 3. 软件栈版本

| 组件 | 实测值 | 备注 |
|---|---|---|
| JetPack | 6.2.1+b38（`dpkg-query -W nvidia-jetpack`） | |
| CUDA 驱动 / 运行时 | 12.6（DeepStream `--version-all` 报告） | |
| CUDA 编译工具 | 12.6.68（`nvcc --version`，2024-08-14 构建） | `nvcc` 位于 `/usr/local/cuda/bin/nvcc`，不在默认 PATH 中 |
| TensorRT | 10.3.0（`trtexec --version` 报告 `TensorRT v100300`） | `trtexec` 位于 `/usr/src/tensorrt/bin/trtexec`，不在默认 PATH 中 |
| cuDNN | 9.0 | DeepStream 报告 |
| DeepStream | 7.1.0 | 安装于 `/opt/nvidia/deepstream/deepstream-7.1`；`deepstream-app --version-all` 可用 |
| libNVWarp360 | 2.0.1d3 | DeepStream 报告 |
| GStreamer | 1.20.3 | `gst-inspect-1.0 --version` |
| CMake | 4.2.1 | |
| GCC / G++ | 11.4.0（Ubuntu 11.4.0-1ubuntu1~22.04.2） | C++17 可用 |
| Python | 3.10.12 | |
| Docker | 29.1.4（build 0e6fee6） | 当前阶段不使用 |
| Git | 2.34.1 | |

## 4. 功耗模式

- 当前模式：`MAXN_SUPER`（nvpmodel mode 2，配置文件 `/etc/nvpmodel.conf`）
- CPU：6 核全部在线，CPU A78 频率区间 729.6 MHz ~ 1728 MHz（核查时运行在 1728 MHz）
- GPU：min/max 频率 1020 MHz
- 工具：`nvpmodel` 位于 `/usr/sbin/nvpmodel`，`tegrastats` 位于 `/usr/bin/tegrastats`，`jetson_clocks` 位于 `/usr/bin/jetson_clocks`（均可用，`tegrastats --interval 500` 实测可输出）
- 核查时温度：CPU 约 48°C，GPU 约 49°C；整机功耗约 6.6~6.8W

基准测试必须记录功耗模式；MAXN_SUPER 下 GPU 固定 1020 MHz。

## 5. GStreamer / DeepStream 插件核查（gst-inspect-1.0）

| 插件 | 状态 |
|---|---|
| nvv4l2decoder | 已安装 |
| nvstreammux | 已安装 |
| nvinfer | 已安装 |
| nvtracker | 已安装 |
| nvdsanalytics | 已安装 |
| nvurisrcbin | 已安装 |
| nvv4l2h264dec | 不存在（不要使用该名称） |
| filesrc / qtdemux / h264parse / queue / tee / fakesink | 均已安装 |

### nvstreammux 关键属性（DS 7.1）

```text
batch-size            最大 batch 数
batched-push-timeout  首帧到达后等待超时（微秒）
width / height        输出 batch buffer 尺寸（MUST be set）
live-source           告知 muxer 源是否为 live
sync-inputs           强制同步输入帧
enable-padding        缩放时保持宽高比并填充黑边
compute-hw            Compute Scaling HW
max-latency           live 模式下允许上游延迟（纳秒）
num-surfaces-per-frame
```

### nvinfer 关键属性（DS 7.1）

```text
config-file-path  指向该实例的配置文件
batch-size        最大推理 batch
process-mode      推理处理模式
unique-id         用于标识输出的唯一 ID
gpu-id
input-tensor-meta / output-tensor-meta
```

`infer-interval` 不是 nvinfer 属性，而是配置文件（`config_infer_primary.txt` 一类）中的键；本机样例配置位于 `/opt/nvidia/deepstream/deepstream-7.1/samples/configs/deepstream-app/`（同时提供 .txt 与 .yml 两种格式）。

### nvv4l2decoder 关键属性

```text
gpu-id
num-extra-surfaces
enable-max-performance
```

## 6. DeepStream 样例与媒体

### 样例源码（C/C++）

```text
/opt/nvidia/deepstream/deepstream-7.1/sources/apps/sample_apps/
  deepstream-test1 ~ test5（逐级递进的官方样例）
  deepstream-app（参考主程序）
  deepstream-nvdsanalytics-test
  deepstream-infer-tensor-meta-test
  ... 其他
```

官方 `deepstream-test1` 是单路文件解码的起点参考；`deepstream-app` 是多流配置参考。

### 样例视频

```text
/opt/nvidia/deepstream/deepstream-7.1/samples/streams/
```

已用 `gst-discoverer-1.0` 确认：

| 文件 | 容器 | 编码 | 分辨率 | 帧率 | 时长 | 音轨 |
|---|---|---|---|---|---|---|
| sample_1080p_h264.mp4 | Quicktime | H.264 High Profile | 1920×1080 | 30 fps | 48.1 s | AAC 48kHz 双声道 |
| sample_720p.h264 | 裸 H.264 | H.264 High Profile | 1280×720 | 30 fps | 40.1 s | 无 |

阶段 1 首选 `sample_1080p_h264.mp4`（注意含音轨，pipeline 需只取视频流或选择无音轨文件；`sample_720p.h264` 无音轨更适合最小 pipeline）。

## 7. 开发环境确认

- 通过 VS Code Remote-SSH 在 Jetson 上编译运行；
- 编译链：g++ 11.4.0，C++17，CMake 4.2.1；
- 构建方式：out-of-source `build/` 目录；
- 不安装系统包，不执行 `sudo apt update`。

## 8. 与本项目相关的重要事实

1. `trtexec` 和 `nvcc` 不在默认 PATH，脚本中应使用绝对路径 `/usr/src/tensorrt/bin/trtexec`、`/usr/local/cuda/bin/nvcc`。
2. 设备是 Orin Nano **Super** 开发套件，功耗模式为 MAXN_SUPER（GPU 1.02GHz），非普通 MAXN。
3. DeepStream 7.1 / GStreamer 1.20 / JetPack 6.2.1 组合；API 以本机 `sources/apps/sample_apps` 和头文件为准，不凭记忆调用。
4. 4 路测试视频可用同一官方样例重复，或后续准备四段不同视频。
5. `tegrastats` 可直接用于系统指标采集（无 sudo 已可运行）。
