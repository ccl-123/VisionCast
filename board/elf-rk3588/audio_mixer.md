# ELF-RK3588 NAU8822 Mixer 记录 (已完工版)

> 项目：VisionCast 智能眼镜音视频推流系统
> 平台：ELF-RK3588
> Codec：rockchip-nau8822
> ALSA 设备：`hw:1,0`
> 状态：Mixer 通道与增益配置已完成，由初始化脚本固化。

---

## 1. 音频 Codec 结构

当前 ELF-RK3588 只有一个 ALSA 采集 PCM 设备 `hw:1,0`，其对应物理板载 Codec芯片 NAU8822：

```text
card 1: rockchip-nau8822
```

主麦克风 (Main Mic) 与 耳机麦克风 (Headset Mic) 共享该声卡通道，利用 NAU8822 内部的 Mixer 进行输入源切换与模拟/数字增益调节。

```text
[Main Mic] ─── (通过 amixer 切换) ───┐
                                      ▼
[Headset Mic] ───────────────> [Input Mixer] ──> [PGA (增益)] ──> [ADC] ──> [ALSA hw:1,0]
```

---

## 2. 采集相关核心 Mixer 控件与配置

以下为 NAU8822 与音频采集紧密关联的 Mixer 控件以及推荐配置值：

| 控件名称 | 作用 | 推荐配置值 | 配置说明 |
| --- | --- | --- | --- |
| `Main Mic` | 主麦克风输入开关 | `on` | 开启板载麦克风输入 |
| `Headset Mic` | 耳机麦克风输入开关 | `off` | 默认关闭耳机麦克风 (防杂音) |
| `ADC` | 模拟转数字采集音量 | `200` | 保持较高的数字捕获音量 |
| `PGA` | 模拟前级放大增益 | `120` | 控制基本增益，过大可能产生杂音 |
| `PGA Boost` | 麦克风增益增强 | `20dB` | 视环境噪底大小动态开启/增强 |
| `Left Input Mixer MicP` | 左声道 MicP 混音开关 | `on` | 将物理输入连接至 ADC |
| `Right Input Mixer MicP`| 右声道 MicP 混音开关 | `on` | 将物理输入连接至 ADC |

---

## 3. 音频录放与 Mixer 初始化脚本

为了避免每次开机或设备复位后音频没有声音，项目中已在 `scripts/board/` 目录下固化了初始化配置脚本：
- **主麦克风启动脚本**：可通过 `scripts/board/setup_main_mic.sh` (或相关板端命令) 快速执行 `amixer` 配置，将物理通道切换到主麦克风，并自动调整基本增益到最佳状态。

### 3.1 命令行手动调试
如果需要调整音频参数，可在板端直接执行：
```bash
# 开启板载主麦克风
amixer -c 1 sset 'Main Mic' on
# 设置前级增益与数字捕获
amixer -c 1 sset 'PGA' 120
amixer -c 1 sset 'ADC' 200
```
或直接通过命令行图形界面进行交互：
```bash
alsamixer -c 1
```

---

## 4. 结论与已验证状态

1. **配置固化**：NAU8822 声卡的 Mixer 通道配置已经在系统初始化与启动脚本中完成固化，默认启动即可自动开启 Main Mic 采集。
2. **多通道混音适配**：`AudioCapture` 在读取 PCM 裸流后会自动检测双声道，并在用户空间进行混音降噪合成单声道，无需依赖复杂的硬件 Mixer 路由，提高了跨设备部署的容错率。
