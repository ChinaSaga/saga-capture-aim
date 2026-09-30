#include "App.h"
#include "RuntimeLog.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include "WebAccess.h"

// ============================================================================
//  配置哈希表（替代易语言 哈希表_ASM）
// ============================================================================
static std::map<std::string, std::string> g_hash;
// 必须是可重入锁：configSyncAll 持锁期间会反复调用 hashGet()
static std::recursive_mutex g_hashMutex;
static std::string g_outBuf;                       // 集_写出内容
static std::map<std::string, std::string> g_webConfig;
static std::string jsonEscape(const std::string& s);

static std::vector<std::string> splitStr(const std::string& s, const std::string& sep)
{
    std::vector<std::string> r;
    size_t start = 0;
    for (;;)
    {
        size_t p = s.find(sep, start);
        if (p == std::string::npos) { r.push_back(s.substr(start)); break; }
        r.push_back(s.substr(start, p - start));
        start = p + sep.size();
    }
    return r;
}

static std::string trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::string stripQuotes(const std::string& s)
{
    std::string r;
    for (char c : s) if (c != '"' && c != '\'') r += c;
    return r;
}

void configInit()
{
    std::lock_guard<std::recursive_mutex> lk(g_hashMutex);
    g_hash.clear();

    std::string raw = readFileAll(g.cfgPath);
    runtime_log::write("[cfg] path=%s rawSize=%zu", g.cfgPath.c_str(), raw.size());
    if (raw.empty()) return;
    std::string text = utf8ToAnsi(raw);
    // 去掉可能的 UTF-8 BOM
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        text = text.substr(3);

    auto lines = splitStr(text, "\n");
    for (auto& ln : lines)
    {
        std::string line = trim(ln);
        if (line.empty()) continue;
        auto kv = splitStr(line, "=");
        if (kv.size() == 2)
            g_hash[trim(kv[0])] = stripQuotes(trim(kv[1]));
    }
}

static std::string hashGet(const std::string& k)
{
    std::lock_guard<std::recursive_mutex> lk(g_hashMutex);
    auto it = g_hash.find(k);
    return (it == g_hash.end()) ? std::string() : it->second;
}

static void hashSet(const std::string& k, const std::string& v)
{
    std::lock_guard<std::recursive_mutex> lk(g_hashMutex);
    g_hash[k] = v;
}

void configApplyStartupDefaults()
{
    // 易语言原版只在启动阶段执行一次；网页运行期修改其它参数的行为不受影响。
    hashSet("本地端口", "8888");
}

static void bufInt(const char* name, int& v)
{
    v = atoi(hashGet(name).c_str());
    g_webConfig[name] = std::to_string(v);
    g_outBuf += name;
    g_outBuf += "=";
    g_outBuf += std::to_string(v);
    g_outBuf += "\n";
}

static void bufStr(const char* name, std::string& v)
{
    v = hashGet(name);
    g_webConfig[name] = (v == "true" || v == "false") && std::string(name) != "模型名称"
        ? v : "\"" + jsonEscape(v) + "\"";
    if (name == std::string("模型名称")) return;      // 原版：读了但不写回缓冲
    if (name == std::string("移动延时")) return;
    g_outBuf += name;
    g_outBuf += "=";
    g_outBuf += v;
    g_outBuf += "\n";
}

static void bufFlt(const char* name, float& v)
{
    v = (float)atof(hashGet(name).c_str());
    g_outBuf += name;
    g_outBuf += "=";
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%g", (double)v);
    g_webConfig[name] = std::isfinite(v) ? tmp : "0";
    g_outBuf += tmp;
    g_outBuf += "\n";
}

// 变量全部对应一次
void configSyncAll(bool save)
{
    std::lock_guard<std::recursive_mutex> lk(g_hashMutex);
    g_outBuf.clear();
    g_webConfig.clear();

    bufInt("本地端口", g.localPort);
    bufInt("移动方式", g.moveMode);
    bufInt("优先锁定", g.prioLock);
    bufInt("其次锁定", g.secondLock);
    bufInt("再次锁定", g.thirdLock);
    bufInt("锁定范围", g.lockRange);
    bufInt("优先锁定位置", g.prioLockPos);
    bufInt("其次锁定位置", g.secondLockPos);
    bufInt("再次锁定位置", g.thirdLockPos);
    bufInt("自瞄主键", g.aimMainKey);
    bufInt("自瞄副键", g.aimSubKey1);
    bufInt("自瞄副键2", g.aimSubKey2);
    bufInt("自瞄副键3", g.aimSubKey3);
    bufInt("准星绑定1", g.crossBind[0]);
    bufInt("准星绑定2", g.crossBind[1]);
    bufInt("准星绑定3", g.crossBind[2]);
    bufInt("准星绑定4", g.crossBind[3]);
    bufInt("扳机热键1", g.triggerKey[0]);
    bufInt("扳机热键2", g.triggerKey[1]);
    bufInt("扳机热键3", g.triggerKey[2]);
    bufInt("扳机热键4", g.triggerKey[3]);
    bufInt("推理引擎", g.engine);
    bufInt("准星类别", g.crossClass);
    bufInt("开镜时间", g.adsTime);
    bufInt("开枪间隔", g.shotInterval);
    bufInt("锁定频率", g.lockFreq);
    bufInt("延时频率", g.delayFreq);
    int campTmp = g.camp.load();
    bufInt("目前阵容", campTmp);
    g.camp = campTmp;
    bufInt("中心点范围", g.centerRange);

    bufStr("模型名称", g.modelName);
    bufStr("框内移动", g.moveInBox);
    bufStr("线程移动", g.threadMove);
    bufStr("自瞄连续按下", g.aimContinuous);
    bufStr("自动截图", g.autoShot);
    bufStr("吸附常驻", g.alwaysOn);
    bufStr("准星扳机", g.crossTrigger);
    bufStr("中心扳机", g.centerTrigger);

    // 原版写出顺序：type0~9 一组，team1type0~9 一组，team2type0~9 一组
    char k[32];
    for (int i = 0; i < 10; ++i) { snprintf(k, sizeof(k), "type%d", i);      bufStr(k, g.type[i]); }
    for (int i = 0; i < 10; ++i) { snprintf(k, sizeof(k), "team1type%d", i); bufStr(k, g.team1[i]); }
    for (int i = 0; i < 10; ++i) { snprintf(k, sizeof(k), "team2type%d", i); bufStr(k, g.team2[i]); }

    bufInt("移动延时", g.moveDelay);
    bufFlt("置信度", g.conf);
    bufFlt("准星精度", g.crossConf);
    bufFlt("交并比", g.nms);
    bufFlt("移动系数", g.moveFactor);
    bufFlt("Kp", g.kp);
    bufFlt("Ki", g.ki);
    bufFlt("扳机范围", g.triggerRange);

    if (save) configSaveFile();
}

void configSaveFile()
{
    g_outBuf += "模型名称=\"";
    g_outBuf += g.modelName;
    g_outBuf += "\"\n";

    g_outBuf += "服务器ip=\"";
    g_outBuf += g.hostIp;
    g_outBuf += "\"\n";

    // 与易语言写出的文件保持一致：换行统一为 CRLF，内容转 UTF-8
    std::string crlf;
    crlf.reserve(g_outBuf.size() + 64);
    for (char c : g_outBuf)
    {
        if (c == '\n') crlf += "\r\n";
        else crlf += c;
    }
    writeFileAll(g.cfgPath, ansiToUtf8(crlf));
    g_outBuf.clear();
}

// ============================================================================
//  HTTP 服务（Winsock 极简实现，替代 HPSocket）
// ============================================================================
static std::string jsonEscape(const std::string& s)
{
    std::string r;
    for (char c : s)
    {
        if (c == '"' || c == '\\') { r += '\\'; r += c; }
        else if (static_cast<unsigned char>(c) < 0x20) {
            char escaped[7];
            snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned char>(c));
            r += escaped;
        }
        else r += c;
    }
    return r;
}

// extraKey 非空时插入一个额外字段（原版是在同一个 JSON 上继续 置属性）
static std::string buildJsonBody(const std::string& extraKey = "",
                                 const std::string& extraVal = "")
{
    std::string j = "{";
    j += "\"推理帧率\":\"" + std::to_string(g.inferFps) + "\",";
    // Latency-derived capacity is separate from captured frames processed per second.
    const double inferMs = g.inferMsPrecise;
    const bool active = g.running.load() && !g.inferPaused.load()
        && nowMs() - g_lastFrameTime < 1000;
    const double inferenceSpeed = active && inferMs > 0.0 ? 1000.0 / inferMs : 0.0;
    j += "\"推理速度FPS\":" + std::to_string(inferenceSpeed) + ",";
    j += "\"推理耗时毫秒\":" + std::to_string(active ? inferMs : 0.0) + ",";
    j += "\"目前阵容\":\"" + std::to_string(g.camp.load()) + "\",";
    if (!extraKey.empty())
        j += "\"" + extraKey + "\":\"" + jsonEscape(extraVal) + "\",";

    std::string hot;                       // 原版：热键1:真   热键2:假 ...
    const std::string sep = "   ";
    for (int i = 1; i <= 6; ++i)
    {
        bool st = false;
        makcuState(i, st);
        hot += "热键" + std::to_string(i) + ":" + (st ? "真" : "假");
        if (i != 6) hot += sep;
    }
    j += "\"热键文本\":\"" + jsonEscape(hot) + "\"";
    j += "}";
    return j;
}

static void sendResponse(SOCKET c, int code, const std::string& body,
                         const char* contentType = "application/json; charset=utf-8")
{
    const char* reason = code == 200 ? "OK" : code == 201 ? "Created" :
        code == 400 ? "Bad Request" : code == 404 ? "Not Found" :
        code == 405 ? "Method Not Allowed" : "Internal Server Error";
    std::string head = "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\n";
    head += std::string("Content-Type: ") + contentType + "\r\n";
    head += "Cache-Control: no-store\r\n";
    head += "Access-Control-Allow-Origin: *\r\n";
    head += "Access-Control-Allow-Headers: *\r\n";
    head += "Access-Control-Allow-Methods: POST, GET, OPTIONS\r\n";
    head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    head += "Connection: close\r\n\r\n";

    std::string all = head + body;
    size_t offset = 0;
    while (offset < all.size()) {
        int sent = send(c, all.data() + offset, (int)(all.size() - offset), 0);
        if (sent <= 0) break;
        offset += sent;
    }
}

static std::string webConfigScript()
{
    std::lock_guard<std::recursive_mutex> lock(g_hashMutex);
    std::string script = "Object.assign(window,{";
    bool first = true;
    for (const auto& [key, value] : g_webConfig) {
        if (!first) script += ',';
        first = false;
        script += "\"" + jsonEscape(key) + "\":" + value;
    }
    script += "});\n";
    return ansiToUtf8(script);
}

static void sendMissingWebPanel(SOCKET c)
{
    sendResponse(c, 404, ansiToUtf8(
        "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
        "<title>网页不存在</title></head><body><h1>网页不存在</h1>"
        "<p>请将 圣人视觉识别系统.html 放到 EXE 同目录，然后刷新此页面。</p>"
        "</body></html>"), "text/html; charset=utf-8");
}

static void handleRequest(SOCKET c, const std::string& req)
{
    // 分离头部与 body
    size_t sep = req.find("\r\n\r\n");
    std::string head = (sep == std::string::npos) ? req : req.substr(0, sep);
    std::string body = (sep == std::string::npos) ? "" : req.substr(sep + 4);

    bool isPost = (head.size() >= 4 && head.compare(0, 4, "POST") == 0);
    bool isOpt = (head.size() >= 7 && head.compare(0, 7, "OPTIONS") == 0);
    std::istringstream requestLine(head);
    std::string method, path;
    requestLine >> method >> path;
    path = path.substr(0, path.find('?'));
    if (method == "GET" && path == "/__saga_health") {
        sendResponse(c, 200, "{\"service\":\"SagaApp\",\"pid\":" + std::to_string(GetCurrentProcessId()) + "}");
        return;
    }
    // The panel is supplied alongside the EXE. Never fall back to an embedded
    // page or a different folder; stale browser tabs must not tune without it.
    const std::string panelPath = g.runDir + "\\圣人视觉识别系统.html";
    if (!fileExists(panelPath)) {
        sendMissingWebPanel(c);
        return;
    }
    if (method == "GET" && path == "/config.js") {
        sendResponse(c, 200, webConfigScript(), "application/javascript; charset=utf-8");
        return;
    }
    if (method == "GET" && (path == "/" || path == "/index.html") &&
        head.find("application/json") == std::string::npos) {
        const std::string page = readFileAll(panelPath);
        if (page.empty()) sendMissingWebPanel(c);
        else sendResponse(c, 200, page, "text/html; charset=utf-8");
        return;
    }
    if (path != "/" && path != "/api") {
        sendResponse(c, 404, "{\"error\":\"Not found\"}");
        return;
    }
    if (method != "GET" && !isPost && !isOpt) {
        sendResponse(c, 405, "{\"error\":\"Method not allowed\"}");
        return;
    }

    std::string extra;
    if (isPost)
    {
        // 按 Content-Length 补齐 body
        size_t cl = head.find("Content-Length:");
        if (cl == std::string::npos) cl = head.find("content-length:");
        if (cl != std::string::npos)
        {
            int want = atoi(head.c_str() + cl + 15);
            int have = (int)body.size();
            while (have < want)
            {
                char buf[4096];
                int n = recv(c, buf, sizeof(buf), 0);
                if (n <= 0) break;
                body.append(buf, n);
                have += n;
            }
        }
    }

    if (isOpt)
    {
        sendResponse(c, 200, "");
        return;
    }

    std::string payload = utf8ToAnsi(body);
    auto parts = splitStr(payload, "|");

    std::string json = buildJsonBody();

    if (parts.size() == 2)
    {
        std::lock_guard<std::recursive_mutex> lock(g_hashMutex);
        const std::string& cmd = trim(parts[0]);
        const std::string& arg = trim(parts[1]);
        if ((cmd == "保存配置" || cmd == "删除配置" || cmd == "导入配置") &&
            (arg.empty() || arg.size() > 128 || arg == "." || arg == ".." ||
             arg.find_first_of("\\/:*?\"<>|") != std::string::npos)) {
            sendResponse(c, 400, "{\"error\":\"Invalid configuration name\"}");
            return;
        }

        std::string cfgDir = g.runDir + "\\配置保存";
        CreateDirectoryA(cfgDir.c_str(), nullptr);

        if (cmd == "获取所有配置名")
        {
            std::string names;
            WIN32_FIND_DATAA fd;
            std::string pat = cfgDir + "\\*.*";
            HANDLE hf = FindFirstFileA(pat.c_str(), &fd);
            if (hf != INVALID_HANDLE_VALUE)
            {
                do
                {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    std::string n = fd.cFileName;
                    size_t dot = n.rfind('.');
                    if (dot != std::string::npos) n = n.substr(0, dot);
                    names += n + ",";
                } while (FindNextFileA(hf, &fd));
                FindClose(hf);
            }
            // 原版 文本_删右边 (局_返回文件名, 1)：去掉最后一个逗号
            if (!names.empty() && names.back() == ',') names.pop_back();
            // 原版是在同一份 JSON 上追加 配置列表，而不是替换
            json = buildJsonBody("配置列表", names);
        }
        else if (cmd == "保存配置")
        {
            CopyFileA(g.cfgPath.c_str(), (cfgDir + "\\" + arg + ".js").c_str(), FALSE);
            // 原版是在同一份 JSON 上追加 是否刷新（仍带 推理帧率/目前阵容/热键文本）
            json = buildJsonBody("是否刷新", "是");
        }
        else if (cmd == "删除配置")
        {
            DeleteFileA((cfgDir + "\\" + arg + ".js").c_str());
            json = buildJsonBody("是否刷新", "是");
        }
        else if (cmd == "导入配置")
        {
            if (!CopyFileA((cfgDir + "\\" + arg + ".js").c_str(), g.cfgPath.c_str(), FALSE)) {
                sendResponse(c, 400, "{\"error\":\"Configuration not found\"}");
                return;
            }
            configInit();
            configApplyStartupDefaults();
            configSyncAll(true);
            json = buildJsonBody("是否刷新", "是");
        }
        else
        {
            hashSet(cmd, arg);
            configSyncAll(true);
        }
    }

    sendResponse(c, 201, ansiToUtf8(json));
}

static void clientThread(SOCKET c)
{
    DWORD timeout = 5000;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    std::string req;
    char buf[4096];
    for (;;)
    {
        int n = recv(c, buf, sizeof(buf), 0);
        if (n <= 0) break;
        req.append(buf, n);
        if (req.find("\r\n\r\n") != std::string::npos)
        {
            // 头部已完整；若声明了 Content-Length 且 body 未到齐则继续读
            size_t sep = req.find("\r\n\r\n");
            std::string head = req.substr(0, sep);
            size_t cl = head.find("Content-Length:");
            if (cl == std::string::npos) cl = head.find("content-length:");
            if (cl == std::string::npos) break;
            int want = atoi(head.c_str() + cl + 15);
            if ((int)req.size() - (int)sep - 4 >= want) break;
        }
        if (req.size() > 65536) break;
    }

    if (!req.empty()) handleRequest(c, req);
    closesocket(c);
}

void httpServerThread()
{
    WSADATA wsa{};
    const int startup = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (startup != 0) {
        webAccessFailed(L"Windows 网络初始化失败，错误 " + std::to_wstring(startup) + L"。\r\n");
        return;
    }
    WebListeners listeners = startWebListeners((unsigned short)g.localPort);
    if (listeners.primary == INVALID_SOCKET) {
        webAccessFailed(listeners.notes);
        WSACleanup();
        return;
    }
    runtime_log::write("[web] LAN listener ready: %s:%u, port80=%s; background preflight started",
        listeners.lan ? "0.0.0.0" : "127.0.0.1", listeners.port,
        listeners.standard != INVALID_SOCKET ? "ready" : "unavailable");
    std::thread(webAccessMonitor, listeners).detach();
    while (g.running.load())
    {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listeners.primary, &readable);
        if (listeners.standard != INVALID_SOCKET) FD_SET(listeners.standard, &readable);
        timeval timeout{0, 500000};
        const int selected = select(0, &readable, nullptr, nullptr, &timeout);
        if (selected == SOCKET_ERROR) {
            webAccessFailed(L"网页监听发生错误 " + std::to_wstring(WSAGetLastError()) + L"。\r\n");
            break;
        }
        if (selected == 0) continue;
        for (SOCKET listener : {listeners.primary, listeners.standard}) {
            if (listener == INVALID_SOCKET || !FD_ISSET(listener, &readable)) continue;
            SOCKET c = accept(listener, nullptr, nullptr);
            if (c != INVALID_SOCKET) std::thread(clientThread, c).detach();
        }
    }

    closeWebListeners(listeners);
    WSACleanup();
}
