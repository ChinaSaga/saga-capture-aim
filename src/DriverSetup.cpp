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
//    3. 以上都不满足 → 释放资源 + pnputil 安装 + **重枚举设备节点** + scan-devices
//
//  ---------------------------------------------------------------------------
//  为什么必须重枚举设备（2026-09-29 补，官方安装器就是这么干的）
//  ---------------------------------------------------------------------------
//  用户问过：“官方 CH343SER.EXE 装完不用拔插就能用，为什么你的不行？”
//  反查官方安装器（SETUP.EXE / DRVSETUP64.exe）的导入表，它用的是：
//      SetupCopyOEMInfA                把 INF 复制进驱动仓库（≈ pnputil /add-driver）
//      SetupDiGetClassDevsA + Enum…    枚举设备
//      SetupDiBuildDriverInfoList …    找匹配的驱动
//      SetupDiCallClassInstaller       真正把驱动装到设备上
//      SetupInstallFilesFromInfSectionA 按 INF 段把文件铺到 System32\drivers 等目录
//      CM_Locate_DevNodeA + CM_Reenumerate_DevNode   ★ 重枚举设备节点＝等价于拔插一次
//  我们之前只做了“把驱动包放进系统”，设备节点还是老状态，所以得靠拔插才生效。
//  现在补上 CM_Locate_DevNode + CM_Reenumerate_DevNode，效果与官方安装器一致。
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
#include <cstring>
#include <shellapi.h>
#include <setupapi.h>       // SetupDi* 枚举设备
#include <cfgmgr32.h>       // CM_Locate_DevNode / CM_Reenumerate_DevNode
#include <newdev.h>         // UpdateDriverForPlugAndPlayDevices（官方安装器同款）

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "newdev.lib")

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

// ---- 找出所有 WCH(CH34x) 设备的实例 ID ---------------------------------------
// INF 支持的硬件 ID 是 USB\VID_1A86&PID_55D2/55D3/55D4/55D5/55D6/55D7/55D8/55DA…/55DE/55DF，
// 统一按前缀 "USB\VID_1A86&PID_55D" 匹配即可（Makcu 盒子是 PID_55D3）。
struct WchDevice
{
    std::string instanceId;
    std::string hardwareId;     // 喂给 UpdateDriverForPlugAndPlayDevices 的就是这个
    std::string description;
};

std::vector<WchDevice> findWchDevices()
{
    std::vector<WchDevice> out;

    HDEVINFO set = SetupDiGetClassDevsA(nullptr, nullptr, nullptr,
                                        DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return out;

    SP_DEVINFO_DATA dev{};
    dev.cbSize = sizeof(dev);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); ++i)
    {
        char hwids[1024] = {};
        DWORD type = 0, need = 0;
        if (!SetupDiGetDeviceRegistryPropertyA(set, &dev, SPDRP_HARDWAREID, &type,
                                               (PBYTE)hwids, sizeof(hwids), &need))
            continue;

        WchDevice item;
        for (const char* p = hwids; *p; p += strlen(p) + 1)
        {
            if (_strnicmp(p, "USB\\VID_1A86&PID_55D", 21) == 0) { item.hardwareId = p; break; }
        }
        if (item.hardwareId.empty()) continue;

        char inst[512] = {};
        if (SetupDiGetDeviceInstanceIdA(set, &dev, inst, sizeof(inst), nullptr))
            item.instanceId = inst;
        char desc[256] = {};
        if (SetupDiGetDeviceRegistryPropertyA(set, &dev, SPDRP_DEVICEDESC, &type,
                                              (PBYTE)desc, sizeof(desc), nullptr))
            item.description = desc;
        out.push_back(item);
    }

    SetupDiDestroyDeviceInfoList(set);
    return out;
}

// ---- 把驱动强制装到「当前在场的匹配设备」上（～官方安装器的核心动作）---------
// SETUP.EXE 里那句 "安装成功!UpdateDriverForPlugA…" 就是写在调用点上的。
// 它会：把驱动装到所有在场匹配设备 + 重启这些设备 —— 所以**不需要拔插**。
// 只跑 pnputil /add-driver 只是把包放进系统，达不到这个效果。
int forceInstallOnPresentDevices(const std::string& infPath)
{
    int ok = 0;
    for (const WchDevice& d : findWchDevices())
    {
        BOOL needReboot = FALSE;
        const BOOL r = UpdateDriverForPlugAndPlayDevicesA(
            nullptr, d.hardwareId.c_str(), infPath.c_str(),
            INSTALLFLAG_FORCE | INSTALLFLAG_NONINTERACTIVE, &needReboot);
        drvLog("[drv] UpdateDriverForPlugAndPlayDevices(%s) → %s%s",
               d.instanceId.c_str(), r ? "成功" : "失败",
               (r && needReboot) ? "（需重启）" : "");
        if (r) ++ok;
    }
    return ok;
}

// ---- 重枚举设备节点（等价于“拔插一次”）---------------------------------------
bool reenumerateDevices(std::vector<std::string>& done)
{
    done.clear();
    for (const WchDevice& d : findWchDevices())
    {
        DEVINST dev = 0;
        if (CM_Locate_DevNodeA(&dev, (DEVINSTID_A)d.instanceId.c_str(), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
            continue;
        // 同步重枚举：返回时设备已经重新走完 PnP，后面查串口才查得到
        if (CM_Reenumerate_DevNode(dev, CM_REENUMERATE_SYNCHRONOUS) == CR_SUCCESS)
            done.push_back(d.instanceId);
    }
    return !done.empty();
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

// ---- 诊断报告（出问题时把 EXE 目录下的《Makcu驱动报告.txt》发来即可）----------
void appendReportFile(const std::string& text)
{
    FILE* f = fopen((g.runDir + "\\Makcu驱动报告.txt").c_str(), "ab");
    if (!f) return;
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
}

// 现场快照：设备 / 串口 / 服务 / 驱动包 / drivers 文件
std::string collectStateText(const char* title)
{
    std::string s = "---------- ";
    s += title;
    s += " ----------\n";
    s += "时间: ";
    {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        char buf[64];
        sprintf_s(buf, "%04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond);
        s += buf;
    }
    s += "\n提权: ";
    s += isElevated() ? "是" : "否";
    s += "\n\n[WCH 在场设备]\n";

    const std::vector<WchDevice> devs = findWchDevices();
    if (devs.empty())
        s += "  （无 —— 盒子没插，或插在别的机器上）\n";
    for (const WchDevice& d : devs)
    {
        s += "  " + d.instanceId + "\n    描述: " + d.description +
             "\n    硬件ID: " + d.hardwareId;
        DEVINST dn = 0;
        if (CM_Locate_DevNodeA(&dn, (DEVINSTID_A)d.instanceId.c_str(), CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS)
        {
            ULONG status = 0, problem = 0;
            if (CM_Get_DevNode_Status(&status, &problem, dn, 0) == CR_SUCCESS)
            {
                char buf[64];
                sprintf_s(buf, "\n    状态: 0x%08X  问题代码: %lu%s", status, problem,
                          problem == 0 ? "（正常）" : problem == 28 ? "（CM_PROB_FAILED_INSTALL＝没装驱动）" : "");
                s += buf;
            }
        }
        s += "\n";
    }

    s += "\n[串口]\n  ";
    {
        HKEY k = nullptr;
        bool any = false;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &k) == ERROR_SUCCESS)
        {
            for (DWORD i = 0;; ++i)
            {
                char name[256] = {}, val[256] = {};
                DWORD nl = sizeof(name), vl = sizeof(val), type = 0;
                if (RegEnumValueA(k, i, name, &nl, nullptr, &type, (PBYTE)val, &vl) != ERROR_SUCCESS) break;
                s += std::string(val, vl ? vl - 1 : 0) + " ";
                any = true;
            }
            RegCloseKey(k);
        }
        if (!any) s += "（无串口）";
        s += "\n";
    }

    s += "\n[驱动服务 / 文件]\n  服务键: ";
    {
        HKEY k = nullptr;
        bool any = false;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Services", 0, KEY_READ, &k) == ERROR_SUCCESS)
        {
            for (DWORD i = 0;; ++i)
            {
                char name[512] = {};
                DWORD len = sizeof(name);
                if (RegEnumKeyExA(k, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                std::string up(name);
                for (char& c : up) c = (char)toupper((unsigned char)c);
                if (up.find("CH34") != std::string::npos) { s += std::string(name) + " "; any = true; }
            }
            RegCloseKey(k);
        }
        if (!any) s += "（无 CH34* 服务）";
    }
    {
        char sysRoot[MAX_PATH] = {};
        GetSystemDirectoryA(sysRoot, MAX_PATH);
        s += "\n  System32\\drivers\\CH343S64.SYS: ";
        s += fileExists(std::string(sysRoot) + "\\drivers\\CH343S64.SYS") ? "在" : "不在";
    }

    s += "\n\n[驱动包 pnputil /enum-drivers]\n";
    {
        std::string out;
        DWORD code = 0;
        runCapture(system32Path("pnputil.exe").c_str(), "/enum-drivers", code, out);

        // 按空行切块（注意 pnputil 用的是 CRLF），只打含 ch343ser 的块
        bool any = false;
        size_t begin = 0;
        while (begin < out.size())
        {
            size_t end = out.find("\r\n\r\n", begin);
            if (end == std::string::npos) end = out.size();
            const std::string block = out.substr(begin, end - begin);
            std::string low = block;
            for (char& c : low) c = (char)tolower((unsigned char)c);
            if (low.find("ch343ser") != std::string::npos)
            {
                s += "  " + block + "\n";
                any = true;
            }
            begin = end + 4;
        }
        if (!any) s += "  （系统里没有 ch343ser 驱动包）\n";
    }
    s += "\n";
    return s;
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

    // 现场快照写进《Makcu驱动报告.txt》—— 出问题时把这个文件发来就能定位
    appendReportFile(collectStateText("启动时"));

    std::string port;
    if (makcuPortPresent(&port))
    {
        drvLog("[drv] 盒子串口已就绪（%s），无需安装驱动", port.c_str());
        appendReportFile(collectStateText("无需处理（串口已就绪）"));
        return true;
    }

    std::vector<WchDevice> devs = findWchDevices();
    const bool haveDevice = !devs.empty();
    drvLog("[drv] 盒子设备: %s | 驱动已绑定(服务+sys): %s",
           haveDevice ? "已插上" : "没插", driverBound() ? "是" : "否");
    for (const WchDevice& d : devs)
        drvLog("[drv]   在场设备 %s（%s / %s）",
               d.instanceId.c_str(), d.description.c_str(), d.hardwareId.c_str());

    if (driverBound() && !haveDevice)
    {
        drvLog("[drv] 驱动已安装，盒子未插。插上后系统会自动装设备，无需再动程序");
        appendReportFile(collectStateText("无需处理（驱动已装、盒子未插）"));
        return true;
    }

    // 驱动是好的、设备也在，却没出串口 → 多半是设备节点还停在旧状态：重枚举一次就好（不用拔插）
    if (driverBound() && haveDevice)
    {
        std::vector<std::string> reenum;
        reenumerateDevices(reenum);
        drvLog("[drv] 驱动正常但没出串口 → 已重枚举 %d 个设备节点", (int)reenum.size());
        if (makcuPortPresent(&port))
        {
            drvLog("[drv] 重枚举后盒子已识别为 %s（没有拔插）", port.c_str());
            appendReportFile(collectStateText("重枚举后已识别"));
            if (installedNow) *installedNow = true;
            return true;
        }
        drvLog("[drv] 重枚举后仍无串口 → 继续走重装流程");
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

    // ★ 关键一步 1：把驱动强制装到「当前在场」的匹配设备上（官方安装器的核心动作）。
    //   这一步会让设备重新启动，所以盒子插着也能立刻生效、不用拔插。
    //   必须在 cleanupDir 之前调用 —— 它需要 INF 的路径。
    const int forced = forceInstallOnPresentDevices(infPath);
    drvLog("[drv] 强制安装到在场设备：%d 个", forced);

    cleanupDir(dir);

    // ★ 关键一步 2：重枚举设备节点 —— 另一条“等价于拔插”的路（对没走上面那步的情况兜底）
    std::vector<std::string> reenum;
    const bool anyReenum = reenumerateDevices(reenum);
    if (anyReenum)
    {
        std::string joined;
        for (const std::string& s : reenum) joined += (joined.empty() ? "" : ", ") + s;
        drvLog("[drv] 已重枚举 %d 个 WCH 设备节点（无需拔插）：%s",
               (int)reenum.size(), joined.c_str());
    }
    else
    {
        drvLog("[drv] 当前没插着 WCH 设备；插上后系统会自动装设备（可能会提示“正在安装设备驱动软件”）");
    }

    // 兜底：再让系统扫描一遍新硬件（前面两步已经做了，这里是双保险）
    DWORD scanCode = 0;
    std::string scanOut;
    runCapture(pnputil.c_str(), "/scan-devices", scanCode, scanOut);
    drvLog("[drv] pnputil /scan-devices 退出码=%lu", (unsigned long)scanCode);

    // 复查结果
    std::string portAfter;
    if (makcuPortPresent(&portAfter))
        drvLog("[drv] 安装完成，盒子已识别为 %s%s", portAfter.c_str(),
               code == 3010 ? "（建议重启一次）" : "");
    else if (haveDevice)
        drvLog("[drv] 设备在场但还没出串口%s。请把 EXE 目录下的『Makcu驱动报告.txt』发来排查",
               code == 3010 ? "（需重启生效）" : "");
    else
        drvLog("[drv] 安装完成%s；盒子还没插 —— 插上即可用，不需要拔插第二次",
               code == 3010 ? "（需重启生效）" : "");

    appendReportFile(collectStateText("处理之后"));
    if (installedNow) *installedNow = true;
    return true;
}
