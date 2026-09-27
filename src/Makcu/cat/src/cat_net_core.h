#ifndef CAT_NET_CORE_H
#define CAT_NET_CORE_H

#include "../asio/include/asio.hpp"
#include <sstream>
#include <iomanip>
#include <cstring>
#include <bitset>
#include <cstdint>
#include "aes_functions.h"
#include <random>
#include "input-event-codes.h"

#define AES_BLOCK_SIZE 16

class CatNetCore {
private:
    static constexpr uint8_t CMD_CONNECT = 0x01;
    static constexpr uint8_t CMD_MONITOR = 0x02;
    static constexpr uint8_t CMD_MOUSE_BUTTON = 0x03;
    static constexpr uint8_t CMD_KEYBOARD_BUTTON = 0x04;
    static constexpr uint8_t CMD_BLOCKED = 0x05;
    static constexpr uint8_t CMD_UNBLOCKED_MOUSE_ALL = 0x06;
    static constexpr uint8_t CMD_UNBLOCKED_KEYBOARD_ALL = 0x07;
    static constexpr uint8_t CMD_MOUSE_MOVE = 0x08;
    static constexpr uint8_t CMD_MOUSE_AUTO_MOVE = 0x09;

#pragma pack(1)
    struct CmdData {
        uint8_t cmd;
        uint16_t options;
        int16_t value1;
        int16_t value2;
    };

    struct MouseEvent {
        int16_t x;
        int16_t y;
        int16_t wheel;
    };

    struct MouseData {
        uint16_t code;
        uint16_t value;
        MouseEvent mouseEvent;
    };

    struct KeyboardData {
        uint16_t code;
        uint16_t value;
        uint8_t lock;
    };

    struct HidData {
        MouseData mouse_data;
        KeyboardData keyboard_data;
    };
#pragma pack()

    struct KeyStateManager {
        std::bitset<KEY_CNT> keyStateBitset;

        KeyStateManager() {
            keyStateBitset.reset();
        }

        void updateKeyState(uint16_t key, bool isPressed) {
            if (key <= KEY_CNT) {
                keyStateBitset.set(key, isPressed);
            }
        }

        [[nodiscard]] bool isKeyPressed(uint16_t key) const {
            if (key <= KEY_CNT) {
                return keyStateBitset.test(key);
            }
            return false;
        }
    };

    struct MouseStateManager {
        std::bitset<KEY_CNT> keyStateBitset;
        MouseEvent mouseEvent{};

        MouseStateManager() {
            keyStateBitset.reset();
        }

        void updateAxis(int16_t x, int16_t y, int16_t wheel) {
            mouseEvent.x = x;
            mouseEvent.y = y;
            mouseEvent.wheel = wheel;
        }

        MouseEvent& getMouseEvent() {
            return mouseEvent;
        }

        void updateKeyState(uint16_t key, bool isPressed) {
            if (key <= KEY_CNT) {
                keyStateBitset.set(key, isPressed);
            }
        }

        bool isKeyPressed(uint16_t key) {
            if (key <= KEY_CNT) {
                return keyStateBitset.test(key);
            }
            return false;
        }
    };

public:
    /**
     * @brief 表示NET操作的错误代码
     *
     * 枚举类型用于描述不同类型的网络操作错误及其状态码。
     */
    enum ErrorCode {
        /**
         * @brief 操作成功
         *
         * 表示操作已成功完成，没有发生任何错误。
         */
        SUCCESS = 0,

        /**
         * @brief 解密失败
         *
         * 表示在解密过程中发生错误。
         */
        DECRYPTION_FAILED = 100,

        /**
         * @brief 加密失败
         *
         * 表示在加密过程中发生错误。
         */
        ENCRYPTION_FAILED = 101,

        /**
         * @brief 发送失败
         *
         * 表示在数据发送过程中发生错误。
         */
        SEND_FAILED = 102,

        /**
         * @brief 接收失败
         *
         * 表示在数据接收过程中发生错误。
         */
        RECEIVE_FAILED = 103,

        /**
         * @brief 接收超时
         *
         * 表示在规定时间内没有接收到数据，发生超时错误。
         */
        RECEIVE_TIMEOUT = 104,

        /**
         * @brief 初始化失败
         *
         * 表示系统或网络模块初始化失败。
         */
        INIT_FAILED = 300,

        /**
         * @brief 监听未开启
         *
         * 表示监视器没有正确启动。
         */
        MONITOR_CLOSE = 301,

        /**
         * @brief 监听已开启
         *
         * 表示监视器已经成功开启。
         */
        MONITOR_OPEN = 302,

        /**
         * @brief socket 错误
         *
         * 表示在 socket 操作过程中发生错误。
         */
        SOCKET_FAILED = 500,

        /**
         * @brief socket 超时
         *
         * 表示 socket 操作超时。
         */
        SOCKET_TIMEOUT = 501
    };

    CatNetCore();

    /**
     * 初始化函数 超时请检查UUID是否正确
     * @param box_ip 盒子ip
     * @param box_port 盒子端口
     * @param uuid 盒子uuid
     * @param milliseconds 初始化盒子响应ACK超时时间 单位ms -1为无限等待
     * @return ErrorCode
     */
    ErrorCode init(const std::string& box_ip, int box_port, const std::string& uuid, int milliseconds = 5000);

    /**
     * 开启鼠键事件监听
     * @param server_port 本机监听的端口
     * @param milliseconds 初始化盒子响应ACK超时时间 单位ms -1为无限等待
     * @return ErrorCode
     */
    ErrorCode monitor(int server_port, int milliseconds = 5000);

    /**
     * 关闭键鼠事件监听
     */
    void closeMonitor();

    // 控制部分
    /**
     * 鼠标移动
     * @param x 正值向右
     * @param y 正值向下
     * @return ErrorCode
     */
    CatNetCore::ErrorCode mouseMove(int16_t x, int16_t y);

    /**
     * 鼠标算法优化移动
     * @param x 正值向下
     * @param y 正值向右
     * @param ms 移动时间
     * @return ErrorCode
     */
    CatNetCore::ErrorCode mouseMoveAuto(int16_t x, int16_t y, int16_t ms);

    /**
     * 鼠标按键触发
     * @param code 按键值，参考event-codes
     * @param value 按下1 释放0
     * @return ErrorCode
     */
    CatNetCore::ErrorCode mouseButton(uint16_t code, uint16_t value);

    /**
     * 鼠标按下多少ms后释放
     * @param code 按键值，参考event-codes
     * @param ms 毫秒
     * @return ErrorCode
     */
    CatNetCore::ErrorCode tapMouseButton(uint16_t code, int16_t ms);

    /**
     * 键盘按键触发
     * @param code 按键值，参考event-codes
     * @param value 按下1 释放0
     * @return ErrorCode
     */
    CatNetCore::ErrorCode keyboardButton(uint16_t code, uint16_t value);

    /**
     * 键盘按键按下多少ms后释放
     * @param code 按键值，参考event-codes
     * @param ms 毫秒
     * @return ErrorCode
     */
    CatNetCore::ErrorCode tapKeyboardButton(uint16_t code, int16_t ms);

    // 监测部分
    /**
     * 监测鼠标某个键是否按下
     * @param code 按键值，参考event-codes
     * @return 按下true 释放false
     */
    bool isMousePressed(uint16_t code);

    /**
     * 监测键盘按键某个键是否按下
     * @param code 按键值，参考event-codes
     * @return 按下true 释放false
     */
    bool isKeyboardPressed(uint16_t code);

    /**
     * 监测锁定键是否按下
     * @param code NumLock CapsLock ScrLOCK
     * @return 按下true 释放false
     */
    bool isLockKeyPressed(uint16_t code);

    // 屏蔽部分
    /**
     * 屏蔽鼠标某个键
     * @param code  按键值，参考event-codes
     * @param value  屏蔽1 解除屏蔽0
     * @return ErrorCode
     */
    CatNetCore::ErrorCode blockedMouse(uint16_t code, uint16_t value);

    /**
     * 屏蔽键盘某个键
     * @param code  按键值，参考event-codes
     * @param value  屏蔽1 解除屏蔽0
     * @return ErrorCode
     */
    CatNetCore::ErrorCode blockedKeyboard(uint16_t code, uint16_t value);

    /**
     * 解除鼠标所有屏蔽按键
     * @return ErrorCode
     */
    CatNetCore::ErrorCode unblockedMouseAll();

    /**
     * 解除键盘所有屏蔽按键
     * @return ErrorCode
     */
    CatNetCore::ErrorCode unblockedKeyboardAll();

private:
    bool is_init = false;
    bool is_monitor = false;
    asio::io_context read_io_context;
    asio::ip::udp::socket read_socket;
    asio::ip::udp::endpoint server_endpoint;
    asio::io_context send_io_context;
    asio::ip::udp::socket send_socket;
    asio::ip::udp::endpoint box_endpoint;
    std::thread read_io_context_thread;
    std::array<char, 1024> buffer{};

    const unsigned char* m_key{};

    MouseStateManager mouse_state;
    KeyStateManager keyboard_state;
    uint8_t lock_state{};

private:
    void startReceive();

    ErrorCode sendCmd(CmdData data, int milliseconds = 300);

    void hidHandle(std::size_t receive_len);

    ErrorCode receiveAck(CmdData data, int milliseconds);

    static int aes128CBCEncrypt(const unsigned char* buf, int buf_len, const unsigned char* key, const unsigned char* iv,
        unsigned char* encrypt_buf) {
        std::vector<unsigned char> padded_buf(buf, buf + buf_len);
        int padding = 16 - (buf_len % 16);
        padded_buf.insert(padded_buf.end(), padding, static_cast<unsigned char>(padding));

        std::array<unsigned char, 176> expanded_key{};
        key_expansion(key, expanded_key.data());

        std::memcpy(encrypt_buf, iv, 16);
        unsigned char* output = encrypt_buf + 16;

        for (size_t i = 0; i < padded_buf.size(); i += 16) {
            for (int j = 0; j < 16; ++j) {
                output[j] = padded_buf[i + j] ^ (i == 0 ? iv[j] : output[j - 16]);
            }

            add_round_key(output, expanded_key.data());

            for (int round = 1; round < 10; ++round) {
                aes_encrypt_round(output, expanded_key.data() + round * 16);
            }

            sub_bytes(output);
            shift_rows(output);
            add_round_key(output, expanded_key.data() + 160);

            output += 16;
        }

        return static_cast<int>(16 + padded_buf.size());
    }

    static int aes128CBCDecrypt(const unsigned char* encrypt_buf, int encrypt_buf_len, const unsigned char* key,
        unsigned char* decrypt_buf) {
        if (encrypt_buf_len % 16 != 0 || encrypt_buf_len < 32) {
            return -1;
        }

        std::array<unsigned char, 176> expanded_key{};
        key_expansion(key, expanded_key.data());

        const unsigned char* iv = encrypt_buf;
        const unsigned char* input = encrypt_buf + 16;
        unsigned char* output = decrypt_buf;
        int input_len = encrypt_buf_len - 16;

        for (int i = 0; i < input_len; i += 16) {
            std::array<unsigned char, 16> temp{};
            std::memcpy(temp.data(), input + i, 16);

            add_round_key(temp.data(), expanded_key.data() + 160);
            inv_shift_rows(temp.data());
            inv_sub_bytes(temp.data());

            for (int round = 9; round > 0; --round) {
                aes_decrypt_round(temp.data(), expanded_key.data() + round * 16);
            }

            add_round_key(temp.data(), expanded_key.data());

            for (int j = 0; j < 16; ++j) {
                output[i + j] = temp[j] ^ (i == 0 ? iv[j] : input[i + j - 16]);
            }
        }

        int padding = output[input_len - 1];
        if (padding < 1 || padding > 16) {
            return -1;
        }

        return input_len - padding;
    }

    static unsigned char* expandTo16Bytes(const std::string& cat_uuid) {
        auto uuid = static_cast<uint32_t>(std::stoul(cat_uuid, nullptr, 16));
        static unsigned char enc_key[16];
        std::memset(enc_key, 0, sizeof(enc_key));

        std::stringstream ss_uuid;
        ss_uuid << std::hex << std::setw(8) << std::setfill('0') << uuid;
        std::string str_uuid = ss_uuid.str();

        for (size_t i = 0; i < 16; ++i) {
            enc_key[i] = (i < str_uuid.size()) ? str_uuid[i] : '0';
        }

        return enc_key;
    }

    static void generateRandomIV(unsigned char* iv, size_t size) {
        std::random_device rd;
        std::mt19937 generator(rd());
        std::uniform_int_distribution<int> distribution(0, 255);

        for (size_t i = 0; i < size; ++i) {
            iv[i] = static_cast<unsigned char>(distribution(generator));
        }
    }
};

#endif //CAT_NET_CORE_H
