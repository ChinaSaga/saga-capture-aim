#pragma once
#include <windows.h>
#include <netfw.h>
#include <wrl/client.h>
#include <string>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

// App-specific inbound access, restricted to the local subnet. Never disable
// the firewall or change global policy. Results appear in the network report.
inline HRESULT configureLanFirewall(const std::wstring& ports = L"80,8888",
                                    std::wstring* diagnostics = nullptr)
{
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized)) return initialized;
    struct ComCleanup { ~ComCleanup() { CoUninitialize(); } } cleanup;
    struct Bstr {
        BSTR value;
        Bstr() : value(nullptr) {}
        explicit Bstr(const wchar_t* text) : value(SysAllocString(text)) {}
        ~Bstr() { SysFreeString(value); }
        operator BSTR() const { return value; }
    };
    using Microsoft::WRL::ComPtr;
    ComPtr<INetFwPolicy2> policy;
    HRESULT hr = CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&policy));
    if (FAILED(hr)) return hr;
    if (diagnostics) {
        long profiles = 0;
        if (SUCCEEDED(policy->get_CurrentProfileTypes(&profiles))) {
            *diagnostics += L"启动预检时的网络类型：";
            for (auto profile : {NET_FW_PROFILE2_DOMAIN, NET_FW_PROFILE2_PRIVATE, NET_FW_PROFILE2_PUBLIC}) {
                if (!(profiles & profile)) continue;
                const wchar_t* label = profile == NET_FW_PROFILE2_DOMAIN ? L"域网络" :
                    profile == NET_FW_PROFILE2_PRIVATE ? L"专用网络" : L"公用网络";
                *diagnostics += std::wstring(label) + L" ";
                VARIANT_BOOL blocked = VARIANT_FALSE;
                if (SUCCEEDED(policy->get_BlockAllInboundTraffic(profile, &blocked)) && blocked)
                    *diagnostics += L"[系统设置为阻止所有入站，允许规则可能不生效] ";
            }
            *diagnostics += L"\r\n";
        } else *diagnostics += L"当前网络类型：无法读取\r\n";
        NET_FW_MODIFY_STATE state{};
        if (SUCCEEDED(policy->get_LocalPolicyModifyState(&state)) && state != NET_FW_MODIFY_STATE_OK)
            *diagnostics += state == NET_FW_MODIFY_STATE_GP_OVERRIDE
                ? L"系统策略：组策略覆盖本地防火墙规则，需要系统管理员处理。\r\n"
                : L"系统策略：当前防火墙禁止应用例外，允许规则可能不生效。\r\n";
    }
    ComPtr<INetFwRules> rules;
    if (FAILED(hr = policy->get_Rules(&rules))) return hr;
    wchar_t path[32768]{};
    if (!GetModuleFileNameW(nullptr, path, _countof(path))) return HRESULT_FROM_WIN32(GetLastError());
    if (diagnostics) {
        ComPtr<IUnknown> collection;
        ComPtr<IEnumVARIANT> entries;
        if (SUCCEEDED(rules->get__NewEnum(&collection)) && SUCCEEDED(collection.As(&entries))) {
            VARIANT entry;
            VariantInit(&entry);
            int blockers = 0;
            while (entries->Next(1, &entry, nullptr) == S_OK) {
                ComPtr<INetFwRule> candidate;
                if (entry.vt == VT_DISPATCH && entry.pdispVal &&
                    SUCCEEDED(entry.pdispVal->QueryInterface(IID_PPV_ARGS(&candidate)))) {
                    VARIANT_BOOL enabled = VARIANT_FALSE;
                    NET_FW_RULE_DIRECTION direction{};
                    NET_FW_ACTION action{};
                    Bstr application;
                    if (SUCCEEDED(candidate->get_Enabled(&enabled)) && enabled &&
                        SUCCEEDED(candidate->get_Direction(&direction)) && direction == NET_FW_RULE_DIR_IN &&
                        SUCCEEDED(candidate->get_Action(&action)) && action == NET_FW_ACTION_BLOCK &&
                        SUCCEEDED(candidate->get_ApplicationName(&application.value)) && application.value &&
                        _wcsicmp(application.value, path) == 0) ++blockers;
                }
                VariantClear(&entry);
            }
            if (blockers) *diagnostics += L"发现当前程序的入站阻止规则 " + std::to_wstring(blockers) +
                L" 条：请检查其网络类型和端口范围；匹配的阻止规则优先于允许规则。\r\n";
        }
    }
    // Distinguish installations, so one running copy cannot change another's rule.
    const std::wstring name = std::wstring(L"SagaApp LAN Web - ") + path;
    ComPtr<INetFwRule> rule;
    const bool existing = SUCCEEDED(rules->Item(Bstr(name.c_str()), &rule));
    if (!existing && FAILED(hr = CoCreateInstance(__uuidof(NetFwRule), nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&rule)))) return hr;
    if (FAILED(hr = rule->put_Name(Bstr(name.c_str()))) ||
        FAILED(hr = rule->put_ApplicationName(Bstr(path))) ||
        FAILED(hr = rule->put_Protocol(NET_FW_IP_PROTOCOL_TCP)) ||
        FAILED(hr = rule->put_LocalPorts(Bstr(ports.c_str()))) ||
        FAILED(hr = rule->put_RemoteAddresses(Bstr(L"LocalSubnet"))) ||
        FAILED(hr = rule->put_Direction(NET_FW_RULE_DIR_IN)) ||
        FAILED(hr = rule->put_Profiles(NET_FW_PROFILE2_ALL)) ||
        FAILED(hr = rule->put_Action(NET_FW_ACTION_ALLOW)) ||
        FAILED(hr = rule->put_Enabled(VARIANT_TRUE))) return hr;
    return existing ? S_OK : rules->Add(rule.Get());
}
