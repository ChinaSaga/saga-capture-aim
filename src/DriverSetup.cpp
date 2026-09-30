// CH343 driver bootstrap: extract the embedded signed package, stage it, bind
// exact INF hardware IDs to present devices, then wait for the COM interface.
// A staged package or a successful bus scan is not proof of a usable device.
// Driver repair and protocol handshaking run after the main window is shown.
// Administrator rights are supplied by the application manifest.
#include "App.h"
#include "RuntimeLog.h"
#include "WchHardwareId.h"
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
#define IDR_MAKCU_DRV_INSTALLER     410

namespace
{

enum class StartupState { Checking, Repairing, Connecting, Ready, Absent, Failed };
std::atomic<StartupState> startupState{StartupState::Checking};

struct DrvFile
{
    int         id;
    const char* name;
};

// Nine INF payloads plus the original signed WCH x64 installer.
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
    { IDR_MAKCU_DRV_INSTALLER,     "DRVSETUP64\\DRVSETUP64.exe" },
};

const char* kInfName = "CH343SER.INF";

// ---- 日志 ------------------------------------------------------------------
// 所有驱动诊断进入统一后台队列，避免设备初始化线程直接写盘。
void drvLog(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    runtime_log::writeV(fmt, ap);
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

// The signed x64 installer bundled inside CH343SER.EXE documents /S as
// implicit installation (not /P, which only preinstalls the package).
// Preserve the original package layout: on x64 Windows the installer strips
// two components off its module path to find the INF. CWD alone cannot fix it.
// root/CH343SER.INF, root/<payloads>, root/DRVSETUP64/DRVSETUP64.exe.
bool runOfficialInstaller(const std::string& dir, DWORD& code)
{
    const std::string exe = dir + "\\DRVSETUP64\\DRVSETUP64.exe";
    if (!fileExists(dir + "\\" + kInfName) || !fileExists(exe)) {
        code = ERROR_FILE_NOT_FOUND;
        drvLog("[drv] official installer layout incomplete: %s", dir.c_str());
        return false;
    }
    const std::string command = "\"" + exe + "\" /S";
    std::vector<char> args(command.begin(), command.end());
    args.push_back(0);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    code = ERROR_SUCCESS;
    if (!CreateProcessA(exe.c_str(), args.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi)) {
        code = GetLastError();
        drvLog("[drv] official installer launch failed: error=%lu", code);
        return false;
    }
    CloseHandle(pi.hThread);
    // Do not remove the extracted files or start a second installer if this
    // one has not finished. It may still be modifying a device's driver.
    const DWORD wait = WaitForSingleObject(pi.hProcess, 120000);
    const bool finished = wait == WAIT_OBJECT_0;
    if (!finished) code = wait == WAIT_TIMEOUT ? WAIT_TIMEOUT : GetLastError();
    else if (!GetExitCodeProcess(pi.hProcess, &code)) code = GetLastError();
    CloseHandle(pi.hProcess);
    drvLog("[drv] official DRVSETUP64.exe /S: finished=%d code=%lu", finished, code);
    return finished;
}

// ---- 盒子的串口出现了没有（最直接的“能用”判据）------------------------------
bool makcuPortPresent(std::string* portOut)
{
    std::vector<std::string> ports = makcu::SerialPort::findMakcuPorts();
    if (!ports.empty() && portOut) *portOut = ports.front();
    return !ports.empty();
}

bool waitForMakcuPort(std::string* portOut, DWORD timeoutMs)
{
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    do {
        if (makcuPortPresent(portOut)) return true;
        if (GetTickCount64() >= deadline) return false;
        Sleep(250);
    } while (true);
}

// ---- 找出所有 WCH(CH34x) 设备的实例 ID ---------------------------------------
// Exact supported IDs are shared with serial-port discovery in WchHardwareId.h.
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
        if (type == REG_MULTI_SZ)
            if (const char* id = wch::findSupportedHardwareId(hwids, need))
                item.hardwareId = id;
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
int forceInstallOnPresentDevices(const std::string& infPath, bool& rebootRequired)
{
    int ok = 0;
    for (const WchDevice& d : findWchDevices())
    {
        BOOL needReboot = FALSE;
        const BOOL r = UpdateDriverForPlugAndPlayDevicesA(
            nullptr, d.hardwareId.c_str(), infPath.c_str(),
            INSTALLFLAG_FORCE | INSTALLFLAG_NONINTERACTIVE, &needReboot);
        const DWORD error = r ? ERROR_SUCCESS : GetLastError();
        rebootRequired = rebootRequired || needReboot != FALSE;
        drvLog("[drv] UpdateDriverForPlugAndPlayDevices(%s) → %s%s error=%lu",
               d.instanceId.c_str(), r ? "成功" : "失败",
               needReboot ? "（需重启）" : "", error);
        if (r) ++ok;
    }
    return ok;
}

// Recovery after installing: request a real stop/start of only matching
// devices. Reenumerating a leaf devnode alone does not restart its driver.
void restartPresentDevices(bool& rebootRequired)
{
    HDEVINFO set = SetupDiCreateDeviceInfoList(nullptr, nullptr);
    if (set == INVALID_HANDLE_VALUE) return;
    for (const WchDevice& device : findWchDevices()) {
        SP_DEVINFO_DATA data{};
        data.cbSize = sizeof(data);
        if (!SetupDiOpenDeviceInfoA(set, device.instanceId.c_str(), nullptr, 0, &data)) {
            drvLog("[drv] restart open failed: %s error=%lu", device.instanceId.c_str(), GetLastError());
            continue;
        }
        SP_PROPCHANGE_PARAMS change{};
        change.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
        change.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
        change.StateChange = DICS_PROPCHANGE;
        change.Scope = DICS_FLAG_CONFIGSPECIFIC;
        const BOOL ok = SetupDiSetClassInstallParamsA(set, &data,
            &change.ClassInstallHeader, sizeof(change)) &&
            SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &data);
        const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
        SP_DEVINSTALL_PARAMS_A params{};
        params.cbSize = sizeof(params);
        if (SetupDiGetDeviceInstallParamsA(set, &data, &params))
            rebootRequired = rebootRequired || (params.Flags & (DI_NEEDRESTART | DI_NEEDREBOOT)) != 0;
        drvLog("[drv] device restart: %s success=%d error=%lu reboot=%d",
            device.instanceId.c_str(), ok, error, rebootRequired);
    }
    SetupDiDestroyDeviceInfoList(set);
}

// ---- Scan the parent bus; this is not a physical unplug/replug. --------------
bool reenumerateDevices(std::vector<std::string>& done)
{
    done.clear();
    for (const WchDevice& d : findWchDevices())
    {
        DEVINST dev = 0;
        if (CM_Locate_DevNodeA(&dev, (DEVINSTID_A)d.instanceId.c_str(), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
            continue;
        // Enumerate the parent bus. Returning does not guarantee the COM
        // interface is ready; callers must wait for it separately.
        DEVINST parent = 0;
        if (CM_Get_Parent(&parent, dev, 0) != CR_SUCCESS) parent = dev;
        const CONFIGRET result = CM_Reenumerate_DevNode(parent, CM_REENUMERATE_SYNCHRONOUS);
        drvLog("[drv] reenumerate(%s): CR=0x%lx", d.instanceId.c_str(), result);
        if (result == CR_SUCCESS)
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

// ---- 诊断报告（与其他事件一起保存在 EXE 目录的《运行日志.txt》）------------
void appendReportFile(const std::string& text)
{
    runtime_log::write("%s", text.c_str());
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
    const std::string installerDir = dir + "\\DRVSETUP64";
    if (!CreateDirectoryA(installerDir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
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
    RemoveDirectoryA((dir + "\\DRVSETUP64").c_str());
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
    return findWchDevices().empty() && driverBound();
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

    std::vector<WchDevice> devs = findWchDevices();
    bool haveDevice = !devs.empty();
    const bool bound = driverBound();
    drvLog("[drv] 盒子设备: %s | 驱动已绑定(服务+sys): %s",
           haveDevice ? "已插上" : "没插", bound ? "是" : "否");
    // An unplugged box is not a broken driver. Do not reinstall, scan buses,
    // collect pnputil reports or wait for a COM interface that cannot appear.
    if (!haveDevice) {
        drvLog("[drv] 未发现盒子，跳过安装、硬件扫描及串口等待%s",
            bound ? "（已有驱动保留）" : "（连接设备后重新启动可自动安装）");
        return bound;
    }
    startupState = StartupState::Repairing;
    // Full reports spawn pnputil; only collect them when a present device
    // actually needs driver repair, never on a healthy or unplugged startup.
    appendReportFile(collectStateText("设备在场但串口未就绪，准备修复"));
    for (const WchDevice& d : devs)
        drvLog("[drv]   在场设备 %s（%s / %s）",
               d.instanceId.c_str(), d.description.c_str(), d.hardwareId.c_str());

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
        return false;
    }
    drvLog("[drv] 已释放 %d 个驱动文件到 %s", (int)(sizeof(kDrvFiles) / sizeof(kDrvFiles[0])), dir.c_str());

    DWORD officialCode = 0;
    if (!runOfficialInstaller(dir, officialCode)) {
        // Preserve files while the installer may still be running.
        drvLog("[drv] official installer incomplete; files retained: %s", dir.c_str());
        appendReportFile(collectStateText("官方安装器未完成"));
        return false;
    }
    if (installedNow) *installedNow = true;
    if (waitForMakcuPort(&port, 10000)) {
        drvLog("[drv] official installation: port ready=%s", port.c_str());
        cleanupDir(dir);
        appendReportFile(collectStateText("官方安装器完成，串口已就绪"));
        return true;
    }
    haveDevice = haveDevice || !findWchDevices().empty();
    drvLog("[drv] official installation returned but no usable port; attempting PnP recovery");

    const std::string pnputil = system32Path("pnputil.exe");
    const std::string infPath = dir + "\\" + kInfName;

    DWORD code = 0;
    std::string out;
    bool ran = runCapture(pnputil.c_str(),
                          "/add-driver \"" + infPath + "\" /install", code, out);
    drvLog("[drv] pnputil /add-driver 退出码=%lu", (unsigned long)code);
    if (!out.empty()) drvLog("[drv] pnputil 输出:\n%s", out.c_str());

    // 3010/3011 indicate a required system restart/service restart, not "already exists".
    bool rebootRequired = code == ERROR_SUCCESS_REBOOT_REQUIRED ||
                          code == ERROR_SUCCESS_RESTART_REQUIRED;
    const bool ok = ran && (code == ERROR_SUCCESS || rebootRequired);
    if (!ok)
    {
        drvLog("[drv] 安装失败，驱动文件保留在 %s 供排查", dir.c_str());
        return false;
    }

    // ★ 关键一步 1：把驱动强制装到「当前在场」的匹配设备上（官方安装器的核心动作）。
    //   这一步会让设备重新启动，所以盒子插着也能立刻生效、不用拔插。
    //   必须在 cleanupDir 之前调用 —— 它需要 INF 的路径。
    const int forced = forceInstallOnPresentDevices(infPath, rebootRequired);
    drvLog("[drv] 强制安装到在场设备：%d 个", forced);

    if (!waitForMakcuPort(&port, 3000)) restartPresentDevices(rebootRequired);

    cleanupDir(dir);

    // Scan the parent bus after binding; COM readiness is checked below.
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
    const bool portReady = waitForMakcuPort(&portAfter, haveDevice ? 15000 : 1000);
    if (portReady)
        drvLog("[drv] 安装完成，盒子已识别为 %s%s", portAfter.c_str(),
               rebootRequired ? "（系统报告需要重启）" : "");
    else if (haveDevice)
        drvLog("[drv] 设备在场但还没出串口%s。请把 EXE 目录下的『运行日志.txt』发来排查",
               rebootRequired ? "（需重启生效）" : "");
    else
        drvLog("[drv] 安装完成%s；盒子还没插 —— 插上即可用，不需要拔插第二次",
               rebootRequired ? "（需重启生效）" : "");

    appendReportFile(collectStateText("处理之后"));
    if (installedNow) *installedNow = true;
    return portReady || (!haveDevice && findWchDevices().empty() && !rebootRequired);
}

void makcuStartupThread()
{
    const ULONGLONG started = GetTickCount64();
    try {
        bool installed = false;
        const bool driverReady = ensureMakcuDriver(&installed);
        drvLog("[05b] CH343 驱动 %s%s，后台用时 %llums", driverReady ? "就绪" : "未就绪",
            installed ? "（本次已安装）" : "", GetTickCount64() - started);

        // No device means no serial probes, and in particular no ten-second
        // retry loop. A present device still gets the existing bounded retry.
        if (!makcuPortPresent(nullptr)) {
            startupState = findWchDevices().empty() ? StartupState::Absent : StartupState::Failed;
            drvLog("[06] 无可用盒子串口，跳过连接等待；后台初始化用时 %llums", GetTickCount64() - started);
            return;
        }
        startupState = StartupState::Connecting;
        const ULONGLONG deadline = GetTickCount64() + 10000;
        bool connected = false;
        do {
            connected = makcuConnect(0);
            if (connected || !g.running.load() || !makcuPortPresent(nullptr) || GetTickCount64() >= deadline) break;
            Sleep(500);
        } while (g.running.load());
        startupState = connected ? StartupState::Ready : StartupState::Failed;
        drvLog("[06] makcuConnect %s；后台初始化用时 %llums", connected ? "成功" : "失败（未收到有效设备应答）",
            GetTickCount64() - started);
    } catch (const std::exception& error) {
        startupState = StartupState::Failed;
        drvLog("[hw] 后台初始化异常：%s", error.what());
    }
}

const char* makcuStartupStatus()
{
    switch (startupState.load()) {
    case StartupState::Checking: return "MAKCU 检测中";
    case StartupState::Repairing: return "MAKCU 安装驱动中";
    case StartupState::Connecting: return "MAKCU 连接中";
    case StartupState::Ready: return saga::makcuIsConnected() ? "MAKCU 已连接" : "MAKCU 已断开";
    case StartupState::Absent: return "MAKCU 未插入";
    default: return "MAKCU 异常，见日志";
    }
}
