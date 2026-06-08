# Repository Guidelines

## Project Structure & Module Organization

VisionCast 是面向 ELF-RK3588 的 C++17 音视频推流工程。核心源码在 `src/`，公共头文件在 `include/`，二者按 `common/`、`media/`、`pipeline/`、`transport/` 分层对应。运行配置放在 `config/*.json`，构建和板端辅助脚本放在 `scripts/build/`、`scripts/run/`、`scripts/board/`。`docs/` 保存系统设计、性能和协议测试文档，`board/elf-rk3588/` 保存板级设备记录。`3rdparty/` 包含 RK3588 相关预编译库和头文件，除依赖升级外不要改动。

## Build, Test, and Development Commands

- `./scripts/build/device_build.sh`：在 RK3588 板端编译并安装到 `install/visioncast/`。
- `./scripts/build/cross_build.sh --build`：在主机交叉编译并生成安装包，不部署。
- `./scripts/build/cross_build.sh --clean`：清理后重新交叉编译，并按脚本默认配置部署。
- `./install/visioncast/bin/visioncast --probe --require-devices`：检查配置中的音视频设备是否可用。
- `./install/visioncast/scripts/run/run_13855_camera.sh webrtc`：使用默认 13855 摄像头配置启动 WebRTC 推流。
- `./install/visioncast/scripts/run/run_usb_camera.sh rtp`：使用 USB C270 配置启动 RTP 推流。

## Coding Style & Naming Conventions

使用 C++17，CMake 已开启 `-Wall -Wextra -Wpedantic`。代码采用 4 空格缩进，文件名和函数名使用 `snake_case`，类和结构体使用 `PascalCase`，成员和局部变量使用描述性小写命名。项目代码放在 `visioncast` 命名空间内，优先保持现有的 Doxygen 风格文件注释和中文关键实现注释。新增配置键应与现有 JSON 风格一致，并同步更新默认配置、示例配置和相关文档。

## Testing Guidelines

当前没有独立自动化测试框架或覆盖率门槛。提交前至少完成对应构建脚本验证，并按改动范围做板端冒烟测试：设备相关改动运行 `--probe --require-devices` 和 `scripts/board/probe_*.sh`；协议相关改动按 `docs/multi_protocol_test_guide.md` 验证 WebRTC、RTMP 或 RTP；性能相关改动记录到 `docs/performance_test.md`。

## Commit & Pull Request Guidelines

提交历史采用 Conventional Commits 风格，例如 `feat: ...`、`fix: ...`、`docs: ...`、`build: ...`、`config: ...`，中文或英文说明均可，但应明确改动对象和结果。Pull Request 需包含变更摘要、测试/板端验证结果、关联 issue（如有），涉及画面、日志格式或协议行为时附截图、日志片段或拉流命令。避免在同一 PR 中混合无关重构和功能改动。

## Security & Configuration Tips

不要提交真实设备密码、内网固定凭据或私有服务器地址；脚本中的部署 IP、用户和目标目录优先通过环境变量覆盖。修改 `LD_LIBRARY_PATH`、部署目录或 `3rdparty/` 运行库时，确认不会破坏 `install/visioncast/` 的板端可运行性。
