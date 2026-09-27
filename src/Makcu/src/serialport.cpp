#include "../include/serialport.h"
#include "../include/makcu.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#include <setupapi.h>
#include <devguid.h>
#include <cfgmgr32.h>
#pragma comment(lib, "setupapi.lib")
#endif

namespace makcu {
namespace {
    using Clock = std::chrono::steady_clock;
    unsigned remainingMilliseconds(Clock::time_point deadline) {
        const auto left = deadline - Clock::now();
        if (left <= Clock::duration::zero()) return 0;
        const auto ms = std::chrono::ceil<std::chrono::milliseconds>(left).count();
        return static_cast<unsigned>((std::min)(ms, static_cast<decltype(ms)>(0xFFFFFFFEu)));
    }
}

    SerialPort::SerialPort()
        : m_baudRate(115200), m_timeout(100), m_isOpen(false)
        , m_deviceType(DeviceType::UNKNOWN)
#ifdef _WIN32
        , m_handle(INVALID_HANDLE_VALUE)
#else
        , m_fd(-1)
#endif
    {
#ifdef _WIN32
        std::memset(&m_dcb, 0, sizeof(m_dcb));
        std::memset(&m_timeouts, 0, sizeof(m_timeouts));
#endif
    }

    SerialPort::~SerialPort() { close(); }

    bool SerialPort::open(const std::string& port, uint32_t baudRate, bool fastMode, DeviceType deviceType) {
        std::lock_guard<std::mutex> lifecycle(m_mutex);
        // closeLocked does not lock m_mutex again, including after a failed RX.
        closeLocked();
        m_portName = port;
        m_baudRate = baudRate;
        m_deviceType = deviceType;
#ifdef _WIN32
        const std::string fullPortName = "\\\\.\\" + port;
        m_handle = CreateFileA(fullPortName.c_str(), GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (m_handle == INVALID_HANDLE_VALUE) return false;
        m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_controlEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        m_readEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_writeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!m_stopEvent || !m_controlEvent || !m_readEvent || !m_writeEvent) {
            closeLocked(); return false;
        }
        SetupComm(m_handle, 65536, 65536);
        std::this_thread::sleep_for(std::chrono::milliseconds(fastMode ? 20 : 100));
        if (!configurePort(fastMode, deviceType) ||
            !PurgeComm(m_handle, PURGE_RXCLEAR | PURGE_TXCLEAR | PURGE_RXABORT | PURGE_TXABORT) ||
            !SetCommMask(m_handle, EV_RXCHAR | EV_ERR)) {
            closeLocked(); return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(fastMode ? 10 : 50));
        {
            std::lock_guard<std::mutex> receive(m_receiveMutex);
            m_parser.reset();
            m_lastButtonMask.store(0, std::memory_order_relaxed);
        }
        m_stopListener.store(false, std::memory_order_release);
        m_isOpen.store(true, std::memory_order_release);
        try { m_listenerThread = std::thread(&SerialPort::listenerLoop, this); }
        catch (...) { closeLocked(); return false; }
        return true;
#else
        return false;
#endif
    }

    void SerialPort::close() {
        std::lock_guard<std::mutex> lifecycle(m_mutex);
        closeLocked();
    }

    void SerialPort::closeLocked() {
        m_isOpen.store(false, std::memory_order_release);
        m_stopListener.store(true, std::memory_order_release);
#ifdef _WIN32
        if (m_stopEvent) SetEvent(m_stopEvent);
        if (m_handle != INVALID_HANDLE_VALUE) CancelIoEx(m_handle, nullptr);
#endif
        if (m_listenerThread.joinable()) m_listenerThread.join();
        // The writer drains its own cancelled OVERLAPPED before releasing this
        // lock. Only then may its event and the shared port handle be closed.
        std::lock_guard<std::mutex> writer(m_writeMutex);
        failPending("Serial port closed");
        {
            std::lock_guard<std::mutex> receive(m_receiveMutex);
            m_parser.reset();
            m_lastButtonMask.store(0, std::memory_order_relaxed);
        }
#ifdef _WIN32
        if (m_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_handle); m_handle = INVALID_HANDLE_VALUE;
        }
        for (HANDLE* event : {&m_stopEvent, &m_controlEvent, &m_readEvent, &m_writeEvent}) {
            if (*event) { CloseHandle(*event); *event = nullptr; }
        }
#endif
    }

    bool SerialPort::isOpen() const { return m_isOpen.load(std::memory_order_acquire); }
    uint8_t SerialPort::buttonMask() const noexcept { return m_lastButtonMask.load(std::memory_order_relaxed); }

    bool SerialPort::setBaudRate(uint32_t baudRate) {
        std::lock_guard<std::mutex> lifecycle(m_mutex);
        std::lock_guard<std::mutex> writer(m_writeMutex);
#ifdef _WIN32
        if (!isOpen()) return false;
        DCB dcb = m_dcb;
        dcb.BaudRate = baudRate;
        if (!SetCommState(m_handle, &dcb)) return false;
        m_dcb = dcb; m_baudRate = baudRate;
        return true;
#else
        return false;
#endif
    }

    uint32_t SerialPort::getBaudRate() const {
        std::lock_guard<std::mutex> lifecycle(m_mutex); return m_baudRate;
    }
    std::string SerialPort::getPortName() const {
        std::lock_guard<std::mutex> lifecycle(m_mutex); return m_portName;
    }

    bool SerialPort::writeLocked(const uint8_t* data, size_t length) {
#ifdef _WIN32
        if (!isOpen() || (!data && length)) return false;
        const auto deadline = Clock::now() + std::chrono::milliseconds(m_timeout.load(std::memory_order_relaxed));
        size_t offset = 0;
        while (offset != length) {
            if (!isOpen()) return false;
            OVERLAPPED overlap{};
            overlap.hEvent = m_writeEvent;
            ResetEvent(overlap.hEvent);
            DWORD written = 0;
            const DWORD requested = static_cast<DWORD>((std::min)(length - offset, size_t(MAXDWORD)));
            BOOL done = WriteFile(m_handle, data + offset, requested, &written, &overlap);
            if (!done) {
                if (GetLastError() != ERROR_IO_PENDING) { markDisconnected("Serial write failed"); return false; }
                HANDLE events[] = {m_stopEvent, m_writeEvent};
                const DWORD wait = WaitForMultipleObjects(2, events, FALSE, remainingMilliseconds(deadline));
                if (wait != WAIT_OBJECT_0 + 1) {
                    CancelIoEx(m_handle, &overlap);
                    GetOverlappedResult(m_handle, &overlap, &written, TRUE);
                    markDisconnected(wait == WAIT_TIMEOUT ? "Serial write timed out" : "Serial write cancelled");
                    return false;
                }
                done = GetOverlappedResult(m_handle, &overlap, &written, FALSE);
                if (!done) {
                    // Even an unexpected wait/result failure must retire the IO
                    // before the stack OVERLAPPED and payload go out of scope.
                    CancelIoEx(m_handle, &overlap);
                    GetOverlappedResult(m_handle, &overlap, &written, TRUE);
                    markDisconnected("Serial write completion failed"); return false;
                }
            }
            if (!written || written > requested) { markDisconnected("Serial write made no progress"); return false; }
            offset += written;
            if (offset != length && !remainingMilliseconds(deadline)) {
                markDisconnected("Serial partial write timed out"); return false;
            }
        }
        return isOpen();
#else
        return false;
#endif
    }

    bool SerialPort::writeTextLocked(const char* data, size_t length) {
        if (!data && length) return false;
        // All motion/button commands fit this fixed buffer: one write, no heap
        // allocation and no forced TX drain on the movement hot path.
        char command[256];
        if (length <= sizeof(command) - 2) {
            if (length) std::memcpy(command, data, length);
            command[length] = '\r'; command[length + 1] = '\n';
            return writeLocked(reinterpret_cast<const uint8_t*>(command), length + 2);
        }
        if (length > (std::numeric_limits<size_t>::max)() - 2) return false;
        std::string commandLong(data, length);
        commandLong += "\r\n";
        return writeLocked(reinterpret_cast<const uint8_t*>(commandLong.data()), commandLong.size());
    }

    bool SerialPort::sendCommand(const std::string& command) { return sendCommand(command.data(), command.size()); }
    bool SerialPort::sendCommand(const char* command, size_t length) {
        std::lock_guard<std::mutex> writer(m_writeMutex);
        return writeTextLocked(command, length);
    }
    bool SerialPort::writeRaw(const uint8_t* data, size_t length) {
        std::lock_guard<std::mutex> writer(m_writeMutex); return writeLocked(data, length);
    }
    bool SerialPort::write(const std::vector<uint8_t>& data) {
        return sendCommand(reinterpret_cast<const char*>(data.data()), data.size());
    }
    bool SerialPort::write(const std::string& data) { return sendCommand(data); }

    std::future<std::string> SerialPort::sendTrackedCommand(const std::string& command, bool expectResponse,
        std::chrono::milliseconds timeout) {
        if (!expectResponse) {
            std::promise<std::string> promise;
            auto future = promise.get_future();
            if (sendCommand(command)) promise.set_value({});
            else promise.set_exception(std::make_exception_ptr(std::runtime_error("Serial command write failed")));
            return future;
        }
        auto pending = std::make_unique<PendingCommand>(timeout);
        auto future = pending->promise.get_future();
        std::lock_guard<std::mutex> writer(m_writeMutex);
        {
            std::lock_guard<std::mutex> receive(m_receiveMutex);
            if (!isOpen() || m_pendingCommand || !m_parser.beginQuery(command)) {
                pending->promise.set_exception(std::make_exception_ptr(std::runtime_error("Serial query unavailable")));
                return future;
            }
            m_pendingCommand = std::move(pending);
        }
#ifdef _WIN32
        SetEvent(m_controlEvent);
#endif
        if (!writeTextLocked(command.data(), command.size())) {
            failPending("Serial query write failed"); return future;
        }
        {
            std::lock_guard<std::mutex> receive(m_receiveMutex);
            if (m_pendingCommand) { m_pendingCommand->sent = true; finishPendingLocked(); }
        }
        return future;
    }

    void SerialPort::finishPendingLocked() {
        if (m_pendingCommand && m_pendingCommand->sent && m_pendingCommand->received) {
            m_pendingCommand->promise.set_value(std::move(m_pendingCommand->result));
            m_pendingCommand.reset();
        }
    }

    bool SerialPort::beginRawButtonsAfterPrompt() {
        std::lock_guard<std::mutex> writer(m_writeMutex);
        std::lock_guard<std::mutex> receive(m_receiveMutex);
        return isOpen() && !m_pendingCommand && m_parser.enterRawButtons();
    }

    void SerialPort::failPending(const char* reason) {
        std::lock_guard<std::mutex> receive(m_receiveMutex);
        m_parser.cancelQuery();
        if (m_pendingCommand) {
            m_pendingCommand->promise.set_exception(std::make_exception_ptr(std::runtime_error(reason)));
            m_pendingCommand.reset();
        }
    }

    void SerialPort::markDisconnected(const char* reason) {
        m_isOpen.store(false, std::memory_order_release);
        m_stopListener.store(true, std::memory_order_release);
#ifdef _WIN32
        // This runs only on the receiver or with m_writeMutex held, so the
        // handle/events cannot be closed until this function returns.
        if (m_stopEvent) SetEvent(m_stopEvent);
        if (m_handle != INVALID_HANDLE_VALUE) CancelIoEx(m_handle, nullptr);
#endif
        failPending(reason);
        std::lock_guard<std::mutex> receive(m_receiveMutex);
        m_lastButtonMask.store(0, std::memory_order_relaxed);
    }

    unsigned SerialPort::nextQueryTimeout() {
        std::lock_guard<std::mutex> receive(m_receiveMutex);
        return m_pendingCommand ? remainingMilliseconds(m_pendingCommand->deadline) : 0xFFFFFFFFu;
    }

    void SerialPort::cleanupTimedOutCommands() {
        bool expired;
        {
            std::lock_guard<std::mutex> receive(m_receiveMutex);
            expired = m_pendingCommand && Clock::now() >= m_pendingCommand->deadline;
        }
        // V4 provides no response ID. Reopening after a timeout prevents a
        // delayed response from being mistaken for the next same-kind query.
        if (expired) markDisconnected("Serial query timed out");
    }

    void SerialPort::listenerLoop() {
#ifdef _WIN32
        uint8_t buffer[BUFFER_SIZE];
        // A single RX operation owns m_readEvent and this stack OVERLAPPED.
        // WaitCommEvent sleeps indefinitely when idle; control wakes it when
        // a query introduces a deadline. There is no timed polling loop.
        auto awaitReceive = [&](OVERLAPPED& overlap, DWORD& transferred) -> bool {
            HANDLE events[] = {m_stopEvent, m_readEvent, m_controlEvent};
            for (;;) {
                const DWORD wait = WaitForMultipleObjects(3, events, FALSE, nextQueryTimeout());
                if (wait == WAIT_OBJECT_0 + 1) {
                    if (GetOverlappedResult(m_handle, &overlap, &transferred, FALSE)) return true;
                    break;
                }
                if (wait == WAIT_OBJECT_0 + 2 || wait == WAIT_TIMEOUT) {
                    cleanupTimedOutCommands();
                    if (!m_stopListener.load(std::memory_order_acquire)) continue;
                }
                break;
            }
            CancelIoEx(m_handle, &overlap);
            GetOverlappedResult(m_handle, &overlap, &transferred, TRUE);
            return false;
        };
        while (!m_stopListener.load(std::memory_order_acquire)) {
            cleanupTimedOutCommands();
            if (m_stopListener.load(std::memory_order_acquire)) break;
            OVERLAPPED overlap{};
            overlap.hEvent = m_readEvent;
            ResetEvent(m_readEvent);
            DWORD eventMask = 0, transferred = 0;
            if (!WaitCommEvent(m_handle, &eventMask, &overlap)) {
                if (GetLastError() != ERROR_IO_PENDING || !awaitReceive(overlap, transferred)) {
                    if (isOpen()) markDisconnected("Serial receive event failed");
                    break;
                }
            }
            if (m_stopListener.load(std::memory_order_acquire)) break;
            for (;;) {
                COMSTAT status{};
                DWORD errors = 0;
                if (!ClearCommError(m_handle, &errors, &status) || errors) {
                    markDisconnected("Serial receive line error"); return;
                }
                if (!status.cbInQue) break;
                overlap = {}; overlap.hEvent = m_readEvent;
                ResetEvent(m_readEvent);
                DWORD received = 0;
                const DWORD requested = (std::min)(status.cbInQue, static_cast<DWORD>(BUFFER_SIZE));
                if (!ReadFile(m_handle, buffer, requested, &received, &overlap)) {
                    if (GetLastError() != ERROR_IO_PENDING || !awaitReceive(overlap, received)) {
                        if (isOpen()) markDisconnected("Serial receive failed");
                        return;
                    }
                }
                if (!received || received > requested) {
                    markDisconnected("Serial receive made no progress"); return;
                }
                uint8_t buttonEvents[BUFFER_SIZE];
                size_t buttonCount = 0;
                bool protocolError = false;
                {
                    // Finish the entire chunk before waking a query waiter:
                    // trailing noise must invalidate the transition barrier.
                    std::lock_guard<std::mutex> receive(m_receiveMutex);
                    for (DWORD i = 0; i < received; ++i) {
                        auto event = m_parser.consume(buffer[i]);
                        if (event.kind == protocol::EventKind::ProtocolError) { protocolError = true; break; }
                        if (event.kind == protocol::EventKind::Button) buttonEvents[buttonCount++] = event.buttonMask;
                        if (event.kind == protocol::EventKind::QueryComplete && m_pendingCommand) {
                            m_pendingCommand->received = true;
                            m_pendingCommand->result = std::move(event.text);
                        }
                    }
                    if (!protocolError) finishPendingLocked();
                }
                if (protocolError) { markDisconnected("Serial button stream lost synchronization"); return; }
                for (size_t i = 0; i < buttonCount; ++i) handleButtonData(buttonEvents[i]);
                if (m_stopListener.load(std::memory_order_acquire)) return;
                cleanupTimedOutCommands();
                if (m_stopListener.load(std::memory_order_acquire)) return;
            }
        }
#endif
    }

    void SerialPort::setButtonCallback(ButtonCallback callback) {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        m_buttonCallback = std::move(callback);
        m_hasButtonCallback.store(static_cast<bool>(m_buttonCallback), std::memory_order_release);
    }

    void SerialPort::handleButtonData(uint8_t data) {
        uint8_t changed;
        {
            std::lock_guard<std::mutex> receive(m_receiveMutex);
            if (!isOpen()) return;
            changed = m_lastButtonMask.load(std::memory_order_relaxed) ^ data;
            if (!changed) return;
            m_lastButtonMask.store(data, std::memory_order_relaxed);
        }
        if (!m_hasButtonCallback.load(std::memory_order_acquire)) return;
        ButtonCallback callback;
        { std::lock_guard<std::mutex> lock(m_callbackMutex); callback = m_buttonCallback; }
        if (callback) {
            for (uint8_t bit = 0; bit < 5; ++bit) {
                if (changed & (1u << bit)) {
                    try { callback(bit, (data & (1u << bit)) != 0); }
                    catch (...) { /* A consumer callback must not terminate RX. */ }
                }
            }
        }
    }

    std::vector<uint8_t> SerialPort::read(size_t) { return {}; }
    std::string SerialPort::readString(size_t) { return {}; }

    size_t SerialPort::available() const {
        std::lock_guard<std::mutex> lifecycle(m_mutex);
#ifdef _WIN32
        if (!isOpen()) return 0;
        COMSTAT status{}; DWORD errors = 0;
        if (ClearCommError(m_handle, &errors, &status)) return status.cbInQue;
#endif
        return 0;
    }

    bool SerialPort::flush() {
        std::lock_guard<std::mutex> lifecycle(m_mutex);
        std::lock_guard<std::mutex> writer(m_writeMutex);
#ifdef _WIN32
        if (!isOpen()) return false;
        if (FlushFileBuffers(m_handle)) return true;
        markDisconnected("Serial flush failed");
#endif
        return false;
    }

    bool SerialPort::updateTimeouts() {
#ifdef _WIN32
        // RX is issued only for queued bytes after EV_RXCHAR. Complete the
        // available bytes immediately; event waiting controls query deadlines.
        m_timeouts.ReadIntervalTimeout = MAXDWORD;
        m_timeouts.ReadTotalTimeoutMultiplier = 0;
        m_timeouts.ReadTotalTimeoutConstant = 0;
        m_timeouts.WriteTotalTimeoutMultiplier = 0;
        m_timeouts.WriteTotalTimeoutConstant = m_timeout.load(std::memory_order_relaxed);
        return SetCommTimeouts(m_handle, &m_timeouts) != FALSE;
#else
        return false;
#endif
    }

    void SerialPort::setTimeout(uint32_t timeoutMs) {
        std::lock_guard<std::mutex> lifecycle(m_mutex);
        std::lock_guard<std::mutex> writer(m_writeMutex);
        m_timeout.store((std::max)(uint32_t(1), timeoutMs), std::memory_order_relaxed);
        if (isOpen() && !updateTimeouts()) markDisconnected("Serial timeout configuration failed");
    }
    uint32_t SerialPort::getTimeout() const { return m_timeout.load(std::memory_order_relaxed); }

    bool SerialPort::configurePort(bool fastMode, DeviceType deviceType) {
#ifdef _WIN32
        // 方案1：根据设备类型选择性重置 DTR/RTS
        // MAKCU 设备需要 DTR/RTS 重置来解决重连问题
        // Ferrum 设备不需要，跳过可以减少 100-200ms 延迟
        if (deviceType == DeviceType::MAKCU || deviceType == DeviceType::UNKNOWN) {
            // MAKCU 或未知设备：执行 DTR/RTS 重置
            EscapeCommFunction(m_handle, CLRDTR);
            EscapeCommFunction(m_handle, CLRRTS);
            
            // fastMode 用于波特率检测时的快速尝试，延迟较短
            if (fastMode) {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
            } else {
                // 正常连接需要更长的重置时间
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        } else {
            // Ferrum 设备：跳过 DTR/RTS 重置，只需短暂延迟
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        m_dcb.DCBlength = sizeof(DCB);

        if (!GetCommState(m_handle, &m_dcb)) {
            return false;
        }

        m_dcb.BaudRate = m_baudRate;
        m_dcb.ByteSize = 8;
        m_dcb.Parity = NOPARITY;
        m_dcb.StopBits = ONESTOPBIT;
        m_dcb.fBinary = TRUE;
        m_dcb.fParity = FALSE;
        m_dcb.fOutxCtsFlow = FALSE;
        m_dcb.fOutxDsrFlow = FALSE;
        // Enable DTR signal for device initialization
        m_dcb.fDtrControl = DTR_CONTROL_ENABLE;
        m_dcb.fDsrSensitivity = FALSE;
        m_dcb.fTXContinueOnXoff = FALSE;
        m_dcb.fOutX = FALSE;
        m_dcb.fInX = FALSE;
        m_dcb.fErrorChar = FALSE;
        m_dcb.fNull = FALSE;
        // Enable RTS signal
        m_dcb.fRtsControl = RTS_CONTROL_ENABLE;
        m_dcb.fAbortOnError = FALSE;

        if (!SetCommState(m_handle, &m_dcb)) {
            return false;
        }

        // 设置 DTR 和 RTS 信号，激活设备
        EscapeCommFunction(m_handle, SETDTR);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        EscapeCommFunction(m_handle, SETRTS);
        
        // fastMode 用于快速检测，延迟较短
        if (fastMode) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        } else {
            // 正常连接需要更长的初始化时间，确保设备完全就绪
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        return updateTimeouts();
#else
        return false;
#endif
    }

    std::vector<std::string> SerialPort::getAvailablePorts() {
        std::vector<std::string> ports;

#ifdef _WIN32
        HKEY hKey;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM",
            0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            char valueName[256];
            char data[256];
            DWORD valueNameSize, dataSize, dataType;
            DWORD index = 0;

            while (true) {
                valueNameSize = sizeof(valueName);
                dataSize = sizeof(data);

                LONG result = RegEnumValueA(hKey, index++, valueName, &valueNameSize,
                    nullptr, &dataType,
                    reinterpret_cast<BYTE*>(data), &dataSize);

                if (result == ERROR_NO_MORE_ITEMS) {
                    break;
                }

                if (result == ERROR_SUCCESS && dataType == REG_SZ) {
                    ports.emplace_back(data);
                }
            }

            RegCloseKey(hKey);
        }
#endif

        std::sort(ports.begin(), ports.end());
        return ports;
    }

    std::vector<std::string> SerialPort::findMakcuPorts() {
        std::vector<std::string> makcuPorts;

#ifdef _WIN32
        auto allPorts = getAvailablePorts();
        HDEVINFO deviceInfoSet = SetupDiGetClassDevs(&GUID_DEVCLASS_PORTS,
            nullptr, nullptr, DIGCF_PRESENT);
        if (deviceInfoSet == INVALID_HANDLE_VALUE) {
            return makcuPorts;
        }

        SP_DEVINFO_DATA deviceInfoData;
        deviceInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

        for (DWORD i = 0; SetupDiEnumDeviceInfo(deviceInfoSet, i, &deviceInfoData); i++) {
            char description[256] = { 0 };
            char portName[256] = { 0 };

            if (SetupDiGetDeviceRegistryPropertyA(deviceInfoSet, &deviceInfoData,
                SPDRP_DEVICEDESC, nullptr,
                reinterpret_cast<BYTE*>(description),
                sizeof(description), nullptr)) {
                std::string desc(description);

                // 支持多种串口芯片：CH340, CH343, CH347, CP2102
                if (desc.find("USB-Enhanced-SERIAL CH343") != std::string::npos ||
                    desc.find("USB-SERIAL CH340") != std::string::npos ||
                    desc.find("CH347") != std::string::npos ||
                    desc.find("CP210") != std::string::npos ||
                    desc.find("Silicon Labs") != std::string::npos) {

                    HKEY hDeviceKey = SetupDiOpenDevRegKey(deviceInfoSet, &deviceInfoData,
                        DICS_FLAG_GLOBAL, 0,
                        DIREG_DEV, KEY_READ);
                    if (hDeviceKey != INVALID_HANDLE_VALUE) {
                        DWORD portNameSize = sizeof(portName);

                        if (RegQueryValueExA(hDeviceKey, "PortName", nullptr, nullptr,
                            reinterpret_cast<BYTE*>(portName),
                            &portNameSize) == ERROR_SUCCESS) {
                            std::string port(portName);
                            if (std::find(allPorts.begin(), allPorts.end(), port) != allPorts.end()) {
                                makcuPorts.emplace_back(port);
                            }
                        }
                        RegCloseKey(hDeviceKey);
                    }
                }
            }
        }

        SetupDiDestroyDeviceInfoList(deviceInfoSet);
#endif

        std::sort(makcuPorts.begin(), makcuPorts.end());
        makcuPorts.erase(std::unique(makcuPorts.begin(), makcuPorts.end()), makcuPorts.end());
        return makcuPorts;
    }

} // namespace makcu
