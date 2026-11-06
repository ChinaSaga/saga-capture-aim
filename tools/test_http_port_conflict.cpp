// Build in an isolated directory, linking src/RuntimeLog.cpp.
// Listener checks do not change firewall rules.
#include "../src/App.h"
#include "../src/WebAccess.h"
#include "../src/LanFirewall.h"
#include <iphlpapi.h>
#include <ws2tcpip.h>
#include <shellapi.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <cassert>

static bool denyWildcard = false;
static int testBind(SOCKET s, const sockaddr* address, int size)
{
    if (denyWildcard && reinterpret_cast<const sockaddr_in*>(address)->sin_addr.s_addr == htonl(INADDR_ANY)) {
        WSASetLastError(WSAEACCES);
        return SOCKET_ERROR;
    }
    return ::bind(s, address, size);
}

#define bind testBind
#include "../src/WebAccess.cpp"
#undef bind

AppState g;

int main()
{
    WSADATA wsa{};
    assert(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    int error = 0;
    unsigned short preferred = 0;
    SOCKET occupied = bindListener(0, true, error, preferred);
    assert(occupied != INVALID_SOCKET);
    auto alternate = startWebListeners(preferred);
    assert(alternate.primary != INVALID_SOCKET && alternate.lan && alternate.port != preferred);
    closeWebListeners(alternate);
    closesocket(occupied);
    puts("PASS: occupied primary port automatically selects another LAN port.");

    std::vector<SOCKET> held;
    preferred = 38888;
    for (int offset = 0; offset < 11; ++offset) {
        unsigned short actual = 0;
        SOCKET s = bindListener(preferred + offset, true, error, actual);
        if (s != INVALID_SOCKET) held.push_back(s);
    }
    auto dynamic = startWebListeners(preferred);
    assert(dynamic.primary != INVALID_SOCKET && dynamic.lan &&
        (dynamic.port < preferred || dynamic.port > preferred + 10));
    closeWebListeners(dynamic);
    for (SOCKET s : held) closesocket(s);
    puts("PASS: all preferred ports occupied falls back to an OS-assigned port.");

    denyWildcard = true;
    auto local = startWebListeners(38888);
    assert(local.primary != INVALID_SOCKET && !local.lan && local.port);
    sockaddr_in address{};
    int size = sizeof(address);
    assert(getsockname(local.primary, reinterpret_cast<sockaddr*>(&address), &size) == 0);
    assert(address.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
    closeWebListeners(local);
    puts("PASS: simulated denial of LAN binding preserves a loopback listener.");
    WSACleanup();
    return 0;
}
