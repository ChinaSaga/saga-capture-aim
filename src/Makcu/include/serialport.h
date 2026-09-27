#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <future>
#include <thread>
#include <chrono>
#include <functional>
#include <memory>
#include "serialprotocol.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace makcu {

    enum class DeviceType;

    struct PendingCommand {
        std::promise<std::string> promise;
        std::chrono::steady_clock::time_point deadline;
        bool sent = false, received = false;
        std::string result;
        explicit PendingCommand(std::chrono::milliseconds timeout)
            : deadline(std::chrono::steady_clock::now() + timeout) {}
    };

    class SerialPort {
    public:
        SerialPort();
        ~SerialPort();

        bool open(const std::string& port, uint32_t baudRate, bool fastMode = false, DeviceType deviceType = static_cast<DeviceType>(0));
        void close();
        bool isOpen() const;

        bool setBaudRate(uint32_t baudRate);
        uint32_t getBaudRate() const;
        std::string getPortName() const;

        // V4 text has no wire IDs. Only one query may be outstanding; its
        // command-specific result and complete prompt must both be received.
        std::future<std::string> sendTrackedCommand(const std::string& command,
            bool expectResponse = false,
            std::chrono::milliseconds timeout = std::chrono::milliseconds(100));

        bool sendCommand(const std::string& command);
        bool sendCommand(const char* command, size_t length);
        bool writeRaw(const uint8_t* data, size_t length);

        // After echo(0), buttons(0), and a successful identity query, enter
        // raw mode atomically at that exact completed prompt boundary.
        bool beginRawButtonsAfterPrompt();
        uint8_t buttonMask() const noexcept;

        bool write(const std::vector<uint8_t>& data);
        bool write(const std::string& data);
        // The receiver owns all RX bytes. Legacy direct reads return empty;
        // use query futures or buttonMask() instead of racing the listener.
        std::vector<uint8_t> read(size_t maxBytes = 1024);
        std::string readString(size_t maxBytes = 1024);

        size_t available() const;
        bool flush();
        void setTimeout(uint32_t timeoutMs);
        uint32_t getTimeout() const;
        static std::vector<std::string> getAvailablePorts();
        static std::vector<std::string> findMakcuPorts();

        // Legacy callback receives bit indices 0..4 on the RX thread. It must
        // not call lifecycle/configuration methods or block on those methods;
        // the application reads buttonMask() directly and uses no callback.
        using ButtonCallback = std::function<void(uint8_t, bool)>;
        void setButtonCallback(ButtonCallback callback);

    private:
        std::string m_portName;
        uint32_t m_baudRate;
        std::atomic<uint32_t> m_timeout;
        std::atomic<bool> m_isOpen;
        mutable std::mutex m_mutex; // lifecycle/configuration, never held by RX
        std::mutex m_writeMutex;
        DeviceType m_deviceType;
#ifdef _WIN32
        HANDLE m_handle;
        HANDLE m_stopEvent = nullptr, m_controlEvent = nullptr;
        HANDLE m_readEvent = nullptr, m_writeEvent = nullptr;
        DCB m_dcb;
        COMMTIMEOUTS m_timeouts;
#else
        int m_fd;
#endif
        std::mutex m_receiveMutex;
        protocol::Parser m_parser;
        std::unique_ptr<PendingCommand> m_pendingCommand;
        std::thread m_listenerThread;
        std::atomic<bool> m_stopListener{false};
        std::mutex m_callbackMutex;
        ButtonCallback m_buttonCallback;
        std::atomic<bool> m_hasButtonCallback{false};
        std::atomic<uint8_t> m_lastButtonMask{0};
        static constexpr size_t BUFFER_SIZE = 4096;

        bool configurePort(bool fastMode = false, DeviceType deviceType = static_cast<DeviceType>(0));
        bool updateTimeouts();
        void closeLocked();
        bool writeLocked(const uint8_t* data, size_t length);
        bool writeTextLocked(const char* data, size_t length);
        void listenerLoop();
        void handleButtonData(uint8_t data);
        void cleanupTimedOutCommands();
        void failPending(const char* reason);
        void markDisconnected(const char* reason);
        void finishPendingLocked();
        unsigned nextQueryTimeout();
        SerialPort(const SerialPort&) = delete;
        SerialPort& operator=(const SerialPort&) = delete;
    };

} // namespace makcu
