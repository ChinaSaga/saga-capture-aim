#include "Inference.h"
#include "include/makcu.h"
#include "include/serialport.h"
#include "include/commands.h"
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string_view>
#include <thread>
#include "cat/include/catdevice.h"
#include "cat/src/input-event-codes.h"

namespace makcu {

    constexpr uint32_t INITIAL_BAUD_RATE = 115200;
    constexpr uint32_t HIGH_SPEED_BAUD_RATE = 4000000;
    constexpr uint32_t FERRUM_HIGH_SPEED_BAUD_RATE = 3000000;
    constexpr std::array<uint8_t, 9> BAUD_CHANGE_COMMAND = {
        0xDE, 0xAD, 0x05, 0x00, 0xA5, 0x00, 0x09, 0x3D, 0x00
    };
    constexpr std::array<std::string_view, 5> PRESS_COMMANDS = {
        "km.left(1)", "km.right(1)", "km.middle(1)", "km.side1(1)", "km.side2(1)"
    };
    constexpr std::array<std::string_view, 5> RELEASE_COMMANDS = {
        "km.left(0)", "km.right(0)", "km.middle(0)", "km.side1(0)", "km.side2(0)"
    };

    namespace {
        bool MapMouseButton(int key, MouseButton& out) {
            switch (key) {
            case 1: out = MouseButton::LEFT; break;
            case 2: out = MouseButton::RIGHT; break;
            case 3: out = MouseButton::MIDDLE; break;
            case 5: out = MouseButton::SIDE1; break;
            case 6: out = MouseButton::SIDE2; break;
            default: return false;
            }
            return true;
        }
    }

    class Device::Impl {
    public:
        std::unique_ptr<SerialPort> serialPort = std::make_unique<SerialPort>();
        std::mutex connectMutex;
        std::mutex queryMutex;
        DeviceType deviceType = DeviceType::UNKNOWN;
        std::string deviceVersion;

        // One atomic publishes initialization and protects the connection epoch.
        // Low bit: online; remaining low 32 bits: active calls, in units of two.
        // The epoch prevents a paused old call entering a newly opened session.
        static constexpr uint64_t ONLINE = 1;
        static constexpr uint64_t READERS = 0xfffffffeull;
        static constexpr uint64_t EPOCH = 0xffffffff00000000ull;
        static constexpr uint64_t NEXT_EPOCH = 0x100000000ull;
        std::atomic<uint64_t> commandState{0};

        bool beginCommand() {
            uint64_t state = commandState.load(std::memory_order_acquire);
            const uint64_t epoch = state & EPOCH;
            while ((state & ONLINE) && (state & EPOCH) == epoch && (state & READERS) != READERS) {
                if (commandState.compare_exchange_weak(state, state + 2,
                    std::memory_order_acquire, std::memory_order_relaxed)) {
                    if (serialPort->isOpen()) return true;
                    markDisconnected();
                    endCommand();
                    return false;
                }
            }
            return false;
        }

        void endCommand() {
            const uint64_t previous = commandState.fetch_sub(2, std::memory_order_release);
            if (!(previous & ONLINE) && (previous & READERS) == 2)
                commandState.notify_all();
        }

        void markDisconnected() {
            commandState.fetch_and(~ONLINE, std::memory_order_acq_rel);
        }

        void stopCommands() {
            markDisconnected();
            uint64_t state = commandState.load(std::memory_order_acquire);
            while (state & READERS) {
                commandState.wait(state, std::memory_order_acquire);
                state = commandState.load(std::memory_order_acquire);
            }
        }

        void startCommands() {
            const uint64_t epoch = (commandState.load(std::memory_order_relaxed) + NEXT_EPOCH) & EPOCH;
            commandState.store(epoch | ONLINE, std::memory_order_release);
        }

        bool isConnected() const {
            return (commandState.load(std::memory_order_acquire) & ONLINE) && serialPort->isOpen();
        }

        struct CommandScope {
            Impl* owner;
            explicit CommandScope(Impl& impl) : owner(impl.beginCommand() ? &impl : nullptr) {}
            ~CommandScope() { if (owner) owner->endCommand(); }
            explicit operator bool() const { return owner != nullptr; }
            CommandScope(const CommandScope&) = delete;
            CommandScope& operator=(const CommandScope&) = delete;
        };

        // Cold path only: stop streaming/echo, then require an actual identity
        // result followed by its complete prompt. A successful write is not ACK.
        static bool queryIdentity(SerialPort& port, DeviceType& type, std::string& version) {
            constexpr std::string_view echoOff = "km.echo(0)";
            constexpr std::string_view buttonsOff = "km.buttons(0)";
            if (!port.sendCommand(echoOff.data(), echoOff.size()) ||
                !port.sendCommand(buttonsOff.data(), buttonsOff.size())) return false;
            try {
                auto result = port.sendTrackedCommand("km.version()", true, std::chrono::milliseconds(150));
                if (result.wait_for(std::chrono::milliseconds(200)) != std::future_status::ready) return false;
                std::string reply = result.get();
                const auto identity = detail::identifyDevice(reply);
                if (identity == detail::DeviceIdentity::Unknown) return false;
                const DeviceType found = identity == detail::DeviceIdentity::Ferrum ? DeviceType::FERRUM : DeviceType::MAKCU;
                if (type != DeviceType::UNKNOWN && type != found) return false;
                type = found;
                version = std::move(reply);
                return true;
            } catch (...) {
                return false;
            }
        }

        uint32_t detectBaudRate(const std::string& portName) {
            for (const uint32_t baud : {HIGH_SPEED_BAUD_RATE, FERRUM_HIGH_SPEED_BAUD_RATE, INITIAL_BAUD_RATE}) {
                SerialPort probe;
                if (!probe.open(portName, baud, true, DeviceType::UNKNOWN)) continue;
                std::this_thread::sleep_for(std::chrono::milliseconds(80));
                DeviceType detected = DeviceType::UNKNOWN;
                std::string version;
                if (queryIdentity(probe, detected, version)) {
                    probe.close();
                    deviceType = detected;
                    deviceVersion = std::move(version);
                    return baud;
                }
                probe.close();
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            return 0;
        }

        bool switchToHighSpeedMode() {
            if (!serialPort->isOpen() ||
                !serialPort->writeRaw(BAUD_CHANGE_COMMAND.data(), BAUD_CHANGE_COMMAND.size()) ||
                !serialPort->flush()) return false;
            const std::string portName = serialPort->getPortName();
            serialPort->close();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (!serialPort->open(portName, HIGH_SPEED_BAUD_RATE, false, deviceType)) return false;
            return queryIdentity(*serialPort, deviceType, deviceVersion);
        }

        bool initializeDevice() {
            if (!serialPort->isOpen()) return false;
            // Ferrum retains command/query operation in text mode; its button
            // state path queries the device and does not consume the raw stream.
            if (deviceType == DeviceType::FERRUM) return true;
            if (!serialPort->beginRawButtonsAfterPrompt()) return false;
            constexpr std::string_view enable = "km.buttons(1)";
            return serialPort->sendCommand(enable.data(), enable.size());
        }

        // The caller owns a CommandScope, so reconnection cannot change the port
        // or protocol mode until this synchronous send finishes.
        bool sendWhileActive(std::string_view command) {
            const bool sent = serialPort->sendCommand(command.data(), command.size());
            if (!sent) markDisconnected();
            return sent;
        }

        bool executeCommand(std::string_view command) {
            CommandScope active(*this);
            return active && sendWhileActive(command);
        }

        bool executeMoveCommand(int32_t x, int32_t y) {
            CommandScope active(*this);
            if (!active) return false;
            // V4's documented domain: reject invalid movement instead of silently
            // clipping it and changing the requested total displacement.
            if (deviceType == DeviceType::MAKCU && !detail::isMakcuMoveInRange(x, y)) return false;
            thread_local detail::MoveCommandCache cache;
            return sendWhileActive(cache.format(x, y));
        }
    };

    Device::Device() : m_impl(std::make_unique<Impl>()) {}

    Device::~Device() {
        m_impl->stopCommands();
        m_impl->serialPort->close();
    }

    std::string Device::findFirstDevice() {
        const auto ports = SerialPort::findMakcuPorts();
        return ports.empty() ? "" : ports.front();
    }

    bool Device::connect(const std::string& port) {
        std::lock_guard<std::mutex> connection(m_impl->connectMutex);
        if (m_impl->isConnected() && (port.empty() || m_impl->serialPort->getPortName() == port))
            return true;

        m_impl->stopCommands();
        m_impl->serialPort->close();
        m_impl->deviceType = DeviceType::UNKNOWN;
        m_impl->deviceVersion.clear();
        const std::string targetPort = port.empty() ? findFirstDevice() : port;
        if (targetPort.empty()) return false;
        const uint32_t detectedBaud = m_impl->detectBaudRate(targetPort);
        if (detectedBaud == 0) return false;

        auto fail = [&] { m_impl->serialPort->close(); return false; };
        if (!m_impl->serialPort->open(targetPort, detectedBaud, false, m_impl->deviceType) ||
            !Impl::queryIdentity(*m_impl->serialPort, m_impl->deviceType, m_impl->deviceVersion)) return fail();

        if (detectedBaud == INITIAL_BAUD_RATE && m_impl->deviceType == DeviceType::MAKCU &&
            !m_impl->switchToHighSpeedMode()) {
            // A baud-change packet is not proof the firmware accepted it. Keep
            // the old speed only if it answers a fresh identity handshake.
            m_impl->serialPort->close();
            if (!m_impl->serialPort->open(targetPort, INITIAL_BAUD_RATE, false, m_impl->deviceType) ||
                !Impl::queryIdentity(*m_impl->serialPort, m_impl->deviceType, m_impl->deviceVersion)) return fail();
        }
        if (!m_impl->initializeDevice()) return fail();
        m_impl->startCommands();
        return true;
    }

    bool Device::isConnected() const { return m_impl->isConnected(); }

    bool Device::mouseDown(MouseButton button) {
        const unsigned index = static_cast<unsigned>(button);
        return index < PRESS_COMMANDS.size() && m_impl->executeCommand(PRESS_COMMANDS[index]);
    }

    bool Device::mouseUp(MouseButton button) {
        const unsigned index = static_cast<unsigned>(button);
        return index < RELEASE_COMMANDS.size() && m_impl->executeCommand(RELEASE_COMMANDS[index]);
    }

    bool Device::click(MouseButton button) {
        const unsigned index = static_cast<unsigned>(button);
        if (index >= PRESS_COMMANDS.size()) return false;
        Impl::CommandScope active(*m_impl);
        if (!active) return false;
        const bool pressed = m_impl->sendWhileActive(PRESS_COMMANDS[index]);
        const bool released = m_impl->sendWhileActive(RELEASE_COMMANDS[index]);
        return pressed && released;
    }

    bool Device::mouseButtonState(MouseButton button) {
        const unsigned index = static_cast<unsigned>(button);
        if (index >= 5) return false;
        Impl::CommandScope active(*m_impl);
        if (!active) return false;
        if (m_impl->deviceType != DeviceType::FERRUM)
            return (m_impl->serialPort->buttonMask() & (1u << index)) != 0;

        static constexpr const char* names[] = {"left", "right", "middle", "side1", "side2"};
        static constexpr const char* queries[] = {"km.left()", "km.right()", "km.middle()", "km.side1()", "km.side2()"};
        std::lock_guard<std::mutex> query(m_impl->queryMutex);
        try {
            auto future = m_impl->serialPort->sendTrackedCommand(queries[index], true, std::chrono::milliseconds(50));
            if (future.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready) return false;
            const int state = detail::parseButtonState(future.get(), names[index]);
            return state >= 0 && (state & 1) != 0;
        } catch (...) {
            return false;
        }
    }

    bool Device::mouseMove(int32_t x, int32_t y) {
        return m_impl->executeMoveCommand(x, y);
    }

} // namespace makcu

// ============================================================================
// Internal application entry points
// ============================================================================
static makcu::Device makcuDevice;

// --------------------------------------------------------------------------
// MAKCU 设备接口
// --------------------------------------------------------------------------
int saga::makcuIsConnected() {
    return makcuDevice.isConnected() ? 1 : 0;
}

int saga::makcuConnect(int port = 0) {

    if (port >= 1) {
        std::string portStr = "COM" + std::to_string(port);
        if (makcuDevice.connect(portStr.c_str())) {
            if (!makcuDevice.mouseMove(0, 0)) {
                return 0;
            }
            return 1;
        }
        return 0;
    }

    auto devicePorts = makcu::SerialPort::findMakcuPorts();
    if (devicePorts.empty()) {
        return 0;
    }
    for (const auto& testPort : devicePorts) {
        if (makcuDevice.connect(testPort)) {
            return 1;
        }
    }
    return 0;
}

int saga::makcuMove(int x, int y) {
    return makcuDevice.mouseMove(x, y) ? 1 : 0;
}

int saga::makcuClick(int key) {
    makcu::MouseButton btn;
    if (!makcu::MapMouseButton(key, btn)) return 0;
    return makcuDevice.click(btn) ? 1 : 0;
}

int saga::makcuMouseDown(int key) {
    makcu::MouseButton btn;
    if (!makcu::MapMouseButton(key, btn)) return 0;
    return makcuDevice.mouseDown(btn) ? 1 : 0;
}

int saga::makcuMouseUp(int key) {
    makcu::MouseButton btn;
    if (!makcu::MapMouseButton(key, btn)) return 0;
    return makcuDevice.mouseUp(btn) ? 1 : 0;
}

int saga::makcuMouseButtonState(int key) {
    makcu::MouseButton btn;
    if (!makcu::MapMouseButton(key, btn)) return 0;
    return makcuDevice.mouseButtonState(btn) ? 1 : 0;
}


// --------------------------------------------------------------------------
// CAT 设备接口(未做修改)
// --------------------------------------------------------------------------
static cat::Device catDevice;

extern "C"  int catConnect(const char* ip, int port, const char* uuid, int timeout_ms = 5000) {
    auto result = catDevice.connect(ip, port, uuid, timeout_ms);
    return static_cast<int>(result);
}

extern "C"  int catIsConnected() {
    return catDevice.isConnected() ? 1 : 0;
}

extern "C"  void catDisconnect() {
    catDevice.disconnect();
}

extern "C"  int catStartMonitor(int listen_port, int timeout_ms = 5000) {
    auto result = catDevice.startMonitor(listen_port, timeout_ms);
    return static_cast<int>(result);
}

extern "C"  void catStopMonitor() {
    catDevice.stopMonitor();
}

extern "C"  int catMouseMove(int x, int y) {
    auto result = catDevice.mouseMove(static_cast<int16_t>(x), static_cast<int16_t>(y));
    return static_cast<int>(result);
}

extern "C"  int catMouseClick(int button, int hold_ms = 50) {
    cat::MouseButton btn;
    switch (button) {
    case 1: btn = cat::MouseButton::LEFT; break;
    case 2: btn = cat::MouseButton::RIGHT; break;
    case 3: btn = cat::MouseButton::MIDDLE; break;
    case 5: btn = cat::MouseButton::SIDE1; break;
    case 6: btn = cat::MouseButton::SIDE2; break;
    default: return static_cast<int>(cat::ErrorCode::SEND_FAILED);
    }
    auto result = catDevice.click(btn, static_cast<int16_t>(hold_ms));
    return static_cast<int>(result);
}

extern "C"  int catMouseDown(int button) {
    cat::MouseButton btn;
    switch (button) {
    case 1: btn = cat::MouseButton::LEFT; break;
    case 2: btn = cat::MouseButton::RIGHT; break;
    case 3: btn = cat::MouseButton::MIDDLE; break;
    case 5: btn = cat::MouseButton::SIDE1; break;
    case 6: btn = cat::MouseButton::SIDE2; break;
    default: return static_cast<int>(cat::ErrorCode::SEND_FAILED);
    }
    auto result = catDevice.mouseDown(btn);
    return static_cast<int>(result);
}

extern "C"  int catMouseUp(int button) {
    cat::MouseButton btn;
    switch (button) {
    case 1: btn = cat::MouseButton::LEFT; break;
    case 2: btn = cat::MouseButton::RIGHT; break;
    case 3: btn = cat::MouseButton::MIDDLE; break;
    case 5: btn = cat::MouseButton::SIDE1; break;
    case 6: btn = cat::MouseButton::SIDE2; break;
    default: return static_cast<int>(cat::ErrorCode::SEND_FAILED);
    }
    auto result = catDevice.mouseUp(btn);
    return static_cast<int>(result);
}

extern "C"  int catKeyDown(int keycode) {
    auto result = catDevice.keyDown(static_cast<uint16_t>(keycode));
    return static_cast<int>(result);
}

extern "C"  int catKeyUp(int keycode) {
    auto result = catDevice.keyUp(static_cast<uint16_t>(keycode));
    return static_cast<int>(result);
}

extern "C"  int catKeyPress(int keycode, int hold_ms = 50) {
    auto result = catDevice.keyPress(static_cast<uint16_t>(keycode), static_cast<int16_t>(hold_ms));
    return static_cast<int>(result);
}

extern "C"  int catIsMousePressed(int button) {
    cat::MouseButton btn;
    switch (button) {
    case 1: btn = cat::MouseButton::LEFT; break;
    case 2: btn = cat::MouseButton::RIGHT; break;
    case 3: btn = cat::MouseButton::MIDDLE; break;
    case 5: btn = cat::MouseButton::SIDE1; break;
    case 6: btn = cat::MouseButton::SIDE2; break;
    default: return 0;
    }
    return catDevice.isMouseButtonPressed(btn) ? 1 : 0;
}

extern "C"  int catIsKeyPressed(int keycode) {
    return catDevice.isKeyPressed(static_cast<uint16_t>(keycode)) ? 1 : 0;
}

extern "C"  int catBlockMouse(int button) {
    cat::MouseButton btn;
    switch (button) {
    case 1: btn = cat::MouseButton::LEFT; break;
    case 2: btn = cat::MouseButton::RIGHT; break;
    case 3: btn = cat::MouseButton::MIDDLE; break;
    case 5: btn = cat::MouseButton::SIDE1; break;
    case 6: btn = cat::MouseButton::SIDE2; break;
    default: return static_cast<int>(cat::ErrorCode::SEND_FAILED);
    }
    auto result = catDevice.blockMouse(btn);
    return static_cast<int>(result);
}

extern "C"  int catUnblockMouse(int button) {
    cat::MouseButton btn;
    switch (button) {
    case 1: btn = cat::MouseButton::LEFT; break;
    case 2: btn = cat::MouseButton::RIGHT; break;
    case 3: btn = cat::MouseButton::MIDDLE; break;
    case 5: btn = cat::MouseButton::SIDE1; break;
    case 6: btn = cat::MouseButton::SIDE2; break;
    default: return static_cast<int>(cat::ErrorCode::SEND_FAILED);
    }
    auto result = catDevice.unblockMouse(btn);
    return static_cast<int>(result);
}

extern "C"  int catUnblockAllMouse() {
    auto result = catDevice.unblockAllMouse();
    return static_cast<int>(result);
}

extern "C"  int catBlockKey(int keycode) {
    auto result = catDevice.blockKey(static_cast<uint16_t>(keycode));
    return static_cast<int>(result);
}

extern "C"  int catUnblockKey(int keycode) {
    auto result = catDevice.unblockKey(static_cast<uint16_t>(keycode));
    return static_cast<int>(result);
}

extern "C"  int catUnblockAllKeys() {
    auto result = catDevice.unblockAllKeys();
    return static_cast<int>(result);
}