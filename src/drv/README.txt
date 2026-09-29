2026-09-29 修复：软件现在嵌入并优先执行官方 DRVSETUP64.exe /S。
该安装器来自同一个 CH343SER.EXE，SHA256：
B5D55FA31753FCD86145C460D644BAC78F171F51D708A050326234B4A2876C4C
其内置帮助说明 /S 为隐式安装，/P 只预安装。程序保持官方目录结构：
根目录存放 INF 与九个驱动载荷，DRVSETUP64\DRVSETUP64.exe 存放安装器。
x64 安装器会从自身目录上移一级查找 INF，不能将 EXE 平铺到根目录。
以根目录为工作目录等待官方安装器退出。
安装器退出后仍需等待串口出现，并由应用完成协议握手才算连接成功。
若没有可用串口，则进入 pnputil / UpdateDriver / 设备重启的恢复流程。
不能仅凭残留的服务键与 SYS 文件跳过安装。

以下为原始驱动文件来源记录；九个 INF 载荷之外，现在另带官方安装器。
这个目录里的 9 个文件会被编译进 EXE 资源（见 ..\DriverRes.rc），
程序启动时若发现系统没装该驱动，会把它们释放到 %TEMP% 再用 pnputil 安装。

来源
    官方驱动包 D:\C++采集卡源码\CH343SER.EXE（WinRAR 自解压，WCH 官方原版）
    版本 2.0.2025.03 / 2025-03-03，WHQL 签名：
      Microsoft Windows Hardware Compatibility Publisher
    发布名 oem244.inf，提供程序 wch.cn

怎么从官方包里取出这些文件（不需要 7-Zip / WinRAR！）
    CH343SER.EXE 的自解压脚本支持静默解包：
        CH343SER.EXE -s2 -d<目标目录>
    -s2 = 不显示任何界面。它会顺带启动 DRVSETUP64.exe（WCH 的图形安装器），
    我们不用它 —— 直接用 pnputil 装 INF 更干净，把那个进程忽略/结束掉即可。
    本仓库的提取脚本：`tools\extract_driver.py`

为什么是这 9 个
    正好是 INF 里 [SourceDisksFiles] 列的 9 条，一条不缺：
      CH343SER.INF    INF 本体
      CH343SER.CAT    签名目录（改了签名就废）
      CH343SER.SYS    x86    驱动（[CH343SER_Inst.NT]      → CopyFiles.SYS）
      CH343S64.SYS    x64    驱动（[CH343SER_Inst.NTamd64] → CopyFiles.SYSA64）
      CH343M64.SYS    ARM64  驱动（[CH343SER_Inst.NTARM64] → CopyFiles.SYSM64）
      CH343PT.DLL     x86 属性页（CopyFiles.DLL / WOWDLL）
      CH343PTA64.DLL  x64 属性页（CopyFiles.DLLA64）
      CH343PORTS.dll  x86 端口配置（CopyFiles.DLL）
      CH343PORTSA64.dll x64 端口配置（CopyFiles.DLLA64）
    注：M64 的 M 是 Machine（ARM64），不是 Modem。

曾经只带 x64 那 6 个文件（INF/CAT/S64.SYS/PT*.DLL/PORTSA64.DLL）——
在本机（驱动已存在）pnputil 能过，但 [SourceDisksFiles] 缺 3 个文件，
在干净系统上有装不上的风险，所以现在按官方原样全带。

⚠ 不要改这些文件的内容，也不要让 git 改写换行（.gitattributes 里 src/drv/** 已按 binary 处理）。
  签名目录 CH343SER.CAT 覆盖这些文件的哈希，改一个字节就会导致静默安装被驱动签名强制拒绝。

更新驱动时
    1. 拿新的官方包，用上面的 -s2 方式解包，替换本目录（文件名保持一致）
    2. 核对 INF 的 [SourceDisksFiles] 有没有新增文件名，有就在 ..\DriverRes.rc 和
       ..\DriverSetup.cpp 的 kDrvFiles[] 里同步加上
    3. 重新构建，再按 docs\构建与部署.md 第 4.5 节验证资源真的进 EXE 了
    4. 用 signtool 复核签名与新 CAT 一致：
       signtool verify -pa -c CH343SER.CAT CH343S64.SYS CH343PT.DLL CH343PTA64.DLL CH343PORTSA64.dll
