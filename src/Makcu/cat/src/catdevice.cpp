#include "../include/catdevice.h"
#include "../../cat/asio/include/asio.hpp"
#include "../src/cat_net_core.h"
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>

namespace cat {

    // Device实现类
    class Device::Impl {
    public:
        std::unique_ptr<CatNetCore> catNet;
        std::atomic<bool> connected;
        std::atomic<bool> monitoring;
        std::mutex mutex;

        MouseButtonCallback mouseButtonCallback;
        KeyboardCallback keyboardCallback;

        Impl() : catNet(std::make_unique<CatNetCore>())
            , connected(false)
            , monitoring(false) {
        }

        ~Impl() {
            if (monitoring.load()) {
                catNet->closeMonitor();
            }
        }
    };

    Device::Device() : m_impl(std::make_unique<Impl>()) {}

    Device::~Device() {
        disconnect();
    }

    ErrorCode Device::connect(const std::string& ip, int port, const std::string& uuid, int timeout_ms) {
        std::lock_guard<std::mutex> lock(m_impl->mutex);

        if (m_impl->connected.load()) {
            return ErrorCode::SUCCESS;
        }

        auto result = m_impl->catNet->init(ip, port, uuid, timeout_ms);
        
        if (result == CatNetCore::ErrorCode::SUCCESS) {
            m_impl->connected.store(true);
            return ErrorCode::SUCCESS;
        }

        return static_cast<ErrorCode>(result);
    }

    bool Device::isConnected() const {
        return m_impl->connected.load();
    }

    void Device::disconnect() {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        
        if (m_impl->monitoring.load()) {
            m_impl->catNet->closeMonitor();
            m_impl->monitoring.store(false);
        }
        
        m_impl->connected.store(false);
    }

    ErrorCode Device::startMonitor(int listen_port, int timeout_ms) {
        if (!m_impl->connected.load()) {
            return ErrorCode::INIT_FAILED;
        }

        if (m_impl->monitoring.load()) {
            return ErrorCode::MONITOR_OPEN;
        }

        auto result = m_impl->catNet->monitor(listen_port, timeout_ms);
        
        if (result == CatNetCore::ErrorCode::SUCCESS) {
            m_impl->monitoring.store(true);
            return ErrorCode::SUCCESS;
        }

        return static_cast<ErrorCode>(result);
    }

    void Device::stopMonitor() {
        if (m_impl->monitoring.load()) {
            m_impl->catNet->closeMonitor();
            m_impl->monitoring.store(false);
        }
    }

    bool Device::isMonitoring() const {
        return m_impl->monitoring.load();
    }

    ErrorCode Device::mouseMove(int16_t x, int16_t y) {
        if (!m_impl->connected.load()) {
            return ErrorCode::INIT_FAILED;
        }

        auto result = m_impl->catNet->mouseMove(x, y);
        return static_cast<ErrorCode>(result);
    }

    ErrorCode Device::mouseMoveAuto(int16_t x, int16_t y, int16_t ms) {
        if (!m_impl->connected.load()) {
            return ErrorCode::INIT_FAILED;
        }

        auto result = m_impl->catNet->mouseMoveAuto(x, y, ms);
        return static_cast<ErrorCode>(result);
    }

    ErrorCode Device::mouseDown(MouseButton button) {
        if (!m_impl->connected.load()) {
            return ErrorCode::INIT_FAILED;
        }

        auto result = m_impl->catNet->mouseButton(static_cast<uint16_t>(button), 1);
        return static_cast<ErrorCode>(result);
    }

    ErrorCode Device::mouseUp(MouseButton button) {
        if (!m_impl->connected.load()) {
            return ErrorCode::INIT_FAILED;
        }

        auto result = m_impl->catNet->mouseButton(static_cast<uint16_t>(button), 0);
        return static_cast<ErrorCode>(result);
    }

    ErrorCode Device::click(MouseButton button, int16_t hold_ms) {
        if (!m_impl->connected.load()) {
            return ErrorCode::INIT_FAILED;
        }

        auto result = m_impl->catNet->tapMouseButton(static_cast<uint16_t>(button), hold_ms);
        return static_cast<ErrorCode>(result);
    }

    ErrorCode Device::keyDown(uint16_t keycode) {
        if (!m_impl->connected.load()) {
            return ErrorCode::INIT_FAILED;
        }

        auto result = m_impl->catNet->keyboardButton(keycode, 1);
        return static_cast<ErrorCode>(result);
    }

    ErrorCode Device::keyUp(uint16_t keycode) {
        if (!m_impl->connected.load()) {
            return ErrorCode::INIT_FAILED;
        }

        auto result = m_impl->catNet->keyboardButton(keycode, 0);
        return static_cast<ErrorCode>(result);
    }

    ErrorCode Device::keyPress(uint16_t keycode, int16_t hold_ms) {
        if (!m_impl->connected.load()) {
            return ErrorCode::INIT_FAILED;
        }

        auto result = m_impl->catNet->tapKeyboardButton(keycode, hold_ms);
        return static_cast<ErrorCode>(result);
    }

    bool Device::isMouseButtonPressed(MouseButton button) {
        if (!m_impl->monitoring.load()) {
            return false;
        }

        return m_impl->catNet->isMousePressed(static_cast<uint16_t>(button));
    }

    bool Device::isKeyPressed(uint16_t keycode) {
        if (!m_impl->monitoring.load()) {
            return false;
        }

        return m_impl->catNet->isKeyboardPressed(keycode);
    }

    bool Device::isLockKeyPressed(uint16_t lockkey) {
        if (!m_impl->monitoring.load()) {
            return false;
        }

        return m_impl->catNet->isLockKeyPressed(lockkey);
    }

    ErrorCode Device::blockMouse(MouseButton button) {
        if (!m_impl->connected.load()) {
            return ErrorCode::INIT_FAILED;
        }

        auto result = m_impl->catNet->blockedMouse(static_cast<uint16_t>(button), 1);
        return static_cast<ErrorCode>(result);
    }

    ErrorCode Device::unblockMouse(MouseButton button) {
        if (!m_impl->connected.load()) {
            return ErrorCode::INIT_FAILED;
        }

        auto result = m_impl->catNet->blockedMouse(static_cast<uint16_t>(button), 0);
        return static_cast<ErrorCode>(result);
    }

    ErrorCode Device::unblockAllMouse() {
        if (!m_impl->connected.load()) {
            return ErrorCode::INIT_FAILED;
        }

        auto result = m_impl->catNet->unblockedMouseAll();
        return static_cast<ErrorCode>(result);
    }

    ErrorCode Device::blockKey(uint16_t keycode) {
        if (!m_impl->connected.load()) {
            return ErrorCode::INIT_FAILED;
        }

        auto result = m_impl->catNet->blockedKeyboard(keycode, 1);
        return static_cast<ErrorCode>(result);
    }

    ErrorCode Device::unblockKey(uint16_t keycode) {
        if (!m_impl->connected.load()) {
            return ErrorCode::INIT_FAILED;
        }

        auto result = m_impl->catNet->blockedKeyboard(keycode, 0);
        return static_cast<ErrorCode>(result);
    }

    ErrorCode Device::unblockAllKeys() {
        if (!m_impl->connected.load()) {
            return ErrorCode::INIT_FAILED;
        }

        auto result = m_impl->catNet->unblockedKeyboardAll();
        return static_cast<ErrorCode>(result);
    }

    void Device::setMouseButtonCallback(MouseButtonCallback callback) {
        m_impl->mouseButtonCallback = callback;
    }

    void Device::setKeyboardCallback(KeyboardCallback callback) {
        m_impl->keyboardCallback = callback;
    }

} // namespace cat