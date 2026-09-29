// ============================================================================
//  DriverSetup.cpp —— Makcu 盒子（WCH CH343 USB 转串口）驱动自动安装
//
//  为什么要它：CH343 是 USB 转串口芯片，Windows 必须装好功能驱动才会出现 COM 口；
//  没装驱动时程序枚举不到盒子，表现为“连不上 Makcu”。
//
//  ---------------------------------------------------------------------------
//  判断“装好了没有”的口径（踩过坑，别改回只看驱动包）
//  ---------------------------------------------------------------------------
//  2026-09-29 的教训：只看「驱动包在不在 DriverStore 里」是**不够**的 ——
//  WCH 的安装器点“卸载”会把服务和 System32\drivers 下的 .sys 删掉，但驱动包
//  还留在系统里；只看包就会误判成“已装好”，于是既没安装、也没有任何提示。
//
//  现在按下面顺序判断：
//    1. 盒子的串口在不在（makcu::SerialPort::findMakcuPorts）→ 在就一切正常，直接收工
//    2. 驱动是否真的被设备用着：服务键 CH34* 存在 **且** System32\drivers\CH343S64.SYS 在
//       （卸载会留下孤儿服务键，所以必须两个条件都满足）
//    3. 以上都不满足 → 释放资源 + pnputil 安装 + scan-devices（幂等，重复跑也不会出重复包）
//
//  安装过程完全无窗口；失败会弹一个消息框（否则用户根本不知道没装上）。
//
//  ⚠ 安装驱动需要管理员权限。本工程的 EXE 清单已设为 requireAdministrator，
//    所以启动时就已经提权，这一步能全程无提示完成。
//
//  驱动来源与版本见 src\DriverRes.rc 顶部注释（WCH 官方 2.0.2025.03，WHQL 签名，
//  取自官方包 CH343SER.EXE，提取方法见 src\drv\README.txt）。
// ============================================================================

#include "App.h"
#include "serialport.h"     // makcu::SerialPort::findMakcuPorts（判断盒子串口在不在）

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

// ---- 日志 ------------------------------------------------------------------
// 必须复用 main.cpp 里那个启动日志句柄：如果再 fopen 一次同一文件，
// 两个句柄各自记住自己的写位置，互相覆盖 —— 上一版就是这么把日志写花的。
void drvLog(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    FILE* f = g_logFile();
    if (f)
    {
        vfprintf(f, fmt, ap);
        fputc('\n', f);
        fflush(f);
    }
    va_end(ap);
}

// ---- 进程是否已提权（只用于日志）--------------------------------------------
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

    WaitForSingleObject(pi.hProcess, 120000);   // pnputil 正常几百毫秒；装机慢时留足
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

// ---- 盒子的串口出现了没有（最直接的“能用”判据）------------------------------
bool makcuPortPresent(std::string* portOut)
{
    std::vector<std::string> ports = makcu::SerialPort::findMakcuPorts();
    if (!ports.empty() && portOut) *portOut = ports.front();
    return !ports.empty();
}

// ---- 驱动是否被设备用着 ------------------------------------------------------
// 注意：卸载驱动后服务键可能残留成孤儿键，所以必须同时确认 .sys 文件还在。
// 驱动包本身在不在 DriverStore 里**不作为**判据（见文件头注释）。
bool driverBound()
{
    char sysRoot[MAX_PATH] = {};
    GetSystemDirectoryA(sysRoot, MAX_PATH);
    if (!fileExists(std::string(sysRoot) + "\\drivers\\CH343S64.SYS")) return false;

    HKEY services = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Services",
                      0, KEY_READ, &services) != ERROR_SUCCESS)
        return false;

    bool found = false;
    for (DWORD i = 0;; ++i)
    {
        char name[512] = {};
        DWORD len = sizeof(name);
        if (RegEnumKeyExA(services, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        std::string up(name);
        for (char& c : up) c = (char)toupper((unsigned char)c);
        if (up.find("CH34") != std::string::npos) { found = true; break; }
    }
    RegCloseKey(services);
    return found;
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
    // 盒子已经能用（串口出现）＝ 什么都不用做
    if (makcuPortPresent(nullptr)) return true;
    // 驱动已被设备用着（盒子没插或没识别，但驱动是好的）
    return driverBound();
}

bool ensureMakcuDriver(bool* installedNow)
{
    if (installedNow) *installedNow = false;

    std::string port;
    if (makcuPortPresent(&port))
    {
        drvLog("[drv] 盒子串口已就绪（%s），无需安装驱动", port.c_str());
        return true;
    }

    if (driverBound())
    {
        drvLog("[drv] 驱动已安装（服务与 CH343S64.SYS 均在），盒子未插或未识别，跳过安装");
        return true;
    }

    drvLog("[drv] 未检测到可用驱动 → 开始从 EXE 资源安装（提权=%d）", (int)isElevated());

    char tmp[MAX_PATH] = {};
    if (!GetTempPathA(MAX_PATH, tmp))
    {
        drvLog("[drv] GetTempPath 失败");
        return false;
    }
    std::string dir = std::string(tmp) + "SagaMakcuDrv_" +
                      std::to_string((unsigned long)GetCurrentProcessId());

    if (!extractDriverFiles(dir))
    {
        drvLog("[drv] 驱动文件释放失败");
        cleanupDir(dir);
        MessageBoxA(nullptr,
            "Makcu 驱动文件释放失败，请看 EXE 目录下的 SagaApp_startup.log（[drv] 开头那几行）",
            "驱动安装失败", MB_OK | MB_ICONWARNING);
        return false;
    }
    drvLog("[drv] 已释放 %d 个驱动文件到 %s", (int)(sizeof(kDrvFiles) / sizeof(kDrvFiles[0])), dir.c_str());

    const std::string pnputil = system32Path("pnputil.exe");
    const std::string infPath = dir + "\\" + kInfName;

    DWORD code = 0;
    std::string out;
    bool ran = runCapture(pnputil.c_str(),
                          "/add-driver \"" + infPath + "\" /install", code, out);
    drvLog("[drv] pnputil /add-driver 退出码=%lu", (unsigned long)code);
    if (!out.empty()) drvLog("[drv] pnputil 输出:\n%s", out.c_str());

    // 0 = 成功；3010 = 成功但需重启；3011 = 已存在（不同版本时的提示）
    const bool ok = ran && (code == 0 || code == 3010 || code == 3011);
    if (!ok)
    {
        drvLog("[drv] 安装失败，驱动文件保留在 %s 供排查", dir.c_str());
        MessageBoxA(nullptr,
            ("Makcu 驱动安装失败（pnputil 退出码 " + std::to_string((unsigned long)code) +
             "）。\n\n详情见 EXE 目录下的 SagaApp_startup.log（[drv] 开头的行）。\n"
             "也可以手动双击官方 CH343SER.EXE 安装。").c_str(),
            "驱动安装失败", MB_OK | MB_ICONWARNING);
        return false;
    }

    // 让已经插着的盒子立刻绑定驱动（不必拔插）
    DWORD scanCode = 0;
    std::string scanOut;
    runCapture(pnputil.c_str(), "/scan-devices", scanCode, scanOut);
    drvLog("[drv] pnputil /scan-devices 退出码=%lu", (unsigned long)scanCode);

    cleanupDir(dir);

    // 复查：盒子插着的话现在应该已经出串口了
    std::string portAfter;
    if (makcuPortPresent(&portAfter))
        drvLog("[drv] 安装完成，盒子已识别为 %s%s", portAfter.c_str(),
               code == 3010 ? "（建议重启一次）" : "");
    else
        drvLog("[drv] 安装完成%s；盒子还没插（或未被识别）——插上后系统会自动装设备，"
               "可能会提示“正在安装设备驱动软件”", code == 3010 ? "（需重启生效）" : "");

    if (installedNow) *installedNow = true;
    return true;
}
