#pragma once
#include <windows.h>
#include <sddl.h>
#include <filesystem>
#include <vector>
#include <stdexcept>
#pragma comment(lib, "advapi32.lib")

// Only use on a freshly created fixture directory owned by this test process.
class FixtureReadDeny {
    std::filesystem::path path;
    std::vector<char> saved;
    DWORD restoreFlags = DACL_SECURITY_INFORMATION;
public:
    explicit FixtureReadDeny(const std::filesystem::path& target) : path(target) {
        DWORD size = 0;
        GetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, nullptr, 0, &size);
        saved.resize(size);
        if (!size || !GetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, saved.data(), size, &size))
            throw std::runtime_error("Cannot save fixture ACL");
        SECURITY_DESCRIPTOR_CONTROL control{}; DWORD revision{};
        GetSecurityDescriptorControl(saved.data(), &control, &revision);
        restoreFlags |= control & SE_DACL_PROTECTED ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION;
        PSECURITY_DESCRIPTOR deny = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(D;;FR;;;WD)(A;;FA;;;WD)", SDDL_REVISION_1, &deny, nullptr))
            throw std::runtime_error("Cannot build fixture ACL");
        const bool applied = SetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, deny);
        LocalFree(deny);
        if (!applied) throw std::runtime_error("Cannot apply fixture ACL");
    }
    ~FixtureReadDeny() { if (!saved.empty()) SetFileSecurityW(path.c_str(), restoreFlags, saved.data()); }
    void restore() {
        if (!SetFileSecurityW(path.c_str(), restoreFlags, saved.data())) throw std::runtime_error("Cannot restore fixture ACL");
        saved.clear();
    }
};
