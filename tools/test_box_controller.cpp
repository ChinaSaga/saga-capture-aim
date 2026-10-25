// Local encrypted UDP peer: verifies routing without moving a physical mouse.
#include "../src/BoxController.cpp"
#include "../src/Input.cpp"
#include "../src/Makcu/cat/src/aes_functions.cpp"
#include "../src/Makcu/cat/src/cat_net_core.cpp"
#include "../src/Makcu/cat/src/catdevice.cpp"
#include <cstdio>
#include <stdexcept>
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ws2_32.lib")

AppState g;
namespace runtime_log { void write(const char*, ...) {} }
static int makcuMoves = 0, makcuDowns = 0, makcuUps = 0;
static bool makcuOnline = true;
static unsigned physicalButtons = 1u << 2;
static std::array<int, 7> buttonReads{};
static std::function<void()> duringButtonRead;
namespace saga {
int makcuMove(int, int) { ++makcuMoves; return 1; }
int makcuMouseDown(int) { ++makcuDowns; return 1; }
int makcuMouseUp(int) { ++makcuUps; return 1; }
int makcuMouseButtonState(int key) {
    ++buttonReads.at(key);
    const bool pressed = (physicalButtons & (1u << key)) != 0;
    if (duringButtonRead) duringButtonRead();
    return pressed;
}
int makcuIsConnected() { return makcuOnline; }
}
const char* makcuStartupStatus() { return "MAKCU"; }
void sleepMs(int) { g.running = false; } // Run exactly one production input scan.
void makcuState(int key, bool& state) { state = boxButtonState(key); }
void RunTimer::start() {}
double RunTimer::ms() const { return 0; }
void soundPlayCamp(int) {}
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F> void eventually(F condition, int ms, const char* message) {
    auto end = GetTickCount64() + ms;
    while (!condition()) { check(GetTickCount64() < end, message); Sleep(5); }
}

void expireButtonSamples() {
    for (auto& sample : buttonSamples) {
        std::lock_guard lock(sample.mutex);
        sample.sampledAt = std::chrono::steady_clock::now() - buttonSampleAge;
    }
}

void testInputSharing() {
    aim_profiles::Config config;
    for (auto& profile : config.profiles) profile.keys.fill(2);
    g.aimProfiles.replace(config);
    auto settings = g.settings.snapshot();
    for (int& key : settings.crossBind) key = 2;
    for (int& key : settings.triggerKey) key = 2;
    g.settings.replace(settings);
    g.rangePressKey = 1057;
    for (bool pressed : {true, false}) {
        physicalButtons = pressed ? 1u << 2 : 0;
        expireButtonSamples();
        buttonReads.fill(0);
        g.crossTrack = false;
        g.running = true;
        hotkeyThread();
        check(buttonReads[2] == 1, "all aim/crosshair/trigger slots query right once per scan, including key-up");
        check(g.aimKeyDown == pressed && g.aimPhysicalDown == pressed &&
            g.crossTrack == pressed && g.triggerDown == pressed && g.rangePressActive == pressed,
            "all consumers must use the same physical right-button result");
    }

    // A scan remains consistent even when the underlying state changes and the
    // shared 2 ms sample expires midway through evaluation.
    buttonReads.fill(0);
    expireButtonSamples();
    BoxInputScan heldScan;
    check(!heldScan.keyDown(2), "initial released sample");
    physicalButtons = 1u << 2;
    expireButtonSamples();
    for (int code = 1001; code <= 1127; ++code) check(!heldScan.keyDown(code), "MAKCU keyboard remains unavailable");
    for (int i = 0; i < 100; ++i) check(!heldScan.keyDown(2), "released result stays shared for the entire scan");
    check(buttonReads[2] == 1, "scan cache does not overflow after more than twenty keys");
    BoxInputScan nextScan;
    check(nextScan.keyDown(2) && buttonReads[2] == 2, "next scan refreshes expired physical state");
    for (int key : {-1, 0, 4, 7, 1000, 1128, 65535}) check(!nextScan.keyDown(key), "invalid bindings do not access devices");

    // Keep the sample fresh deterministically while another thread acts as the
    // web status/camp reader. Expiration is exercised separately above.
    { std::lock_guard lock(buttonSamples[1].mutex);
      buttonSamples[1].sampledAt = std::chrono::steady_clock::now() + std::chrono::seconds(1); }
    bool webPressed = false;
    std::thread status([&] { webPressed = boxButtonState(2); });
    status.join();
    check(webPressed && buttonReads[2] == 2, "other threads reuse the shared button result without device calls");
    makcuOnline = false;
    check(!boxButtonState(2), "disconnect immediately invalidates a fresh held sample");
    makcuOnline = true;
    physicalButtons = 0;
    check(!boxButtonState(2) && buttonReads[2] == 3, "reconnect cannot reuse a stale held sample");
    physicalButtons = 1u << 2;
    expireButtonSamples();
    BoxInputScan oldRoute;
    check(oldRoute.keyDown(2), "held key before switching route");
    switchRoute(nullptr);
    check(!oldRoute.keyDown(2), "route change invalidates the entire old scan");
    physicalButtons = 0;
    check(!boxButtonState(2), "route change bypasses even a fresh shared sample");
    physicalButtons = 1u << 2;
    expireButtonSamples();
    duringButtonRead = [] { switchRoute(nullptr); };
    check(!boxButtonState(2), "query finishing after a route change must not publish a held key");
    duringButtonRead = {};
    g.rangePressKey = 0;
    g.rangePressActive = false;
    expireButtonSamples();
}

class Peer {
    asio::io_context io;
    asio::ip::udp::socket socket{io, asio::ip::udp::endpoint(asio::ip::make_address("127.0.0.1"), 0)};
    std::thread thread;
    std::atomic<bool> stop{false};
    std::array<unsigned char, 176> expanded{};
    std::mutex mutex;
    std::vector<std::array<unsigned char, 16>> commands;
    std::atomic<unsigned short> monitorPort{0};
    std::atomic<int> report{-1};
    std::atomic<int> keyboardReport{-1};
    std::array<unsigned char, 32> encryptReport(int mask, int keyboardCode = 0, int mouseCode = 0x110) {
        // One 15-byte HidData record + PKCS#7 padding, prefixed by a zero IV.
        std::array<unsigned char, 32> frame{};
        unsigned char* out = frame.data() + 16;
        if (keyboardCode) {
            out[10] = static_cast<unsigned char>(keyboardCode);
            out[11] = static_cast<unsigned char>(keyboardCode >> 8);
            out[12] = static_cast<unsigned char>(mask);
        } else {
            out[0] = static_cast<unsigned char>(mouseCode);
            out[1] = static_cast<unsigned char>(mouseCode >> 8);
            out[2] = static_cast<unsigned char>(mask);
        }
        out[15] = 1;
        add_round_key(out, expanded.data());
        for (int r = 1; r < 10; ++r) aes_encrypt_round(out, expanded.data() + r * 16);
        sub_bytes(out); shift_rows(out); add_round_key(out, expanded.data() + 160);
        return frame;
    }
    void run() {
        while (!stop) {
            std::array<unsigned char, 1024> data{};
            asio::ip::udp::endpoint source;
            asio::error_code ec;
            const auto size = socket.receive_from(asio::buffer(data), source, 0, ec);
            if (!ec && size == 32) {
                std::array<unsigned char, 16> plain{};
                std::copy_n(data.data() + 16, 16, plain.data());
                add_round_key(plain.data(), expanded.data() + 160);
                inv_shift_rows(plain.data()); inv_sub_bytes(plain.data());
                for (int r = 9; r > 0; --r) aes_decrypt_round(plain.data(), expanded.data() + r * 16);
                add_round_key(plain.data(), expanded.data());
                for (int i = 0; i < 16; ++i) plain[i] ^= data[i];
                bool padding = true;
                for (int i = 7; i < 16; ++i) padding = padding && plain[i] == 9;
                if (!padding) continue; // Wrong UUID must not receive an ACK.
                { std::lock_guard<std::mutex> lock(mutex); commands.push_back(plain); }
                if (plain[0] == 2) monitorPort = static_cast<unsigned short>(plain[1] | (plain[2] << 8));
                if (!drop && !(dropMonitor && plain[0] == 2)) socket.send_to(asio::buffer(data.data(), size), source, 0, ec);
            }
            const int state = report.exchange(-1);
            if (state >= 0 && monitorPort) {
                auto frame = encryptReport(state & 1, 0, 0x110 + (state >> 1));
                socket.send_to(asio::buffer(frame), {asio::ip::make_address("127.0.0.1"), monitorPort.load()}, 0, ec);
            }
            const int key = keyboardReport.exchange(-1);
            if (key >= 0 && monitorPort) {
                auto frame = encryptReport((key >> 16) & 1, key & 0xffff);
                socket.send_to(asio::buffer(frame), {asio::ip::make_address("127.0.0.1"), monitorPort.load()}, 0, ec);
            }
            Sleep(1);
        }
    }
public:
    std::atomic<bool> drop{false}, dropMonitor{false};
    Peer() {
        const unsigned char key[] = "1234abcd00000000";
        key_expansion(key, expanded.data());
        socket.non_blocking(true);
        thread = std::thread([this] { run(); });
    }
    ~Peer() { stop = true; thread.join(); }
    int port() { return socket.local_endpoint().port(); }
    void button(bool down, int key = 1) { report = (buttonIndex(key) << 1) | (down ? 1 : 0); }
    void keyboard(int code, bool down) { keyboardReport = code | (down ? 0x10000 : 0); }
    auto snapshot() { std::lock_guard<std::mutex> lock(mutex); return commands; }
};

int main() {
    try {
        testInputSharing();
        check(validCatSettings({"192.168.1.2", "65535", "12ABcdef", true}), "valid settings");
        for (const auto& ip : {"", "1.2.3", "1.2.3.256", "1.2.3.4.5", "0.0.0.0", "224.0.0.1"})
            check(!validCatSettings({ip, "1234", "1234abcd", true}), "reject malformed IP");
        for (const auto& port : {"", "0", "-1", "65536", "12a"})
            check(!validCatSettings({"127.0.0.1", port, "1234abcd", true}), "reject malformed port");
        for (const auto& uuid : {"", "1234567", "123456789", "1234567z"})
            check(!validCatSettings({"127.0.0.1", "1234", uuid, true}), "reject malformed UUID");

        Peer peer;
        {
            cat::Device wrongKey;
            check(wrongKey.connect("127.0.0.1", peer.port(), "ffffffff", 40) != cat::ErrorCode::SUCCESS, "wrong UUID cannot connect");
        }
        boxMove(2, 3); boxDown(1); boxUp(1);
        check(boxLastMoveNeedsDelay(), "MAKCU retains movement pacing");
        check(makcuMoves == 1 && makcuDowns == 1 && makcuUps == 1 && boxButtonState(2), "MAKCU default route");
        check(!boxKeyState(1030) && !boxKeyState(1001), "MAKCU never substitutes local keyboard state");
        auto session = std::make_shared<CatSession>();
        check(session->device.connect("127.0.0.1", peer.port(), "1234abcd", 100) == cat::ErrorCode::SUCCESS, "CAT handshake");
        check(session->device.startMonitor(0, 100) == cat::ErrorCode::SUCCESS, "ephemeral monitor port handshake");
        boxDown(1);
        switchRoute(session);
        check(makcuUps == 2, "release old MAKCU press on switch");
        boxMove(-321, 123);
        check(!boxLastMoveNeedsDelay(), "CAT skips additional movement pacing");
        for (int key : keys) { boxDown(key); boxUp(key); }
        check(makcuMoves == 1 && makcuDowns == 2, "CAT commands do not leak to MAKCU");
        const auto commands = peer.snapshot();
        bool move = false;
        unsigned downMask = 0, upMask = 0;
        for (auto c : commands) {
            const auto a = static_cast<int16_t>(c[3] | (c[4] << 8));
            const auto b = static_cast<int16_t>(c[5] | (c[6] << 8));
            if (c[0] == 8 && a == -321 && b == 123) move = true;
            if (c[0] == 3 && c[1] >= 0x10 && c[1] <= 0x14 && c[2] == 1) {
                if (a == 1) downMask |= 1u << (c[1] - 0x10);
                if (a == 0) upMask |= 1u << (c[1] - 0x10);
            }
        }
        check(move && downMask == 31 && upMask == 31, "encrypted displacement and all five button mappings");
        const auto count = peer.snapshot().size();
        boxMove(32768, 0); boxDown(4); boxUp(4);
        check(peer.snapshot().size() == count, "invalid inputs never sent or narrowed");
        peer.button(true);
        eventually([] { return boxButtonState(1); }, 500, "CAT physical down monitor");
        peer.button(false);
        eventually([] { return !boxButtonState(1); }, 500, "CAT physical up monitor");
        peer.button(true, 2);
        eventually([] { return boxButtonState(2); }, 500, "CAT right down monitor");
        g.crossTrack = false;
        g.running = true;
        hotkeyThread();
        check(g.aimKeyDown && g.aimPhysicalDown && g.crossTrack && g.triggerDown,
            "CAT right report drives aim, crosshair and trigger together");
        peer.button(false, 2);
        eventually([] { return !boxButtonState(2); }, 500, "CAT right up monitor");
        g.running = true;
        hotkeyThread();
        check(!g.aimKeyDown && !g.aimPhysicalDown && !g.triggerDown,
            "CAT right release reaches aim and trigger on the next scan");
        for (int code : {1, 30, 42, 54, 97, 59, 96, 103, 127}) {
            peer.keyboard(code, true);
            eventually([code] { return boxKeyState(1000 + code); }, 500, "CAT keyboard down");
            check(!boxKeyState(1), "keyboard code never aliases mouse left");
            peer.keyboard(code, false);
            eventually([code] { return !boxKeyState(1000 + code); }, 500, "CAT keyboard up");
        }
        check(!boxKeyState(1000) && !boxKeyState(1128) && !boxKeyState(-1), "invalid keyboard bindings rejected");
        check(peer.snapshot().size() == count, "all CAT input reads consume local reports without sending query commands");
        g.running = true;
        g.rangePressActive = true;
        g.rangePressKey = 1057;
        keyboardTick(GetTickCount64(), 50, 75);
        check(repeatHeld && peer.snapshot().back()[0] == 4 && peer.snapshot().back()[1] == 57 && peer.snapshot().back()[3] == 1, "CAT space down");
        const auto heldCount = peer.snapshot().size();
        keyboardTick(repeatDue - 1, 50, 75);
        check(peer.snapshot().size() == heldCount, "no transition before deadline");
        keyboardTick(repeatDue, 50, 75);
        check(!repeatHeld && peer.snapshot().back()[3] == 0, "CAT space up");
        keyboardTick(repeatDue, 50, 75);
        check(repeatHeld, "repeat after release gap");
        g.rangePressKey = 1030;
        keyboardTick(GetTickCount64(), 50, 75);
        auto changed = peer.snapshot();
        check(changed[changed.size()-2][1] == 57 && changed[changed.size()-2][3] == 0 && changed.back()[1] == 30 && changed.back()[3] == 1, "changing key releases old key first");
        g.rangePressActive = false;
        keyboardTick(GetTickCount64(), 50, 75);
        check(!repeatHeld && peer.snapshot().back()[3] == 0, "aim release stops immediately without waiting for deadline");
        g.rangePressActive = true;
        g.rangePressKey = 1;
        auto invalidCount = peer.snapshot().size();
        keyboardTick(GetTickCount64(), 50, 75);
        check(peer.snapshot().size() == invalidCount, "mouse binding cannot produce keyboard output");
        g.rangePressKey = 1057;
        keyboardTick(GetTickCount64(), 50, 75);
        peer.drop = true;
        g.rangePressKey = 0;
        keyboardTick(GetTickCount64(), 50, 75);
        check(pendingKeyboardReleases.test(57) && !session->online, "failed release retained for reconnect");
        peer.drop = false;
        switchRoute(nullptr);
        session->online = true;
        switchRoute(session);
        check(pendingKeyboardReleases.none() && peer.snapshot().back()[0] == 4 && peer.snapshot().back()[3] == 0, "reconnect releases keyboard before use");
        g.rangePressKey = 1057;
        keyboardTick(GetTickCount64(), 50, 75);
        boxStopKeyboard();
        check(!repeatHeld && peer.snapshot().back()[3] == 0, "application exit releases held keyboard");
        auto stoppedCount = peer.snapshot().size();
        keyboardTick(GetTickCount64(), 50, 75);
        check(peer.snapshot().size() == stoppedCount, "exit prevents new presses");
        g.rangePressActive = false;
        g.rangePressKey = 0;
        peer.keyboard(30, true);
        eventually([] { return boxKeyState(1030); }, 500, "held keyboard before route change");
        boxDown(3);
        switchRoute(nullptr);
        check(!boxKeyState(1030), "held CAT keyboard state cleared on MAKCU fallback");
        check(peer.snapshot().back()[0] == 3 && peer.snapshot().back()[1] == 0x12 && peer.snapshot().back()[3] == 0, "release old CAT press on switch");
        session->device.stopMonitor();
        peer.dropMonitor = true;
        check(session->device.startMonitor(0, 40) != cat::ErrorCode::SUCCESS, "monitor timeout");
        peer.dropMonitor = false;
        check(session->device.startMonitor(0, 100) == cat::ErrorCode::SUCCESS, "monitor restart after failure");
        session->device.disconnect();
        check(session->device.connect("127.0.0.1", peer.port(), "1234abcd", 100) == cat::ErrorCode::SUCCESS, "reconnect socket lifecycle");
        session.reset();

        // Exercise the production background worker, including timeout/fallback
        // and a settings change that disables further CAT connections.
        { std::lock_guard<std::mutex> lock(settingsMutex);
          settings = {"127.0.0.1", std::to_string(peer.port()), "1234abcd", true}; ++settingsVersion; }
        g.running = true;
        std::thread worker(catWorker);
        try {
            eventually([] { return activeCat.load() != nullptr; }, 1500, "automatic CAT connection");
            peer.drop = true;
            boxDown(1);
            boxMove(7, 9);
            auto failed = activeCat.load();
            check(failed && !failed->online, "failed command marks offline");
            check(!boxButtonState(1), "offline state cannot stay pressed");
            eventually([] { return !activeCat.load(); }, 1000, "automatic MAKCU fallback");
            const int before = makcuMoves;
            boxMove(3, 4);
            check(boxLastMoveNeedsDelay(), "MAKCU fallback restores movement pacing");
            check(makcuMoves == before + 1, "fallback routes next move");
            peer.drop = false;
            eventually([] { return activeCat.load() != nullptr; }, 4500, "automatic CAT recovery");
            { std::lock_guard<std::mutex> lock(routeMutex); check(pendingCatReleases == 0, "lost release retried before publishing recovered CAT"); }
            { std::lock_guard<std::mutex> lock(settingsMutex); settings.enabled = false; ++settingsVersion; }
            eventually([] { return !activeCat.load(); }, 1000, "disable CAT reverts route");
        } catch (...) { g.running = false; worker.join(); throw; }
        g.running = false; worker.join();
        std::puts("PASS: shared hotkey scans, cross-thread cache, release/route invalidation, settings, encrypted UDP, five buttons, input monitor, timeout, fallback, recovery and disable");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1;
    }
}
