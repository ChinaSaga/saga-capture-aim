#pragma once

#include <string>
#include <memory>
#include <cstdint>

namespace makcu {

    // Forward declaration
    class SerialPort;

    // 设备类型枚举
    enum class DeviceType {
        UNKNOWN = 0,
        MAKCU = 1,
        FERRUM = 2
    };

    // Enums
    enum class MouseButton : uint8_t {
        LEFT = 0,
        RIGHT = 1,
        MIDDLE = 2,
        SIDE1 = 3,
        SIDE2 = 4
    };

    // Main Device class - Minimal MAKCU Mouse Controller
    class Device {
    public:
        Device();
        ~Device();

        // Core functions
        bool connect(const std::string& port = "");
        bool isConnected() const;
        // MAKCU accepts -32768..32767 per axis; invalid requests return false.
        // Repeated equal relative moves are each sent. No device-side smoothing
        // command is substituted for the existing km.move protocol.
        bool mouseMove(int32_t x, int32_t y);
        bool click(MouseButton button);
        bool mouseDown(MouseButton button);
        bool mouseUp(MouseButton button);
        bool mouseButtonState(MouseButton button);

    private:
        class Impl;
        std::unique_ptr<Impl> m_impl;
        
        static std::string findFirstDevice();
        Device(const Device&) = delete;
        Device& operator=(const Device&) = delete;
    };
} // namespace makcu
