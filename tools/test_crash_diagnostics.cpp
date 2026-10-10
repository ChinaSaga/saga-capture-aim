#include "../src/CrashDiagnostics.cpp"
#include <stdexcept>
#include <thread>
int wmain(int argc, wchar_t** argv) {
    if (argc != 3 || !initializeCrashDiagnostics(argv[2])) return 2;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (wcscmp(argv[1], L"--thread") == 0) {
        std::thread([] { throw std::runtime_error("diagnostic worker exception"); }).join();
    }
    if (wcscmp(argv[1], L"--cpp") == 0) {
        throw std::runtime_error("diagnostic regression exception");
    }
    ULONG_PTR info[] = {0, 0x1234};
    RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 2, info);
    return 3;
}
