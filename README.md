# SagaApp（圣人双机服务端）

采集卡取帧 → YOLO 推理 → Makcu 串口鼠标控制的 Windows 桌面程序。
它是易语言原版「圣人自用 / 圣人双机」的 C++ 翻写版：界面、配置项、数值语义都对齐易语言原版，
但把原版依赖的 `Saga.dll`（采集 + NCNN/ONNX + 串口）源码直接编进了 EXE，**运行时不再需要 Saga.dll**。

- 工程：`SagaApp.vcxproj`（唯一工程，`Saga.sln` 引用）/ 源码：`src\`
- 目标：**x64 Release**（工程只配了 x64，没有 Win32 配置）
- 产物：`x64\Release\圣人双机服务端.exe`（TargetName 已改名，不是 SagaApp.exe）
- 许可：本项目主体 **GPL-3.0**（`LICENSE`）；第三方代码与数据见 `THIRD-PARTY-NOTICES.md`

---

## 0. 运行效果

![运行演示 1](docs/demo/demo-01.png)
![运行演示 2](docs/demo/demo-02.png)
![运行演示 3](docs/demo/demo-03.png)
![运行演示 4](docs/demo/demo-04.png)

🎬 **[观看完整运行演示（36 MB，放在 Release 附件）](https://github.com/ChinaSaga/saga-capture-aim/releases/latest)**

> 视频没放进仓库：GitHub 的 Markdown 渲染器不支持内联播放仓库里的 mp4（`<video>` 会被清掉，
> raw 链接只会触发下载），而且 36 MB 会让每次 `git clone` 都变慢。
> 放在 Release 附件里，点开即可播放，仓库体积保持在 11 MB 左右。

---

## 1. 30 秒构建

```powershell
cd D:\C++采集卡源码
& 'C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe' `
    SagaApp.vcxproj /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
```

构建后会自动把运行时 DLL 拷到输出目录（`RuntimeDependencies.targets`，仅 x64 生效）：
NCNN、OpenCV、ONNXRuntime、DirectML 及 VC 运行库。

MSBuild 被环境策略拦住时的备用方式（Git Bash）：

```bash
bash "D:/C++采集卡源码/.workbuddy/build.sh"     # → .workbuddy\build-x64\圣人双机服务端.exe
```

细节、依赖版本、排错见 **[docs/构建与部署.md](docs/构建与部署.md)**。

---

## 2. 运行需要什么（EXE 同目录）

| 文件 | 来源 | 说明 |
|---|---|---|
| CH343 串口驱动 | **已打进 EXE 资源** | Makcu 盒子（WCH CH343 USB 转串口）的驱动，检测到没装会自动静默安装，见下 |
| `DirectML.dll`、`ncnn.dll`、`onnxruntime.dll`、`opencv_world4120.dll` | 随构建复制 | 缺一项启动时弹「缺少 …」并退出 |
| VC 运行库 `msvcp140*.dll`、`vcruntime140*.dll`、`vcomp140.dll` 等 | 随构建复制 | 工程用 `/MD` |
| `mouse.bin` | 自备：自己采集训练，或把你那份拷进去 | 人手轨迹模型，缺失直接提示退出。训练工程见 [人手轨迹\README.md](人手轨迹/README.md)（**仓库不含该文件**，属个人数据） |
| `<模型名>.param` + `<模型名>.bin` | 用户自备 | NCNN 模型（`推理引擎=1`） |
| `<模型名>.onnx` + `空类别.txt` | 用户自备 | ONNX 模型（`推理引擎=2`，缺类别文件会写空文件） |
| `圣人自用.js` | 首次启动自动按默认值生成 | 全部配置键值对 |
| `主机IP.ini` | 首次启动自动写 | 无命令行参数时弹框问本机 IP |
| `监控双机.ini` | 按需要 | 双机模式下自动出图：`[保留参数] 宽/高/编码/名称/帧率/截图范围` |

启动会写 `SagaApp_startup.log`（分步骤 `[01]`~`[13]`），崩溃时先查它。
界面变量记忆写 `配置保存.ini`，配置快照/网页多配置写在 `配置保存\` 目录。

### 关于那个 USB 串口驱动

Makcu 盒子用的是 WCH **CH343** 芯片，Windows 没有内置它的驱动，没装时枚举不到 COM 口。
本程序把驱动（WCH 官方 2.0.2025.03，WHQL 签名）以资源形式打包进 EXE，启动时：

1. 查系统是否已装（`pnputil /enum-drivers` 里找 `ch343ser.inf`，另有 `System32\drivers\CH343S64.SYS` 快路径）
2. 没装就把 6 个驱动文件释放到 `%TEMP%\SagaMakcuDrv_<pid>\`
3. `pnputil /add-driver CH343SER.INF /install` + `pnputil /scan-devices`（让已插着的盒子立刻可用）

**代价：安装驱动要管理员权限**，所以 EXE 清单声明了 `requireAdministrator` —— 双击时会弹一次系统 UAC。
详见 [docs\架构与源码索引.md](docs/架构与源码索引.md) 和 `src\DriverRes.rc` 顶部注释。

---

## 3. 目录结构

```
D:\C++采集卡源码
├── Saga.sln / SagaApp.vcxproj         唯一工程（Release|x64、Debug|x64）
├── RuntimeDependencies.targets        构建后自动复制第三方 DLL
├── .gitignore / .gitattributes        入库规则：排除产物、DLL、模型、私有目录
├── 圣人自用.sample.js                 配置模板（入库版本；真实 `圣人自用.js` 被忽略）
├── src\                               全部源码（应用层 + 引擎层 + Makcu）
│   ├── 应用层：main / Ui / Util / Capture / Aim / Input / Net / DriverSetup
│   ├── drv\                           打进 EXE 的 CH343 驱动（INF/CAT/SYS/DLL）
│   ├── DriverRes.rc                   资源：驱动文件 + 提权清单
│   ├── app.manifest                   清单本体（requireAdministrator）
│   ├── 引擎层：OpnecvCapture / NCNN / ONNX / Mouse（原 Saga.dll）
│   ├── 头-only：App.h / Inference.h / AimLock.h / CapturePixels.h /
│   │            TensorPixels.h / NcnnPostprocess.h / FrameWait.h
│   ├── ONNX 侧：preprocessing.hpp detection.hpp nms.hpp session_base.hpp …
│   └── Makcu\                         串口协议 + Cat(网络键鼠，暂未启用) + 自带 asio
├── web\圣人视觉识别系统.html           网页控制面板（HTTP 调本机 :8888）
├── 人手轨迹\                          mouse.bin 的采集/训练工程（Python + PyTorch）
├── x64\Release\                       构建输出（EXE + PDB + 运行时 DLL + 配置，不入库）
├── docs\                              本文档与其配套文档
│   └── reference\                     GPU 型号表数据源（pci.ids 等）
└── .workbuddy\                        不属于源码：构建脚本、记忆、备份
    ├── build.sh                       x64 手工构建脚本（MSBuild 不可用时）
    ├── memory\                        会话记忆（勿删）
    ├── backup\                        历史备份与归档（可删，勿回滚覆盖）
    └── 易语言原版备份\                原版易语言源码（对照用，只读）
```

---

## 4. 文档索引

| 文档 | 看什么 |
|---|---|
| [docs/构建与部署.md](docs/构建与部署.md) | 环境、第三方库路径、编译开关、产物、部署、排错 |
| [docs/架构与源码索引.md](docs/架构与源码索引.md) | 模块职责、数据流、帧格式、线程表、锁约定 |
| [docs/配置与运行时接口.md](docs/配置与运行时接口.md) | `圣人自用.js` 键表、HTTP 接口、**网页端**与参数、UI 控件、ini 文件 |
| [docs/维护红线与已知问题.md](docs/维护红线与已知问题.md) | 不能改的东西、易语言语义坑、与原版差异、历史优化结论 |

---

## 5. 仓库包含 / 不包含什么（准备上传 GitHub）

**入库**：`README.md`、`docs\`（含 `reference\`）、`src\`、`web\`、`人手轨迹\`（只含脚本，**不含个人采集数据**）、`Saga.sln`、`SagaApp.vcxproj`、
`RuntimeDependencies.targets`、`圣人自用.sample.js`、`.gitignore`、`.gitattributes`、`LICENSE`。

**不入库**（见 `.gitignore`）：

| 类别 | 内容 | 原因 |
|---|---|---|
| 构建产物 | `x64\Release\`、`*.obj/pdb/tlog/log`、VC 中间目录 | 可由源码重建 |
| 第三方 DLL | ncnn / OpenCV / ONNXRuntime / DirectML / VC 运行库 | 体积合计约 110 MB，且构建时自动复制 |
| **模型** | `*.onnx`、`*.param`、`*.bin`（含 NCNN 权重） | 体积大、非本仓库产出、可能涉及第三方授权 |
| 个人数据 | `mouse.bin`（人手轨迹）、`圣人自用.js`、`主机IP.ini`、`监控双机.ini`、`配置保存*`、`SagaApp_startup.log` | 含个人 IP 与调参结果 |
| 私有辅助 | `.workbuddy\`（会话记忆、备份、易语言原版源码 19 MB）、`.vs\` | 非项目本体 |

克隆后要跑起来：先按 [docs/构建与部署.md](docs/构建与部署.md) 构建，再把 `mouse.bin`、模型文件、
把 `圣人自用.sample.js` 复制成 `圣人自用.js` 放到 EXE 同目录。

**注意**：第三方代码与数据的授权见 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) ——
其中 `src\Makcu\` 是第三方 SDK 源码，公开前请先确认其原始授权。

---

## 6. 三条最容易踩的全局约定

1. **编码「内 GBK、外 UTF-8」**：全工程 `/source-charset:utf-8 /execution-charset:.936`。
   源码存 UTF-8，中文常量编成 GBK；`圣人自用.js`／HTTP 走 UTF-8（`ansiToUtf8` / `utf8ToAnsi` 转换）。
   **不要改成 `/utf-8`**。
2. **不可省的三个开关**：`/arch:AVX2`（AVX2+FMA 内联指令）、`/EHsc`（Makcu 有 try/catch）、
   `/fp:fast`（数值路径，改了会出现 1 ULP 级差异）。
3. **几个硬性数值是用户明确要求保留的**：鼠标轨迹分段等待 **0.885 ms**、截图 **JPEG 质量 95**、
   截图节流 **80 ms**、单帧目标上限 **99**。详见「维护红线」。

---

## 7. 当前状态

- x64 Release 可构建；采集、推理、用户界面、HTTP、串口鼠标均已实机跑通（见维护红线里的实机数据）。
- 尚未做「采集卡 → 推理 → 鼠标」端到端帧率实测：**不要把局部微基准的倍率外推成整机 FPS**。
- `src\Makcu\cat\`（网络键鼠 Kmbox/Cat）已随源码编译并导出 C API，但**应用层目前没有调用**，
  只走串口 Makcu。
- 本仓库不含 UDP 发送端：双机模式下画面由外部采集机以 **1472 字节分包**发到本机 `:6666`。
