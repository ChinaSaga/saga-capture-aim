#pragma once

#include <string>
#include <memory>
#include <cstdint>
#include <functional>

namespace cat {

    // 设备错误代码
    enum class ErrorCode {
        SUCCESS = 0,
        DECRYPTION_FAILED = 100,
        ENCRYPTION_FAILED = 101,
        SEND_FAILED = 102,
        RECEIVE_FAILED = 103,
        RECEIVE_TIMEOUT = 104,
        INIT_FAILED = 300,
        MONITOR_CLOSE = 301,
        MONITOR_OPEN = 302,
        SOCKET_FAILED = 500,
        SOCKET_TIMEOUT = 501
    };

    // 鼠标按键代码 (Linux input-event-codes)
    enum class MouseButton : uint16_t {
        LEFT = 0x110,    // BTN_LEFT
        RIGHT = 0x111,   // BTN_RIGHT
        MIDDLE = 0x112,  // BTN_MIDDLE
        SIDE1 = 0x113,   // BTN_SIDE
        SIDE2 = 0x114    // BTN_EXTRA
    };

    // 键盘按键回调类型
    using KeyboardCallback = std::function<void(uint16_t code, bool pressed)>;
    using MouseButtonCallback = std::function<void(uint16_t code, bool pressed)>;

    // CAT设备类 - 网络键鼠控制
    class Device {
    public:
        Device();
        ~Device();

        // 核心连接功能
        ErrorCode connect(const std::string& ip, int port, const std::string& uuid, int timeout_ms = 5000);
        bool isConnected() const;
        void disconnect();

        // 监听功能
        ErrorCode startMonitor(int listen_port, int timeout_ms = 5000);
        void stopMonitor();
        bool isMonitoring() const;

        // 鼠标控制
        ErrorCode mouseMove(int16_t x, int16_t y);
        ErrorCode mouseMoveAuto(int16_t x, int16_t y, int16_t ms);
        ErrorCode mouseDown(MouseButton button);
        ErrorCode mouseUp(MouseButton button);
        ErrorCode click(MouseButton button, int16_t hold_ms = 50);

        // 键盘控制
        ErrorCode keyDown(uint16_t keycode);
        ErrorCode keyUp(uint16_t keycode);
        ErrorCode keyPress(uint16_t keycode, int16_t hold_ms = 50);

        // 状态查询
        bool isMouseButtonPressed(MouseButton button);
        bool isKeyPressed(uint16_t keycode);
        bool isLockKeyPressed(uint16_t lockkey);

        // 按键屏蔽
        ErrorCode blockMouse(MouseButton button);
        ErrorCode unblockMouse(MouseButton button);
        ErrorCode unblockAllMouse();
        ErrorCode blockKey(uint16_t keycode);
        ErrorCode unblockKey(uint16_t keycode);
        ErrorCode unblockAllKeys();

        // 回调设置
        void setMouseButtonCallback(MouseButtonCallback callback);
        void setKeyboardCallback(KeyboardCallback callback);

    private:
        class Impl;
        std::unique_ptr<Impl> m_impl;

        Device(const Device&) = delete;
        Device& operator=(const Device&) = delete;
    };

} // namespace cat