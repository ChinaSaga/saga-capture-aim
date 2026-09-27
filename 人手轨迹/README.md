# 人手轨迹：mouse.bin 的采集与训练

用神经网络拟合真人鼠标移动轨迹 —— 主程序 `src\Mouse.cpp` 加载的 `mouse.bin` 就是这里训练出来的。

> **本仓库不包含 `mouse.bin` 和采集数据**：那是本人真实鼠标移动数据，属于个人信息。
> 需要的话按下面的流程自己采、自己训（约 300 条轨迹就够），产出直接放到主程序 EXE 同目录。

## 它解决什么问题

固定曲线（PID / 直线分段）做出来的鼠标移动和真人差别很大，容易被轨迹检测识别。
这里用**三个全连接层**的小网络拟合真人：

| | 形状 | 说明 |
|---|---|---|
| 输入 | `dx, dy` | 目标点相对当前位置的位移 |
| 输出 | `10 × 2` | 拟合这条位移所用的 10 个轨迹点 |

`src\Mouse.cpp` 里的 `saga::mc_calc()` 就是用 AVX2 + FMA 手写的同一套前向推理（2→64→32→20），
再把 10 个点按 `trajMove10 / 5 / 3` 分组，段间等待 **0.885 ms** 发给硬件鼠标。

## 文件说明

| 文件 | 作用 |
|---|---|
| `collect_mouse_data.py` | 采集工具（Tkinter 全屏）：点红球开始记录 → 移动到蓝球再点结束，一条轨迹自动采样成 10 个关键点追加进 `mouse_data.csv`（同时做一次水平镜像增广，一条变两条） |
| `train.py` | 训练 + 导出：读 `mouse_data.csv`，训练 `SimpleNet(2→64→ReLU→32→ReLU→20)`，500 epoch，最后导出 `mouse.bin` |
| `show.py` | 画散点图，肉眼验证拟合出的 10 个点像不像自己的轨迹 |
| `test.py` | 检查 `mouse.onnx` 是否导出成功（可选路径，本项目实际只用 bin） |
| `collect_mouse_data.spec` | PyInstaller 打包配置（生成免 Python 环境的采集 exe） |
| `一键训练.bat` | 调本机 conda 环境 `mouse` 跑 `train.py`（**里面写的是作者的 python 路径，改成你自己的**） |
| `训练人手和打包.txt` | 训练 + 打包的步骤速记 |

## 完整流程

```bash
# 0. 依赖
pip install numpy pandas torch

# 1. 采集（约 300 条；Esc 随时退出，数据追加进 mouse_data.csv）
python collect_mouse_data.py

# 2.（可选）自己切一部分数据到 mouse_data_test.csv 当验证集

# 3. 训练并导出 mouse.bin
python train.py

# 4. 部署：放到主程序 EXE 同目录
cp mouse.bin  <主程序输出目录>/mouse.bin
```

主程序启动时 `trajInit()` 会读 `运行目录\mouse.bin`，**缺文件会弹框并退出**。
`mouse.bin` / `mouse_data.csv` 已被 `.gitignore` 排除，放心放在这个目录里也不会被提交。

## mouse.bin 的格式

按 float32 顺序直接写下（`train.py::export_bin`）：

```
w1[64×2]  b1[64]  w2[32×64]  b2[32]  w3[20×32]  b3[20]
= (128 + 64 + 2048 + 32 + 640 + 20) × 4 = 11,728 字节
```

`Mouse.cpp` 按同样的顺序读取，所以**改网络结构就必须同步改 C++ 侧**，否则结果全乱。

## 原工程里没有搬进本仓库的东西

| 文件 | 原因 |
|---|---|
| `mouse.bin`、`mouse_data.csv`、`圣人的人手\mouse_data.csv` | 本人真实鼠标数据，不公开 |
| `collect_mouse_data.exe`（13 MB） | PyInstaller 产物，可由 `.spec` 重新打包 |
| `build\` | PyInstaller 临时目录 |
| `makcu.dll`（69 KB）、`opencv_world4120.dll`（55 MB） | 第三方 DLL，按需自备 |
| `人手轨迹测试.e`（6.8 MB） | 另一个易语言工程（轨迹测试），不属于本 C++ 项目 |
