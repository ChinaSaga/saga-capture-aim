#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

namespace aim_profiles {
struct Profile {
    std::array<int, 4> keys{};
    int prioLock = 1, secondLock = 2, thirdLock = 3;
    int prioLockPos = 50, secondLockPos = 50, thirdLockPos = 50;
    int lockRange = 256, centerRange = 50;
    bool moveInBox = false;
    bool operator==(const Profile&) const = default;
};
struct Config {
    std::array<Profile, 3> profiles{};
    bool alwaysOn = false;
    bool operator==(const Config&) const = default;
};

// Each group is an OR of four keys. The first matching group wins.
template<class ReadKey>
int select(const Config& config, ReadKey readKey) {
    if (config.alwaysOn) return 0;
    for (int group = 0; group < 3; ++group)
        for (int key : config.profiles[group].keys)
            if (key > 0 && readKey(key)) return group;
    return -1;
}

// Publish complete configurations and selections, never partially updated fields.
class Store {
public:
    struct ConfigSnapshot { Config config; std::uint64_t revision; };
    struct ActiveSnapshot { Profile profile; int index; std::uint64_t token; };
    ConfigSnapshot config() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return {config_, revision_};
    }
    void replace(const Config& config) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (config_ == config) return;
        config_ = config;
        ++revision_;
        active_ = -1;
        ++token_;
    }
    bool activate(int index, std::uint64_t revision) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (revision != revision_) return false;
        if (active_ != index) { active_ = index; ++token_; }
        return active_ >= 0;
    }
    ActiveSnapshot active() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return {config_.profiles[active_ >= 0 ? active_ : 0], active_, token_.load()};
    }
    std::uint64_t token() const { return token_.load(); }
private:
    mutable std::mutex mutex_;
    Config config_;
    std::uint64_t revision_ = 0;
    int active_ = -1;
    std::atomic<std::uint64_t> token_{1};
};
}
