#include "../src/AimProfiles.h"
#include <iostream>
#include <stdexcept>

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
int main() {
    try {
        aim_profiles::Config config;
        // Every slot participates, including simultaneous presses and release fallback.
        for (int slot = 0; slot < 4; ++slot) {
            config = {};
            for (int group = 0; group < 3; ++group) config.profiles[group].keys[slot] = group + 1;
            for (int mask = 0; mask < 8; ++mask) {
                const int expected = mask & 1 ? 0 : mask & 2 ? 1 : mask & 4 ? 2 : -1;
                require(aim_profiles::select(config, [mask](int key) { return (mask & (1 << (key - 1))) != 0; }) == expected,
                    "Simultaneous keys must select the lowest group number");
            }
        }
        for (auto& profile : config.profiles) profile.keys = {5, 5, 5, 5};
        require(aim_profiles::select(config, [](int key) { return key == 5; }) == 0, "Duplicate bindings must prefer group 1");
        config.profiles[0].keys = {};
        require(aim_profiles::select(config, [](int key) { return key == 5; }) == 1, "Group 2 must beat group 3");
        config.alwaysOn = true;
        require(aim_profiles::select(config, [](int) { throw std::runtime_error("Always-on must not query keys"); return false; }) == 0,
            "Always-on belongs to group 1");
        config.alwaysOn = false;
        for (auto& profile : config.profiles) profile.keys = {};
        require(aim_profiles::select(config, [](int) { return true; }) == -1, "Blank bindings must not trigger");

        aim_profiles::Store store;
        config.profiles[1].prioLock = 7;
        store.replace(config);
        const auto saved = store.config();
        require(store.activate(1, saved.revision), "Group 2 activation failed");
        const auto second = store.active();
        require(second.index == 1 && second.profile.prioLock == 7, "Active target settings differ from selected group");
        store.replace(config);
        require(store.token() == second.token, "Unrelated settings must not reset the active lock");
        store.activate(2, saved.revision);
        require(store.token() != second.token, "Profile switch must invalidate pending movement and target memory");
        config.profiles[2].prioLock = 8;
        store.replace(config);
        require(!store.activate(1, saved.revision), "Stale key scans must not publish after configuration changes");
        require(store.active().index == -1, "Changed configuration waits for a fresh key scan");
        const auto fresh = store.config();
        require(store.activate(2, fresh.revision) && store.active().profile.prioLock == 8, "Fresh configuration did not activate");
        require(!store.activate(-1, fresh.revision), "All released must deactivate");
        std::cout << "PASS all key slots, simultaneous keys, duplicates, always-on, release and configuration generations\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
