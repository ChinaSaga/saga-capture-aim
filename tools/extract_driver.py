#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
从官方驱动包 CH343SER.EXE 里提取驱动文件到 src\\drv\\

官方包是 WinRAR 自解压格式，它的脚本支持静默解包（-s2 = 不显示任何界面），
所以**不需要 7-Zip / WinRAR**：

    CH343SER.EXE -s2 -d<目标目录>

解包后它会顺手启动 DRVSETUP64.exe（WCH 的图形安装器）—— 我们走 pnputil
装 INF，不用那个界面，所以脚本会把它结束掉。

用法：
    python .workbuddy\extract_driver.py [官方包路径] [输出目录]
默认：
    官方包 = D:\\C++采集卡源码\\CH343SER.EXE
    输出目录 = D:\\C++采集卡源码\\src\\drv
"""

import os
import shutil
import subprocess
import sys
import time

DEFAULT_SFX = r'D:\C++采集卡源码\CH343SER.EXE'
DEFAULT_OUT = r'D:\C++采集卡源码\src\drv'

# INF 的 [SourceDisksFiles] 就是这 9 条；kDrvFiles[] / DriverRes.rc 必须和它对齐
WANTED = [
    'CH343SER.INF', 'CH343SER.CAT',
    'CH343SER.SYS', 'CH343S64.SYS', 'CH343M64.SYS',
    'CH343PT.DLL', 'CH343PTA64.DLL',
    'CH343PORTS.dll', 'CH343PORTSA64.dll',
    'DRVSETUP64.exe',
]


def main():
    sfx = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_SFX
    out = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_OUT
    staging = os.path.join(os.environ.get('TEMP', r'C:\Windows\Temp'), 'ch343_extract')

    if not os.path.isfile(sfx):
        print('找不到官方包:', sfx); return 1

    shutil.rmtree(staging, ignore_errors=True)
    os.makedirs(staging, exist_ok=True)

    print('静默解包:', sfx, '->', staging)
    p = subprocess.Popen([sfx, '-s2', '-d' + staging], cwd=os.path.dirname(sfx))
    for _ in range(30):
        if p.poll() is not None:
            break
        time.sleep(1)
    print('  解包退出码:', p.poll() if p.poll() is not None else '（超时）')

    # 官方包会拉起这个图形安装器，我们用不着，结束掉
    subprocess.run(['taskkill', '/F', '/IM', 'DRVSETUP64.exe'], capture_output=True)
    subprocess.run(['taskkill', '/F', '/IM', 'SETUP.EXE'], capture_output=True)

    os.makedirs(out, exist_ok=True)
    missing = []
    for name in WANTED:
        src = os.path.join(staging, name)
        if not os.path.isfile(src):          # 有些包把 x64 文件放在子目录里
            for root, _dirs, files in os.walk(staging):
                if name.lower() in [f.lower() for f in files]:
                    src = os.path.join(root, name); break
        if not os.path.isfile(src):
            missing.append(name); continue
        dst = os.path.join(out, name)
        shutil.copyfile(src, dst)
        print('  写出 %-22s %8d 字节' % (name, os.path.getsize(dst)))

    shutil.rmtree(staging, ignore_errors=True)

    if missing:
        print('缺失:', missing, '（官方包版本可能变了，先看 INF 的 [SourceDisksFiles]）')
        return 2

    # 顺手核对签名（官方包里的 CAT 应该覆盖这些文件）
    signtool = r'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe'
    if os.path.isfile(signtool):
        cat = os.path.join(out, 'CH343SER.CAT')
        files = [os.path.join(out, f) for f in WANTED if f.endswith('.sys') or f.endswith('.dll')]
        print('校验签名:', subprocess.run([signtool, 'verify', '-pa', '-c', cat] + files,
                                        capture_output=True).returncode == 0 and '通过' or '有问题')

    print('完成，记得重新构建 EXE（驱动是编译进资源的）')
    return 0


if __name__ == '__main__':
    sys.exit(main())
