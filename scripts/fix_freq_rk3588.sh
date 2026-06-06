#!/usr/bin/env bash
# ==============================================================================
# RK3588 满血性能锁频脚本
#
# 参考：
#   face_attendance/RK3588-NPU/C++/face_recognition_cap/fix_freq_rk3588.sh
#
# 使用场景：
#   VisionCast 做摄像头、RGA、MPP 编码、RTP/UDP 推流延迟测试前，锁定 CPU/GPU/NPU/DDR
#   频率并关闭 CPU 深度 idle，减少频率波动和唤醒延迟对测试结果的影响。
#
# 用法：
#   sudo bash scripts/build/fix_freq_rk3588.sh
#   部署到板端后：
#   sudo bash scripts/fix_freq_rk3588.sh
#
# 注意：
#   1. 必须 root 权限运行；
#   2. 不同内核/板卡镜像的 sysfs 节点可能略有差异，脚本会跳过不存在的节点；
#   3. 长时间满频会增加功耗和温度，性能测试结束后建议重启或恢复 governor。
# ==============================================================================

set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
    echo "Please run as root, for example: sudo bash $0"
    exit 1
fi

echo "========================================"
echo "Locking RK3588 high-performance mode"
echo "========================================"

echo " -> disabling deep CPU idle..."
for i in {0..7}; do
    if [[ -f "/sys/devices/system/cpu/cpu${i}/cpuidle/state1/disable" ]]; then
        echo 1 > "/sys/devices/system/cpu/cpu${i}/cpuidle/state1/disable"
    fi
done

echo " -> locking NPU frequency..."
if [[ -d "/sys/class/devfreq/fdab0000.npu" ]]; then
    echo "    available: $(cat /sys/class/devfreq/fdab0000.npu/available_frequencies)"
    echo userspace > /sys/class/devfreq/fdab0000.npu/governor
    echo 1000000000 > /sys/class/devfreq/fdab0000.npu/userspace/set_freq
    echo "    current: $(cat /sys/class/devfreq/fdab0000.npu/cur_freq)"
fi

echo " -> locking CPU frequency..."
if [[ -d "/sys/devices/system/cpu/cpufreq/policy0" ]]; then
    echo userspace > /sys/devices/system/cpu/cpufreq/policy0/scaling_governor
    echo 1800000 > /sys/devices/system/cpu/cpufreq/policy0/scaling_setspeed
    echo "    policy0: $(cat /sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq)"
fi

if [[ -d "/sys/devices/system/cpu/cpufreq/policy4" ]]; then
    echo userspace > /sys/devices/system/cpu/cpufreq/policy4/scaling_governor
    echo 2352000 > /sys/devices/system/cpu/cpufreq/policy4/scaling_setspeed
    echo "    policy4: $(cat /sys/devices/system/cpu/cpufreq/policy4/scaling_cur_freq)"
fi

if [[ -d "/sys/devices/system/cpu/cpufreq/policy6" ]]; then
    echo userspace > /sys/devices/system/cpu/cpufreq/policy6/scaling_governor
    echo 2352000 > /sys/devices/system/cpu/cpufreq/policy6/scaling_setspeed
    echo "    policy6: $(cat /sys/devices/system/cpu/cpufreq/policy6/scaling_cur_freq)"
fi

echo " -> locking GPU frequency..."
if [[ -d "/sys/class/devfreq/fb000000.gpu" ]]; then
    echo userspace > /sys/class/devfreq/fb000000.gpu/governor
    echo 1000000000 > /sys/class/devfreq/fb000000.gpu/userspace/set_freq
    echo "    current: $(cat /sys/class/devfreq/fb000000.gpu/cur_freq)"
fi

echo " -> locking DDR frequency..."
if [[ -d "/sys/class/devfreq/dmc" ]]; then
    echo userspace > /sys/class/devfreq/dmc/governor
    echo 2112000000 > /sys/class/devfreq/dmc/userspace/set_freq
    echo "    current: $(cat /sys/class/devfreq/dmc/cur_freq)"
fi

echo "========================================"
echo "RK3588 high-performance mode locked"
echo "========================================"
