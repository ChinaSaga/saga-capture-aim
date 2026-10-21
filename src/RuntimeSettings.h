#pragma once
#include <mutex>

// HTTP publishes a complete value; workers take one small snapshot per cycle.
// No strings, allocations, or locks are needed while processing detections.
struct RuntimeSettings {
    float conf = .55f, nms = .25f, crossConf = .20f;
    int crossClass = 5;
    int moveMode = 0, moveDelay = 5;
    float moveFactor = .618f, kp = 0, ki = 0;
    int lockFreq = 100, delayFreq = 200;
    int crossBind[4]{}, triggerKey[4]{};
    int shotInterval = 80, adsTime = 350;
    float triggerRange = .66f;
    bool threadMove = false, aimContinuous = true, autoShot = false;
    bool crossTrigger = false, centerTrigger = false;
    bool type[10]{}, team1[10]{}, team2[10]{};
};

class RuntimeSettingsStore {
public:
    RuntimeSettings snapshot() const {
        std::lock_guard lock(mutex_);
        return value_;
    }
    void replace(const RuntimeSettings& value) {
        std::lock_guard lock(mutex_);
        value_ = value;
    }
private:
    mutable std::mutex mutex_;
    RuntimeSettings value_;
};
