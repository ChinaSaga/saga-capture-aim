#define NOMINMAX
#include "App.h"
#include "WebAccess.h"
#include "LanFirewall.h"
#include <iphlpapi.h>
#include <ws2tcpip.h>
#include <shellapi.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <limits>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

namespace {
std::mutex stateMutex;
std::wstring localUrl;
std::wstring displayAddress = L"正在获取";
std::wstring latestReport = L"正在准备本机网页服务，请稍后再查看。";
std::atomic<bool> monitorEnabled{false};

std::filesystem::path executableDirectory()
{
    wchar_t path[32768]{};
    GetModuleFileNameW(nullptr, path, _countof(path));
    return std::filesystem::path(path).parent_path();
}

void publish(const std::wstring& url, const std::wstring& report)
{
    {
        std::lock_guard lock(stateMutex);
        localUrl = url;
        if (latestReport == report) return;
        latestReport = report;
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, report.data(), (int)report.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, report.data(), (int)report.size(), utf8.data(), size, nullptr, nullptr);
    std::ofstream file(executableDirectory() / L"网络自检.txt", std::ios::binary | std::ios::trunc);
    file << "\xef\xbb\xbf" << utf8;
}

SOCKET bindListener(unsigned short port, bool lan, int& error, unsigned short& actualPort)
{
    SOCKET socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socketHandle == INVALID_SOCKET) { error = WSAGetLastError(); return INVALID_SOCKET; }
    BOOL exclusive = TRUE;
    setsockopt(socketHandle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(lan ? INADDR_ANY : INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (bind(socketHandle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(socketHandle, 16) != 0) {
        error = WSAGetLastError();
        closesocket(socketHandle);
        return INVALID_SOCKET;
    }
    int size = sizeof(address);
    if (getsockname(socketHandle, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
        error = WSAGetLastError();
        closesocket(socketHandle);
        return INVALID_SOCKET;
    }
    actualPort = ntohs(address.sin_port);
    error = 0;
    return socketHandle;
}

int probeLocalHttp(unsigned short port)
{
    // Direct Winsock deliberately avoids system/browser proxies and DNS.
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return WSAGetLastError();
    struct CloseSocket { SOCKET value; ~CloseSocket() { closesocket(value); } } cleanup{s};
    u_long nonblocking = 1;
    ioctlsocket(s, FIONBIO, &nonblocking);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (connect(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        const int error = WSAGetLastError();
        if (error != WSAEWOULDBLOCK) return error;
        fd_set writable, errors;
        FD_ZERO(&writable); FD_SET(s, &writable);
        FD_ZERO(&errors); FD_SET(s, &errors);
        timeval timeout{1, 0};
        int selected = select(0, nullptr, &writable, &errors, &timeout);
        if (selected <= 0) return selected == 0 ? WSAETIMEDOUT : WSAGetLastError();
        int status = 0, size = sizeof(status);
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&status), &size) != 0) return WSAGetLastError();
        if (status) return status;
    }
    nonblocking = 0;
    ioctlsocket(s, FIONBIO, &nonblocking);
    DWORD timeout = 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    const std::string request = "GET /__saga_health HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    size_t sent = 0;
    while (sent < request.size()) {
        int size = send(s, request.data() + sent, (int)(request.size() - sent), 0);
        if (size <= 0) return size == 0 ? WSAECONNRESET : WSAGetLastError();
        sent += size;
    }
    std::string response;
    char buffer[1024];
    while (response.size() < 4096) {
        int size = recv(s, buffer, sizeof(buffer), 0);
        if (size == 0) break;
        if (size < 0) return WSAGetLastError();
        response.append(buffer, size);
    }
    const std::string identity = "\"pid\":" + std::to_string(GetCurrentProcessId()) + "}";
    return response.starts_with("HTTP/1.1 200 ") &&
        response.find("\"service\":\"SagaApp\"") != std::string::npos &&
        response.find(identity) != std::string::npos ? 0 : ERROR_INVALID_DATA;
}

struct LanAddresses {
    std::wstring report;
    std::wstring preferredIp;
};

LanAddresses lanAddresses(unsigned short port, bool standardPort)
{
    ULONG size = 16384;
    std::vector<unsigned char> buffer(size);
    ULONG result = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && result == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buffer.resize(size);
        result = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
            GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS, nullptr,
            reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (result != NO_ERROR) return {L"网卡地址读取失败（错误 " + std::to_wstring(result) + L"），可继续使用本机地址。\r\n", L""};
    LanAddresses addresses;
    int bestRank = -1;
    ULONG bestMetric = std::numeric_limits<ULONG>::max();
    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* item = adapter->FirstUnicastAddress; item; item = item->Next) {
            if (!item->Address.lpSockaddr || item->Address.lpSockaddr->sa_family != AF_INET) continue;
            const auto* ip = reinterpret_cast<sockaddr_in*>(item->Address.lpSockaddr);
            wchar_t text[INET_ADDRSTRLEN]{};
            if (!InetNtopW(AF_INET, &ip->sin_addr, text, _countof(text))) continue;
            if (wcsncmp(text, L"127.", 4) == 0 || wcscmp(text, L"0.0.0.0") == 0) continue;
            const bool linkLocal = wcsncmp(text, L"169.254.", 8) == 0;
            const bool wiredOrWifi = adapter->IfType == IF_TYPE_ETHERNET_CSMACD || adapter->IfType == IF_TYPE_IEEE80211;
            bool gateway = false;
            for (auto* entry = adapter->FirstGatewayAddress; entry; entry = entry->Next) {
                if (entry->Address.lpSockaddr && entry->Address.lpSockaddr->sa_family == AF_INET &&
                    reinterpret_cast<sockaddr_in*>(entry->Address.lpSockaddr)->sin_addr.s_addr != INADDR_ANY)
                    gateway = true;
            }
            // Prefer usable LAN addresses and Ethernet/Wi-Fi; break ties using
            // the adapter's default gateway and IPv4 interface metric.
            const int rank = (linkLocal ? 0 : 8) + (wiredOrWifi ? 4 : 0) + (gateway ? 2 : 0);
            if (rank > bestRank || (rank == bestRank && adapter->Ipv4Metric < bestMetric)) {
                bestRank = rank;
                bestMetric = adapter->Ipv4Metric;
                addresses.preferredIp = text;
            }
            addresses.report += std::wstring(adapter->FriendlyName ? adapter->FriendlyName : L"网卡") +
                L"：http://" + text + L":" + std::to_wstring(port) + L"/";
            if (standardPort) addresses.report += L"（也可 http://" + std::wstring(text) + L"/）";
            if (linkLocal) addresses.report += L" [自动分配地址，请检查网线/Wi-Fi/DHCP]";
            addresses.report += L"\r\n";
        }
    }
    if (addresses.report.empty()) addresses.report = L"未发现已连接的局域网 IPv4 地址；离线时仍可使用本机地址。\r\n";
    return addresses;
}

void updateDisplayAddress(const WebListeners& listeners, const std::wstring& preferredIp)
{
    const bool localOnly = !listeners.lan || preferredIp.empty();
    std::wstring address = localOnly ? L"127.0.0.1" : preferredIp;
    if (listeners.standard == INVALID_SOCKET && listeners.port != 80)
        address += L":" + std::to_wstring(listeners.port);
    if (localOnly) address += L"（仅本机）";
    std::lock_guard lock(stateMutex);
    if (monitorEnabled.load()) displayAddress = address;
}
std::wstring firewallServices()
{
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) return L"防火墙服务状态：无法读取。\r\n";
    std::wstring report;
    for (const wchar_t* name : {L"MpsSvc", L"BFE"}) {
        report += std::wstring(name) + L" 服务：";
        SC_HANDLE service = OpenServiceW(manager, name, SERVICE_QUERY_STATUS);
        SERVICE_STATUS_PROCESS status{};
        DWORD needed = 0;
        if (service && QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
            reinterpret_cast<LPBYTE>(&status), sizeof(status), &needed))
            report += status.dwCurrentState == SERVICE_RUNNING ? L"运行中。\r\n" : L"未运行。\r\n";
        else report += L"状态不可读取。\r\n";
        if (service) CloseServiceHandle(service);
    }
    CloseServiceHandle(manager);
    return report;
}
} // namespace

WebListeners startWebListeners(unsigned short preferredPort)
{
    WebListeners result;
    int error = 0;
    // First choose a usable LAN port. If wildcard binding is denied, still try
    // loopback. Port zero asks Windows for an unoccupied, non-reserved port.
    for (bool lan : {true, false}) {
        for (int offset = 0; offset < 11 && result.primary == INVALID_SOCKET; ++offset) {
            const unsigned int port = preferredPort + offset;
            if (port > 65535) break;
            result.primary = bindListener((unsigned short)port, lan, error, result.port);
            if (offset == 0 && result.primary == INVALID_SOCKET)
                result.notes += std::wstring(lan ? L"局域网" : L"本机") + L"端口 " +
                    std::to_wstring(port) + L" 不可用（错误 " + std::to_wstring(error) + L"），尝试备用端口。\r\n";
        }
        if (result.primary == INVALID_SOCKET) result.primary = bindListener(0, lan, error, result.port);
        if (result.primary != INVALID_SOCKET) { result.lan = lan; break; }
    }
    if (result.primary == INVALID_SOCKET) {
        result.notes += L"无法建立本机监听，Windows 错误 " + std::to_wstring(error) + L"。\r\n";
        return result;
    }
    if (result.port != preferredPort) result.notes += L"已自动切换到可用端口 " + std::to_wstring(result.port) + L"。\r\n";
    if (result.lan && result.port != 80) {
        unsigned short actual = 0;
        result.standard = bindListener(80, true, error, actual);
        if (result.standard == INVALID_SOCKET)
            result.notes += L"80 端口不可用（错误 " + std::to_wstring(error) + L"），请使用带端口的地址。\r\n";
    }
    publish(L"http://127.0.0.1:" + std::to_wstring(result.port) + L"/",
        L"网页监听已启动，正在检测本机连接与防火墙设置。\r\n" + result.notes);
    monitorEnabled = true;
    // Resolve before firewall preflight so the title does not wait for it.
    updateDisplayAddress(result, result.lan ?
        lanAddresses(result.port, result.standard != INVALID_SOCKET).preferredIp : L"");
    return result;
}

void closeWebListeners(WebListeners& listeners)
{
    monitorEnabled = false;
    if (listeners.primary != INVALID_SOCKET) closesocket(listeners.primary);
    if (listeners.standard != INVALID_SOCKET) closesocket(listeners.standard);
    listeners.primary = listeners.standard = INVALID_SOCKET;
}

void webAccessFailed(const std::wstring& reason)
{
    monitorEnabled = false;
    { std::lock_guard lock(stateMutex); displayAddress = L"服务未就绪"; }
    publish(L"", L"本机网页服务未就绪\r\n" + reason +
        L"请检查系统网络服务或安全软件限制。主界面和采集功能仍可使用。\r\n");
}

void webAccessMonitor(WebListeners listeners)
{
    std::wstring details;
    const std::wstring ports = std::to_wstring(listeners.port) +
        (listeners.standard != INVALID_SOCKET ? L",80" : L"");
    // Firewall APIs are kept off the accept/UI threads, including on systems
    // whose firewall service is stopped or managed by organization policy.
    const HRESULT firewall = configureLanFirewall(ports, &details);
    details += firewallServices();
    wchar_t code[16]; swprintf_s(code, L"0x%08lX", (unsigned long)firewall);
    std::wstring firewallText = SUCCEEDED(firewall)
        ? L"防火墙预设：已配置当前 EXE 的 TCP " + ports + L" 局域网规则（域/专用/公用网络，仅本地子网）。\r\n"
        : L"防火墙预设失败：" + std::wstring(code) + L"。请检查管理员权限、Windows 防火墙/BFE 服务或系统策略。\r\n";
    const auto directory = executableDirectory();
    const auto page = directory / L"圣人视觉识别系统.html";
    const std::wstring url = L"http://127.0.0.1:" + std::to_wstring(listeners.port) + L"/";
    while (g.running.load() && monitorEnabled.load()) {
        int probe = probeLocalHttp(listeners.port);
        if (!monitorEnabled.load()) return;
        std::ifstream html(page, std::ios::binary);
        const bool hasPage = html && html.peek() != std::char_traits<char>::eof();
        html.close();
        std::wstring report = L"本机网络自检（自动更新，报告保存在 EXE 同目录的 网络自检.txt）\r\n\r\n";
        report += L"本机调参地址：" + url + L"\r\n";
        report += probe == 0 ? L"本机 HTTP 自检：通过，已收到当前程序的响应。\r\n"
            : L"本机 HTTP 自检：失败，错误 " + std::to_wstring(probe) + L"，请检查本机安全软件。\r\n";
        report += hasPage ? L"网页文件：可以读取。\r\n" :
            L"网页文件：网页不存在、为空或不可读取；将可读的 圣人视觉识别系统.html 放在 EXE 同目录后刷新。\r\n";
        report += listeners.notes;
        report += listeners.lan ? L"监听范围：本机和局域网 IPv4。\r\n\r\n" :
            L"监听范围：仅本机（系统拒绝局域网监听），本机调参仍可使用上面的地址。\r\n\r\n";
        const auto addresses = listeners.lan ? lanAddresses(listeners.port, listeners.standard != INVALID_SOCKET) : LanAddresses{};
        updateDisplayAddress(listeners, addresses.preferredIp);
        if (listeners.lan) report += L"当前网卡访问地址：\r\n" + addresses.report + L"\r\n";
        report += firewallText + details;
        report += L"\r\n本机检测通过不代表其他设备一定可达。其他设备无法连接时，请检查是否在同一子网、访客 Wi-Fi/路由器客户端隔离、VPN/代理或第三方安全软件。\r\n"
                  L"本机浏览器请使用上面的 http://127.0.0.1 地址（不要改成 https）；若本机自检通过但浏览器失败，请检查浏览器代理是否绕过本地地址。";
        publish(url, report);
        for (int i = 0; i < 20 && g.running.load() && monitorEnabled.load(); ++i) Sleep(250);
    }
}

std::wstring webAccessAddress()
{
    std::lock_guard lock(stateMutex);
    return displayAddress;
}

void webShowDiagnostics(HWND owner)
{
    std::wstring report;
    { std::lock_guard lock(stateMutex); report = latestReport; }
    MessageBoxW(owner, report.c_str(), L"本机网络检查", MB_OK | MB_ICONINFORMATION);
}

void webOpenLocalPage(HWND owner)
{
    std::wstring url;
    { std::lock_guard lock(stateMutex); url = localUrl; }
    if (url.empty()) { webShowDiagnostics(owner); return; }
    if ((INT_PTR)ShellExecuteW(owner, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL) <= 32)
        MessageBoxW(owner, (L"无法打开默认浏览器，请在浏览器中访问：\r\n" + url).c_str(), L"本机调参", MB_OK | MB_ICONINFORMATION);
}
