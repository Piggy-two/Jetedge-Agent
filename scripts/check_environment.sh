#!/usr/bin/env bash
# check_environment.sh — JetEdge-Agent 只读环境核查
#
# 只包含只读命令：不安装软件、不修改系统配置、不执行 sudo。
# 用法: bash scripts/check_environment.sh
# 输出可直接用于更新 docs/environment_snapshot.md

set -u

echo "===== OS ====="
cat /etc/os-release
uname -a

echo
echo "===== Jetson Linux (L4T) ====="
head -n 2 /etc/nv_tegra_release 2>/dev/null || echo "no /etc/nv_tegra_release"

echo
echo "===== Device Model ====="
cat /proc/device-tree/model 2>/dev/null || echo "unknown"

echo
echo "===== JetPack ====="
dpkg-query -W nvidia-jetpack 2>/dev/null || echo "nvidia-jetpack not installed"

echo
echo "===== CUDA (nvcc) ====="
/usr/local/cuda/bin/nvcc --version 2>/dev/null || nvcc --version 2>/dev/null || echo "nvcc not found"

echo
echo "===== TensorRT (trtexec) ====="
/usr/src/tensorrt/bin/trtexec --version 2>/dev/null | head -n 3 || trtexec --version 2>/dev/null | head -n 3 || echo "trtexec not found"

echo
echo "===== DeepStream ====="
deepstream-app --version-all 2>/dev/null || echo "deepstream-app not found"

echo
echo "===== GStreamer ====="
gst-inspect-1.0 --version

echo
echo "===== DeepStream / GStreamer Plugins ====="
for p in nvv4l2decoder nvstreammux nvinfer nvtracker nvdsanalytics nvurisrcbin filesrc qtdemux h264parse queue fakesink tee; do
  if gst-inspect-1.0 "$p" >/dev/null 2>&1; then
    echo "OK:      $p"
  else
    echo "MISSING: $p"
  fi
done

echo
echo "===== Power Mode ====="
nvpmodel -q --verbose 2>/dev/null | grep -E "Current mode|CORE_|CPU_A78|GPU:" | head -n 20 || nvpmodel -q 2>/dev/null || echo "nvpmodel not available"

echo
echo "===== Memory / Disk ====="
free -h
df -h / | tail -n 2

echo
echo "===== Toolchain ====="
cmake --version 2>/dev/null | head -n 1 || echo "no cmake"
g++ --version 2>/dev/null | head -n 1 || echo "no g++"
python3 --version 2>/dev/null || echo "no python3"
git --version 2>/dev/null || echo "no git"
docker --version 2>/dev/null || echo "no docker"

echo
echo "===== Jetson Monitor Tools ====="
command -v tegrastats || echo "no tegrastats"
command -v jetson_clocks || echo "no jetson_clocks"
command -v nvpmodel || echo "no nvpmodel"

echo
echo "===== DeepStream Samples ====="
ls -d /opt/nvidia/deepstream/deepstream-7.1/samples/streams/ 2>/dev/null || echo "samples not found"
ls /opt/nvidia/deepstream/deepstream-7.1/samples/streams/*.mp4 /opt/nvidia/deepstream/deepstream-7.1/samples/streams/*.h264 2>/dev/null

echo
echo "===== Current Power Mode Number ====="
nvpmodel -q 2>/dev/null

echo
echo "Done. Update docs/environment_snapshot.md with any changes."
