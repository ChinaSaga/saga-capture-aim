#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <vector>

namespace humanTrajectory {
struct Point { int x, y; };
struct Sample {
    std::array<float, 2> input{};
    std::array<float, 20> target{};
};

// Exactly the row-major float32 layout consumed by Mouse.cpp::mc_create.
constexpr size_t W1 = 0, B1 = 128, W2 = 192, B2 = 2240, W3 = 2272, B3 = 2912;
constexpr size_t ParameterCount = 2932;
using Parameters = std::array<float, ParameterCount>;

struct Network {
    Parameters weights{};
    explicit Network(uint32_t seed);
    std::array<float, 20> predict(const std::array<float, 2>& input) const;
};

// Retains the Python collector's ten-point sampling, inverted Y and X mirror.
bool appendRecording(const std::filesystem::path& csv, const std::vector<Point>& path);
std::vector<Sample> loadSamples(const std::filesystem::path& csv);
double accumulateGradient(const Network& network, const Sample& sample, Parameters& gradient);
double meanLoss(const Network& network, const std::vector<Sample>& samples);

struct TrainOptions {
    int epochs = 500;
    size_t batchSize = 64;
    float learningRate = 0.001f;
    uint32_t seed = 0;
};
struct Progress {
    int epoch, epochs;
    double loss;
    float learningRate;
};
struct TrainingCancelled : std::runtime_error {
    TrainingCancelled() : std::runtime_error("Training cancelled") {}
};
Network train(const std::vector<Sample>& samples, const TrainOptions& options,
    const std::function<void(const Progress&)>& progress = {},
    const std::function<bool()>& cancelled = {});
void saveModel(const Network& network, const std::filesystem::path& destination);
} // namespace humanTrajectory
