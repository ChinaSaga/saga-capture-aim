// ============================================================================
//  DriverSetup.cpp —— Makcu 盒子（WCH CH343 USB 转串口）驱动自动安装
//
//  为什么要它：CH343 是 USB 转串口芯片，Windows 必须装好它的功能驱动才会
//  出现 COM 口；没装驱动时程序枚举不到盒子，表现为“连不上 Makcu”。
//
//  做法（不碰 WCH 那个图形安装器，pnputil 一行命令搞定）：
//    1. 先查系统是否已经装过这个驱动包（快路径看 System32\drivers\CH343S64.SYS，
//       否则跑 pnputil /enum-drivers 找 ch343ser.inf）
//    2. 没装就从本 EXE 的资源里把 9 个驱动文件释放到 %TEMP%
//    3. 用微软自带的 pnputil /add-driver <INF> /install 安装（无任何窗口）
//    4. 再 pnputil /scan-devices 让已经插着的盒子立刻绑定驱动
//
//  ⚠ 安装驱动需要管理员权限。本工程的 EXE 清单已设为 requireAdministrator，
//    所以启动时就已经提权，这一步能全程无提示完成。
//    如果哪次是以普通用户身份运行的，这里会失败并把 pnputil 的原文记进日志。
//
//  驱动来源与版本见 src\DriverRes.rc 顶部注释（WCH 官方 2.0.2025.03，WHQL 签名，
//  取自官方包 CH343SER.EXE，提取方法见 src\drv\README.txt）。
// ============================================================================

#include "App.h"

#include <cstdarg>
#include <shellapi.h>

// 资源 ID 与 DriverRes.rc 保持一致
#define IDR_MAKCU_DRV_INF           401
#define IDR_MAKCU_DRV_CAT           402
#define IDR_MAKCU_DRV_SYS_X86       403
#define IDR_MAKCU_DRV_SYS_X64       404
#define IDR_MAKCU_DRV_SYS_ARM64     405
#define IDR_MAKCU_DRV_DLL_PT_X86    406
#define IDR_MAKCU_DRV_DLL_PT_X64    407
#define IDR_MAKCU_DRV_DLL_PORTS_X86 408
#define IDR_MAKCU_DRV_DLL_PORTS_X64 409

namespace
{

struct DrvFile
{
    int         id;
    const char* name;
};

// 官方包里的全部 9 个驱动文件（正好是 INF 的 [SourceDisksFiles] 那 9 条）。
// 之前只带 x64 的 6 个，在本机（驱动已存在）能装，但缺文件的包在干净系统上有风险，
// 现在按官方原样全带 —— 反正一共才 640 KB 左右，打进 EXE 换省心。
const DrvFile kDrvFiles[] = {
    { IDR_MAKCU_DRV_INF,           "CH343SER.INF" },
    { IDR_MAKCU_DRV_CAT,           "CH343SER.CAT" },
    { IDR_MAKCU_DRV_SYS_X86,       "CH343SER.SYS" },
    { IDR_MAKCU_DRV_SYS_X64,       "CH343S64.SYS" },
    { IDR_MAKCU_DRV_SYS_ARM64,     "CH343M64.SYS" },
    { IDR_MAKCU_DRV_DLL_PT_X86,    "CH343PT.DLL" },
    { IDR_MAKCU_DRV_DLL_PT_X64,    "CH343PTA64.DLL" },
    { IDR_MAKCU_DRV_DLL_PORTS_X86, "CH343PORTS.dll" },
    { IDR_MAKCU_DRV_DLL_PORTS_X64, "CH343PORTSA64.dll" },
};

const char* kInfName = "CH343SER.INF";
const char* kPnpMarker = "ch343ser.inf";   // pnputil /enum-drivers 输出里的“原始名称”

// ---- 日志：追加进 EXE 目录下的启动日志 --------------------------------------
void drvLog(const char* fmt, ...)
{
    std::string path = g.runDir + "\\SagaApp_startup.log";
    FILE* f = fopen(path.c_str(), "ab");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// ---- 当前进程是否已提权 ------------------------------------------------------
bool isElevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elev{};
    DWORD size = sizeof(elev);
    BOOL ok = GetTokenInformation(token, TokenElevation, &elev, size, &size);
    CloseHandle(token);
    return ok && elev.TokenIsElevated != 0;
}

// ---- 跑一个程序并抓它的输出，全程无窗口（CREATE_NO_WINDOW）-------------------
bool runCapture(const char* exePath, const std::string& args,
                DWORD& exitCode, std::string& output)
{
    exitCode = (DWORD)-1;
    output.clear();

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;

    std::string cmdline = std::string("\"") + exePath + "\" " + args;
    std::vector<char> mutableCmd(cmdline.begin(), cmdline.end());
    mutableCmd.push_back('\0');

    PROCESS_INFORMATION pi{};
    BOOL started = CreateProcessA(exePath, mutableCmd.data(), nullptr, nullptr,
                                  TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!started)
    {
        CloseHandle(rd);
        return false;
    }

    char buf[4096];
    DWORD got = 0;
    while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got > 0)
        output.append(buf, got);

    WaitForSingleObject(pi.hProcess, 60000);        // pnputil 正常几百毫秒内结束
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(rd);
    return true;
}

std::string system32Path(const char* name)
{
    char dir[MAX_PATH] = {};
    GetSystemDirectoryA(dir, MAX_PATH);
    return std::string(dir) + "\\" + name;
}

// ---- 驱动包是否已经装过 ------------------------------------------------------
// 查 pnputil 比查注册表更准：驱动包可能已安装但盒子没插过，那种情况下
// System32\drivers 里还看不到 .sys，只能靠 pnputil 列出的驱动包判断。
bool driverPackagePresent()
{
    DWORD code = 0;
    std::string out;
    if (!runCapture(system32Path("pnputil.exe").c_str(), "/enum-drivers", code, out))
        return false;

    // 输出是本地 ANSI 编码（中文系统是 GBK），原始名称一行形如 "原始名称:      ch343ser.inf"
    std::string low = out;
    for (char& c : low) c = (char)tolower((unsigned char)c);
    return low.find(kPnpMarker) != std::string::npos;
}

// ---- 把资源里的驱动释放到临时目录 -------------------------------------------
bool extractDriverFiles(const std::string& dir)
{
    char tmp[MAX_PATH] = {};
    if (!GetTempPathA(MAX_PATH, tmp)) return false;
    if (!CreateDirectoryA(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        return false;

    HMODULE mod = GetModuleHandleA(nullptr);
    for (const DrvFile& df : kDrvFiles)
    {
        HRSRC res = FindResourceA(mod, MAKEINTRESOURCEA(df.id), RT_RCDATA);
        if (!res)
        {
            drvLog("[drv] 缺少资源 %d (%s)", df.id, df.name);
            return false;
        }
        DWORD size = SizeofResource(mod, res);
        HGLOBAL glob = LoadResource(mod, res);
        const void* data = glob ? LockResource(glob) : nullptr;
        if (!data || size == 0)
        {
            drvLog("[drv] 资源读取失败 %s", df.name);
            return false;
        }

        std::string path = dir + "\\" + df.name;
        FILE* f = fopen(path.c_str(), "wb");
        if (!f)
        {
            drvLog("[drv] 无法写出 %s", path.c_str());
            return false;
        }
        size_t wrote = fwrite(data, 1, size, f);
        fclose(f);
        if (wrote != size)
        {
            drvLog("[drv] 写入不完整 %s", path.c_str());
            return false;
        }
    }
    return true;
}

void cleanupDir(const std::string& dir)
{
    for (const DrvFile& df : kDrvFiles)
        DeleteFileA((dir + "\\" + df.name).c_str());
    RemoveDirectoryA(dir.c_str());
}

} // namespace

// ============================================================================
//  对外接口
// ============================================================================

bool makcuDriverReady()
{
    // 快路径：驱动已经绑定过设备，System32\drivers 里就有它的 .sys
    char sysRoot[MAX_PATH] = {};
    GetSystemDirectoryA(sysRoot, MAX_PATH);
    if (fileExists(std::string(sysRoot) + "\\drivers\\CH343S64.SYS")) return true;

    // 慢路径（要起一个 pnputil 进程，约几十毫秒）：驱动包是否已经安装
    return driverPackagePresent();
}

bool ensureMakcuDriver(bool* installedNow)
{
    if (installedNow) *installedNow = false;

    if (makcuDriverReady())
    {
        drvLog("[drv] CH343 驱动已存在，跳过安装");
        return true;
    }

    drvLog("[drv] 未检测到 CH343 驱动，开始从 EXE 资源安装（提权=%d）",
           (int)isElevated());

    char tmp[MAX_PATH] = {};
    if (!GetTempPathA(MAX_PATH, tmp))
    {
        drvLog("[drv] GetTempPath 失败");
        return false;
    }
    std::string dir = std::string(tmp) + "SagaMakcuDrv";
    // 用进程号区分，避免多开时互相覆盖
    dir += "_" + std::to_string((unsigned long)GetCurrentProcessId());

    if (!extractDriverFiles(dir))
    {
        drvLog("[drv] 驱动文件释放失败");
        cleanupDir(dir);
        return false;
    }
    drvLog("[drv] 已释放驱动到 %s", dir.c_str());

    std::string pnputil = system32Path("pnputil.exe");
    std::string infPath = dir + "\\" + kInfName;

    DWORD code = 0;
    std::string out;
    bool ran = runCapture(pnputil.c_str(),
                          "/add-driver \"" + infPath + "\" /install", code, out);
    drvLog("[drv] pnputil /add-driver 退出码=%lu", (unsigned long)code);
    if (!out.empty()) drvLog("[drv] 输出:\n%s", out.c_str());

    // 退出码 0 = 成功；3010 = 成功但需重启；其它按失败处理
    bool ok = ran && (code == 0 || code == 3010);
    if (ok)
    {
        // 让已经插着的盒子立刻绑定驱动（否则要拔插一次）
        DWORD scanCode = 0;
        std::string scanOut;
        runCapture(pnputil.c_str(), "/scan-devices", scanCode, scanOut);
        drvLog("[drv] pnputil /scan-devices 退出码=%lu", (unsigned long)scanCode);
        if (installedNow) *installedNow = true;
        drvLog("[drv] 驱动安装成功%s", code == 3010 ? "（需重启生效）" : "");
    }
    else
    {
        // 保留临时文件，方便排查（日志里能看到路径）
        drvLog("[drv] 驱动安装失败，文件保留在 %s（可手动运行 pnputil 重试）", dir.c_str());
        return false;
    }

    cleanupDir(dir);
    return true;
}
