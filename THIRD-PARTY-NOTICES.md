# 第三方代码与数据说明

本项目主体按 LICENSE（GNU GPL-3.0）发布。仓库里还包含若干**来自第三方**的代码与数据，
它们各自沿用原始授权，请勿把 LICENSE 当成对它们的重新授权。

| 内容 | 位置 | 原授权 / 来源 |
|---|---|---|
| standalone Asio 1.38.2（头文件版） | `src\Makcu\cat\asio\` | Boost Software License 1.0；许可证和上游提交记录随源码保留 |
| Makcu 串口协议维护分支 | `src\Makcu\Makcu.cpp`、`src\Makcu\src\serialport.cpp`、`src\Makcu\include\` | 核查的 K4HVH/makcu-cpp v1.3.5 上游为 GPL-3.0；参考许可证见 `docs/licenses/MAKCU-upstream-GPL-3.0.txt`，本地定制部分的原始来源仍需保留原作者说明 |
| Cat(Kmbox) 网络 SDK | `src\Makcu\cat\src\`、`src\Makcu\cat\include\` | **原始 SDK 的上游版本及授权未声明**；其中 Linux `input-event-codes.h` 自带 GPL-2.0 声明 |
| **WCH CH343 USB 转串口驱动及安装器** | `src\drv\`（9 个驱动文件及 `DRVSETUP64.exe`，编进 EXE 资源） | 南京沁恒（WCH，wch.cn）官方驱动包，驱动版本 2.0.2025.03 / 2025-03-03；驱动目录具有 **WHQL 签名**（Microsoft Windows Hardware Compatibility Publisher）。安装器与驱动文件按官方包原样保留 |
| PCI 设备 ID 表数据 | `src\GpuIdTable.h`（源数据 `docs\reference\pci.ids`） | pci.ids 项目：GPL-2.0+ 或 3-clause BSD（二选一，均可与本项目共存） |
| 显卡名简化规则 | `src\GpuNameTable.h` | 本项目自行整理 |
| YOLOs-CPP 相关头文件维护分支 | `src\yolos.hpp`、`detection.hpp`、`nms.hpp`、`session_base.hpp`、`preprocessing.hpp` 等 | Geekgineer/YOLOs-CPP v1.1.0 上游为 AGPL-3.0；参考许可证见 `docs/licenses/YOLOs-CPP-AGPL-3.0.txt`；保留本地 SIMD 和缓冲复用改动 |

## ⚠ 需要注意的两点

1. **`src\Makcu\` 是第三方源码**，其原始授权未在文件中声明。仓库以 GPL-3.0 整体发布时，
   这部分存在授权风险。可选做法（由你决定）：
   - 补全该目录的原始许可证文件后再发布；或
   - 把 `src\Makcu\` 排除出公开仓库，改为 README 里说明依赖来源；或
   - 换成 MIT 等更宽松的整体授权（asio 兼容，Makcu 部分仍需原作者授权）。
2. **运行期依赖不在此仓库**：NCNN、OpenCV、ONNXRuntime、DirectML、VC 运行库各有其授权
   （构建时由 `RuntimeDependencies.targets` 自动复制），模型权重与人手轨迹 `mouse.bin` 同理由使用者自备。

运行时 SDK 位于忽略入库的 `.deps`；OpenCV（Apache-2.0）、NCNN（BSD-3-Clause）、ONNX Runtime（MIT）和 DirectML 的许可证及第三方说明构建时复制到输出目录 `licenses`。下载和源码构建固定在 `dependencies.lock.json` 记录的版本与校验值；更新详情见 [依赖更新记录](docs/依赖更新-2026-10-02.md)。
