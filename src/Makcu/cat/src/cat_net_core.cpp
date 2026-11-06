#include "cat_net_core.h"

#include <iostream>
#include <thread>

CatNetCore::CatNetCore() : read_io_context(), send_io_context(), read_socket(read_io_context), send_socket(send_io_context) {
}

CatNetCore::~CatNetCore() { closeMonitor(); }

CatNetCore::ErrorCode CatNetCore::init(const std::string& box_ip, int box_port, const std::string& uuid, int milliseconds) {
    try {
        closeMonitor();
        asio::error_code ignored;
        send_socket.close(ignored);
        is_init = false;
        m_key = expandTo16Bytes(uuid);
        box_endpoint = asio::ip::udp::endpoint(asio::ip::make_address(box_ip), box_port);
        send_socket.open(asio::ip::udp::v4());
        send_socket.bind(asio::ip::udp::endpoint(asio::ip::udp::v4(), 0));
        
        CmdData cmd_data{
                .cmd = CMD_CONNECT
        };
        ErrorCode err = sendCmd(cmd_data, milliseconds);
        if (err != SUCCESS) {
            return err;
        }

        is_init = true;
        return SUCCESS;
    }
    catch (const std::exception&) {
        return SOCKET_FAILED;
    }
}

CatNetCore::ErrorCode CatNetCore::monitor(int server_port, int milliseconds) {
    if (!is_init) {
        return INIT_FAILED;
    }
    if (is_monitor) {
        return MONITOR_OPEN;
    }
    try {
        server_endpoint = asio::ip::udp::endpoint(asio::ip::udp::v4(), server_port);
        read_socket.open(asio::ip::udp::v4());
        read_socket.bind(server_endpoint);
        
        CmdData cmd_data{
                .cmd = CMD_MONITOR,
                .options = read_socket.local_endpoint().port()
        };
        is_monitor = true;
        startReceive();
        read_io_context_thread = std::thread([this]() {
            read_io_context.run();
        });

        ErrorCode err = sendCmd(cmd_data, milliseconds);
        if (err != SUCCESS) {
            closeMonitor();
            return err;
        }

        return SUCCESS;
    }
    catch (const std::exception&) {
        closeMonitor();
        return SOCKET_FAILED;
    }
}

void CatNetCore::closeMonitor() {
    is_monitor = false;
    read_io_context.stop();
    if (read_io_context_thread.joinable()) read_io_context_thread.join();
    asio::error_code ignored;
    read_socket.close(ignored);
    // Drain cancelled handlers before any new monitor session starts.
    read_io_context.restart();
    read_io_context.poll();
    read_io_context.restart();
    std::lock_guard<std::mutex> state(state_mutex);
    mouse_state = {};
    keyboard_state = {};
    lock_state = 0;
}

void CatNetCore::hidHandle(std::size_t receive_len) {
    if (receive_len < AES_BLOCK_SIZE) {
        return;
    }
    const auto* encrypt_buf = reinterpret_cast<const unsigned char*>(buffer.data());
    unsigned char buf[1024];
    int len = aes128CBCDecrypt(encrypt_buf, static_cast<int>(receive_len), m_key.data(), buf);
    
    HidData hid_data{};
    if (len >= sizeof(HidData)) {
        std::memcpy(&hid_data, buf, sizeof(HidData));
    }
    else {
        return;
    }

    std::lock_guard<std::mutex> state(state_mutex);
    mouse_state.updateKeyState(hid_data.mouse_data.code, hid_data.mouse_data.value);
    mouse_state.updateAxis(hid_data.mouse_data.mouseEvent.x, hid_data.mouse_data.mouseEvent.y, hid_data.mouse_data.mouseEvent.wheel);

    keyboard_state.updateKeyState(hid_data.keyboard_data.code, hid_data.keyboard_data.value);
    lock_state = hid_data.keyboard_data.lock;
}

void CatNetCore::startReceive() {
    read_socket.async_receive_from(
        asio::buffer(buffer), monitor_sender,
        [this](asio::error_code ec, std::size_t len) {
            if (!ec && len > 0 && monitor_sender.address() == box_endpoint.address()) {
                hidHandle(len);
            }
            if (!ec && is_monitor.load()) startReceive();
            if (ec) is_monitor = false;
        });
}

CatNetCore::ErrorCode CatNetCore::receiveAck(CmdData data, int milliseconds) {
    std::array<char, 1024> buf{};
    const auto sock_fd = send_socket.native_handle();

    timeval timeout{};
    timeval* timeout_ptr = nullptr;

    if (milliseconds != -1) {
        timeout.tv_sec = milliseconds / 1000;
        timeout.tv_usec = (milliseconds % 1000) * 1000;
        timeout_ptr = &timeout;
    }

    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(sock_fd, &read_fds);
    int result = select(0, &read_fds, nullptr, nullptr, timeout_ptr);
    if (result > 0) {
        asio::ip::udp::endpoint sender;
        size_t len = send_socket.receive_from(asio::buffer(buf), sender);
        if (sender != box_endpoint) return RECEIVE_FAILED;
        if (len > 0) {
            const auto* encrypt_buf = reinterpret_cast<const unsigned char*>(buf.data());
            unsigned char decrypt_buf[1024];
            CmdData cmdData{};
            int decrypt_len = aes128CBCDecrypt(encrypt_buf, static_cast<int>(len), m_key.data(), decrypt_buf);
            if (decrypt_len >= sizeof(CmdData)) {
                std::memcpy(&cmdData, decrypt_buf, sizeof(CmdData));
                if (cmdData.cmd == data.cmd) {
                    return SUCCESS;
                }
            }
            return RECEIVE_FAILED;
        }
    }
    else if (result == 0) {
        return RECEIVE_TIMEOUT;
    }
    return RECEIVE_FAILED;
}

CatNetCore::ErrorCode CatNetCore::sendCmd(CmdData data, int milliseconds) {
    if (!send_socket.is_open()) {
        return SOCKET_FAILED;
    }
    unsigned char iv[AES_BLOCK_SIZE];
    generateRandomIV(iv, AES_BLOCK_SIZE);
    unsigned char cmd_buf[sizeof(CmdData)];
    memcpy(cmd_buf, &data, sizeof(CmdData));

    unsigned char encrypt_buf[1024];
    int encrypt_len = aes128CBCEncrypt(cmd_buf, sizeof(CmdData), m_key.data(), iv, encrypt_buf);

    if (encrypt_len <= 0) {
        return ENCRYPTION_FAILED;
    }

    try {
        for (unsigned drained = 0; drained < 64; ++drained) {
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(send_socket.native_handle(), &read_fds);
            timeval timeout = { 0, 0 };
            int result = select(0, &read_fds, nullptr, nullptr, &timeout);
            if (result > 0) {
                std::array<char, 1024> recv_buf{};
                asio::error_code ec;
                asio::ip::udp::endpoint sender;
                send_socket.receive_from(asio::buffer(recv_buf), sender, 0, ec);
                if (ec) return RECEIVE_FAILED;
            }
            else if (result == 0) {
                break;
            }
            else return RECEIVE_FAILED;
        }

        std::size_t size = send_socket.send_to(asio::buffer(encrypt_buf, encrypt_len), box_endpoint);
        if (size <= 0) {
            return SEND_FAILED;
        }
        return receiveAck(data, milliseconds);
    }
    catch (asio::system_error&) {
        return SEND_FAILED;
    }
}

CatNetCore::ErrorCode CatNetCore::mouseMove(int16_t x, int16_t y) {
    if (!is_init) {
        return INIT_FAILED;
    }
    CmdData cmd_data{
            .cmd = CMD_MOUSE_MOVE
    };
    cmd_data.value1 = x;
    cmd_data.value2 = y;
    return sendCmd(cmd_data);
}

CatNetCore::ErrorCode CatNetCore::mouseMoveAuto(int16_t x, int16_t y, int16_t ms) {
    if (!is_init) {
        return INIT_FAILED;
    }
    CmdData cmd_data{
            .cmd = CMD_MOUSE_AUTO_MOVE
    };
    cmd_data.options = ms;
    cmd_data.value1 = x;
    cmd_data.value2 = y;
    return sendCmd(cmd_data, ms + 300);
}

CatNetCore::ErrorCode CatNetCore::mouseButton(uint16_t code, uint16_t value) {
    if (!is_init) {
        return INIT_FAILED;
    }
    CmdData cmd_data{
            .cmd = CMD_MOUSE_BUTTON
    };
    cmd_data.options = code;
    cmd_data.value1 = static_cast<int16_t>(value);
    return sendCmd(cmd_data);
}

CatNetCore::ErrorCode CatNetCore::tapMouseButton(uint16_t code, int16_t ms) {
    if (!is_init) {
        return INIT_FAILED;
    }
    CmdData cmd_data{
            .cmd = CMD_MOUSE_BUTTON
    };
    cmd_data.options = code;
    cmd_data.value1 = 1;
    sendCmd(cmd_data);
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    cmd_data.value1 = 0;
    return sendCmd(cmd_data);
}

CatNetCore::ErrorCode CatNetCore::keyboardButton(uint16_t code, uint16_t value) {
    if (!is_init) {
        return INIT_FAILED;
    }
    CmdData cmd_data{
            .cmd = CMD_KEYBOARD_BUTTON
    };
    cmd_data.options = code;
    cmd_data.value1 = static_cast<int16_t>(value);
    return sendCmd(cmd_data);
}

CatNetCore::ErrorCode CatNetCore::tapKeyboardButton(uint16_t code, int16_t ms) {
    if (!is_init) {
        return INIT_FAILED;
    }
    CmdData cmd_data{
            .cmd = CMD_KEYBOARD_BUTTON
    };
    cmd_data.options = code;
    cmd_data.value1 = 1;
    sendCmd(cmd_data);
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    cmd_data.value1 = 0;
    return sendCmd(cmd_data);
}

bool CatNetCore::isMousePressed(uint16_t code) {
    std::lock_guard<std::mutex> state(state_mutex);
    if (!is_monitor) {
        return false;
    }
    return mouse_state.isKeyPressed(code);
}

bool CatNetCore::isKeyboardPressed(uint16_t code) {
    std::lock_guard<std::mutex> state(state_mutex);
    if (!is_monitor) {
        return false;
    }
    return keyboard_state.isKeyPressed(code);
}

bool CatNetCore::isLockKeyPressed(uint16_t code) {
    std::lock_guard<std::mutex> state(state_mutex);
    if (!is_monitor) {
        return false;
    }
    return (lock_state & code) != 0;
}

CatNetCore::ErrorCode CatNetCore::blockedMouse(uint16_t code, uint16_t value) {
    if (!is_init) {
        return INIT_FAILED;
    }
    CmdData cmd_data{
            .cmd = CMD_BLOCKED
    };
    cmd_data.options = 0x01;
    cmd_data.value1 = static_cast<int16_t>(code);
    cmd_data.value2 = static_cast<int16_t>(value);
    return sendCmd(cmd_data);
}

CatNetCore::ErrorCode CatNetCore::blockedKeyboard(uint16_t code, uint16_t value) {
    if (!is_init) {
        return INIT_FAILED;
    }
    CmdData cmd_data{
            .cmd = CMD_BLOCKED
    };
    cmd_data.options = 0x02;
    cmd_data.value1 = static_cast<int16_t>(code);
    cmd_data.value2 = static_cast<int16_t>(value);
    return sendCmd(cmd_data);
}

CatNetCore::ErrorCode CatNetCore::unblockedMouseAll() {
    if (!is_init) {
        return INIT_FAILED;
    }
    CmdData cmd_data{
            .cmd = CMD_UNBLOCKED_MOUSE_ALL
    };
    return sendCmd(cmd_data);
}

CatNetCore::ErrorCode CatNetCore::unblockedKeyboardAll() {
    if (!is_init) {
        return INIT_FAILED;
    }
    CmdData cmd_data{
            .cmd = CMD_UNBLOCKED_KEYBOARD_ALL
    };
    return sendCmd(cmd_data);
}
