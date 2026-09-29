#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include "../src/AppExit.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <thread>
#pragma comment(lib, "ws2_32.lib")

int main(int argc, char** argv)
{
    WSADATA wsa{};
    assert(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    if (argc == 2) {
        SOCKET socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<u_short>(atoi(argv[1])));
        if (bind(socketHandle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
            return 2;
        // A joinable, indefinitely blocked worker would abort normal teardown.
        std::thread blocked([] { Sleep(INFINITE); });
        exitApplicationNow();
    }
    SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    int size = sizeof(address);
    assert(getsockname(probe, reinterpret_cast<sockaddr*>(&address), &size) == 0);
    closesocket(probe);
    char executable[MAX_PATH]{};
    GetModuleFileNameA(nullptr, executable, MAX_PATH);
    char command[MAX_PATH + 32]{};
    sprintf_s(command, "\"%s\" %u", executable, ntohs(address.sin_port));
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    assert(CreateProcessA(executable, command, nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process));
    assert(WaitForSingleObject(process.hProcess, 5000) == WAIT_OBJECT_0);
    DWORD code = 1;
    assert(GetExitCodeProcess(process.hProcess, &code) && code == 0);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    closesocket(probe);
    WSACleanup();
    puts("PASS: blocked worker terminated; process exited; TCP port released.");
}
