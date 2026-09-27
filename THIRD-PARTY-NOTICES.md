# 第三方代码与数据说明

本项目主体按 LICENSE（GNU GPL-3.0）发布。仓库里还包含若干**来自第三方**的代码与数据，
它们各自沿用原始授权，请勿把 LICENSE 当成对它们的重新授权。

| 内容 | 位置 | 原授权 / 来源 |
|---|---|---|
| standalone Asio（头文件版） | `src\Makcu\cat\asio\` | Boost Software License 1.0（与 GPL-3.0 兼容） |
| Makcu / Cat(Kmbox) 串口与网络协议实现 | `src\Makcu\Makcu.cpp`、`src\Makcu\src\serialport.cpp`、`src\Makcu\include\`、`src\Makcu\cat\src\` | **第三方 SDK 源码，原授权未在仓库中声明** |
| PCI 设备 ID 表数据 | `src\GpuIdTable.h`（源数据 `docs\reference\pci.ids`） | pci.ids 项目：GPL-2.0+ 或 3-clause BSD（二选一，均可与本项目共存） |
| 显卡名简化规则 | `src\GpuNameTable.h` | 本项目自行整理 |
| YOLO/ONNX 相关后处理头文件 | `src\yolos.hpp`、`detection.hpp`、`nms.hpp`、`session_base.hpp`、`preprocessing.hpp` 等 | 依照其原始工程惯例整理 |

## ⚠ 需要注意的两点

1. **`src\Makcu\` 是第三方源码**，其原始授权未在文件中声明。仓库以 GPL-3.0 整体发布时，
   这部分存在授权风险。可选做法（由你决定）：
   - 补全该目录的原始许可证文件后再发布；或
   - 把 `src\Makcu\` 排除出公开仓库，改为 README 里说明依赖来源；或
   - 换成 MIT 等更宽松的整体授权（asio 兼容，Makcu 部分仍需原作者授权）。
2. **运行期依赖不在此仓库**：NCNN、OpenCV、ONNXRuntime、DirectML、VC 运行库各有其授权
   （构建时由 `RuntimeDependencies.targets` 自动复制），模型权重与人手轨迹 `mouse.bin` 同理由使用者自备。
