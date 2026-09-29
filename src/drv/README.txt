这个目录里的 6 个文件会被编译进 EXE 资源（见 ..\DriverRes.rc），
程序启动时若发现系统没装该驱动，会把它们释放到 %TEMP% 并用 pnputil 安装。

来源
    WCH 官方 CH343 驱动包（厂商给的 makcu驱动.EXE 就是它的 WinRAR 自解压版）
    版本 2.0.2025.03 / 2025-03-03，WHQL 签名：
      Microsoft Windows Hardware Compatibility Publisher
    在本机 Windows 上安装后，从驱动仓库把文件抄出来的：
      C:\Windows\System32\DriverStore\FileRepository\ch343ser.inf_amd64_*
    发布名 oem244.inf，提供程序 wch.cn

为什么是这 6 个
    INF 的 [CH343SER_Inst.NTamd64] 只引用这些：
      CH343SER.INF                  INF 本体
      CH343SER.CAT                  签名目录（改了签名就废）
      CH343S64.SYS   (CopyFiles.SYSA64)  x64 驱动本体
      CH343PTA64.DLL (CopyFiles.DLLA64)  属性页
      CH343PORTSA64.DLL (DLLA64)         端口配置
      CH343PT.DLL    (CopyFiles.WOWDLL)  WOW64 属性页
    不带 CH343SER.SYS（x86 用）、CH343M64.SYS（ARM64 用，名字里的 M 是 Machine 不是 Modem）、
    CH343PORTS.DLL（x86 用）—— 64 位系统用不到。

⚠ 不要改这些文件的内容，也不要让 git 改写换行（.gitattributes 里 src/drv/** 已按 binary 处理）。
  签名目录 CH343SER.CAT 覆盖这些文件的哈希，改一个字节就会导致静默安装被驱动签名强制拒绝。

更新驱动时
    1. 用新包里的文件替换本目录（保持文件名一致）
    2. 确认 INF 里 [CH343SER_Inst.NTamd64] 引用的文件都在，缺了就在 .rc 里补一条资源
    3. 重新构建，然后按 docs\构建与部署.md 第 4.5 节验证资源真的进 EXE 了
