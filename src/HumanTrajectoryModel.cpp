#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "HumanTrajectoryModel.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <numeric>
#include <random>
#include <sstream>
#include <string>

namespace humanTrajectory {
namespace {
struct Activations {
    std::array<float, 64> first{};
    std::array<float, 32> second{};
    std::array<float, 20> output{};
};

Activations forward(const Parameters& w, const std::array<float, 2>& input)
{
    Activations a;
    for (size_t i = 0; i < 64; ++i)
        a.first[i] = std::max(0.0f, w[B1 + i] + w[W1 + i * 2] * input[0] + w[W1 + i * 2 + 1] * input[1]);
    for (size_t i = 0; i < 32; ++i) {
        float value = w[B2 + i];
        for (size_t j = 0; j < 64; ++j) value += w[W2 + i * 64 + j] * a.first[j];
        a.second[i] = std::max(0.0f, value);
    }
    for (size_t i = 0; i < 20; ++i) {
        float value = w[B3 + i];
        for (size_t j = 0; j < 32; ++j) value += w[W3 + i * 32 + j] * a.second[j];
        a.output[i] = value;
    }
    return a;
}

void validate(const Parameters& values)
{
    for (float value : values)
        if (!std::isfinite(value)) throw std::runtime_error("训练出现无效数值，已有模型保持不变。");
}

Sample parseRow(const std::string& line)
{
    Sample sample;
    std::istringstream stream(line);
    for (int i = 0; i < 11; ++i) {
        stream >> std::ws;
        if (stream.get() != '"') throw std::runtime_error("坐标应使用 CSV 引号包裹，例如 \"12,-5\"。");
        std::string coordinate;
        if (!std::getline(stream, coordinate, '"') || stream.eof()) throw std::runtime_error("坐标缺少结束引号。");
        std::istringstream pair(coordinate);
        int x, y;
        char comma;
        if (!(pair >> x >> comma >> y) || comma != ',') throw std::runtime_error("坐标必须是两个整数。");
        pair >> std::ws;
        if (!pair.eof()) throw std::runtime_error("坐标中包含多余内容。");
        if (i == 0) sample.input = {float(x), float(y)};
        else {
            sample.target[(i - 1) * 2] = float(x);
            sample.target[(i - 1) * 2 + 1] = float(y);
        }
        stream >> std::ws;
        if (i < 10 && stream.get() != ',') throw std::runtime_error("每行必须有目标位移和 10 个轨迹点，共 11 列。");
    }
    if (!stream.eof()) throw std::runtime_error("每行只能有 11 列。");
    return sample;
}
} // namespace

Network::Network(uint32_t seed)
{
    std::mt19937 random(seed);
    // Same distribution as torch.nn.Linear's default initialization.
    for (auto layer : {std::array<size_t, 3>{W1, W2, 2}, {W2, W3, 64}, {W3, ParameterCount, 32}}) {
        const float bound = 1.0f / std::sqrt(float(layer[2]));
        std::uniform_real_distribution<float> distribution(-bound, bound);
        for (size_t i = layer[0]; i < layer[1]; ++i) weights[i] = distribution(random);
    }
}

std::array<float, 20> Network::predict(const std::array<float, 2>& input) const
{
    return forward(weights, input).output;
}

bool appendRecording(const std::filesystem::path& csv, const std::vector<Point>& path)
{
    if (path.size() < 10) return false;
    const size_t step = std::max(size_t(1), path.size() / 10);
    std::ostringstream rows;
    for (int sign : {1, -1}) {
        const auto writePoint = [&](size_t index) {
            rows << '"' << (int64_t(path[index].x) - path[0].x) * sign << ','
                 << int64_t(path[0].y) - path[index].y << '"';
        };
        writePoint(9 * step);
        for (size_t i = 0; i < 10; ++i) { rows << ','; writePoint(i * step); }
        rows << '\n';
    }
    std::ofstream output(csv, std::ios::app | std::ios::binary);
    output << rows.str();
    output.close();
    if (!output) throw std::runtime_error("无法追加人手数据，请检查文件权限和磁盘空间。");
    return true;
}

std::vector<Sample> loadSamples(const std::filesystem::path& csv)
{
    std::ifstream input(csv, std::ios::binary);
    if (!input) throw std::runtime_error("无法读取 人手数据.txt，请先记录人手数据。");
    std::vector<Sample> samples;
    std::string line;
    size_t number = 0;
    while (std::getline(input, line)) {
        ++number;
        if (number == 1 && line.compare(0, 3, "\xef\xbb\xbf") == 0) line.erase(0, 3);
        if (line.find_first_not_of(" \t\r\n") == std::string::npos) continue;
        try { samples.push_back(parseRow(line)); }
        catch (const std::exception& error) {
            throw std::runtime_error("CSV 第 " + std::to_string(number) + " 行：" + error.what());
        }
    }
    if (!input.eof()) throw std::runtime_error("读取采集数据时发生错误。");
    if (samples.empty()) throw std::runtime_error("还没有有效数据，请先点击红球、蓝球完成记录。");
    return samples;
}

double accumulateGradient(const Network& network, const Sample& sample, Parameters& gradient)
{
    const auto& w = network.weights;
    const auto a = forward(w, sample.input);
    std::array<float, 32> secondDelta{};
    std::array<float, 64> firstDelta{};
    double loss = 0;
    for (size_t i = 0; i < 20; ++i) {
        const float difference = a.output[i] - sample.target[i];
        loss += double(difference) * difference / 20.0;
        const float delta = difference * (2.0f / 20.0f);
        gradient[B3 + i] += delta;
        for (size_t j = 0; j < 32; ++j) {
            gradient[W3 + i * 32 + j] += delta * a.second[j];
            secondDelta[j] += delta * w[W3 + i * 32 + j];
        }
    }
    for (size_t i = 0; i < 32; ++i) {
        if (a.second[i] <= 0) continue;
        const float delta = secondDelta[i];
        gradient[B2 + i] += delta;
        for (size_t j = 0; j < 64; ++j) {
            gradient[W2 + i * 64 + j] += delta * a.first[j];
            firstDelta[j] += delta * w[W2 + i * 64 + j];
        }
    }
    for (size_t i = 0; i < 64; ++i) {
        if (a.first[i] <= 0) continue;
        const float delta = firstDelta[i];
        gradient[B1 + i] += delta;
        gradient[W1 + i * 2] += delta * sample.input[0];
        gradient[W1 + i * 2 + 1] += delta * sample.input[1];
    }
    return loss;
}

double meanLoss(const Network& network, const std::vector<Sample>& samples)
{
    if (samples.empty()) throw std::runtime_error("训练数据为空。");
    double loss = 0;
    for (const auto& sample : samples) {
        auto prediction = network.predict(sample.input);
        for (size_t i = 0; i < 20; ++i) {
            const double difference = double(prediction[i]) - sample.target[i];
            loss += difference * difference / 20.0;
        }
    }
    return loss / samples.size();
}

Network train(const std::vector<Sample>& samples, const TrainOptions& options,
    const std::function<void(const Progress&)>& progress, const std::function<bool()>& cancelled)
{
    if (samples.empty() || options.epochs < 1 || !options.batchSize ||
        !std::isfinite(options.learningRate) || options.learningRate <= 0)
        throw std::runtime_error("训练数据或训练参数无效。");
    Network network(options.seed);
    Parameters firstMoment{}, secondMoment{}, gradient{};
    std::vector<size_t> order(samples.size());
    std::iota(order.begin(), order.end(), 0);
    std::mt19937 random(options.seed ^ 0x51a7c0de);
    double beta1Power = 1, beta2Power = 1;
    float learningRate = options.learningRate;
    size_t step = 0;
    for (int epoch = 1; epoch <= options.epochs; ++epoch) {
        std::shuffle(order.begin(), order.end(), random);
        double epochLoss = 0;
        for (size_t begin = 0; begin < order.size(); begin += options.batchSize) {
            if (cancelled && cancelled()) throw TrainingCancelled();
            const size_t end = std::min(begin + options.batchSize, order.size());
            gradient.fill(0);
            for (size_t i = begin; i < end; ++i)
                epochLoss += accumulateGradient(network, samples[order[i]], gradient);
            validate(gradient);
            beta1Power *= 0.9;
            beta2Power *= 0.999;
            const double scale = learningRate / (1.0 - beta1Power);
            for (size_t i = 0; i < ParameterCount; ++i) {
                const float g = gradient[i] / float(end - begin);
                firstMoment[i] = 0.9f * firstMoment[i] + 0.1f * g;
                secondMoment[i] = 0.999f * secondMoment[i] + 0.001f * g * g;
                network.weights[i] -= float(scale * firstMoment[i] /
                    (std::sqrt(secondMoment[i] / (1.0 - beta2Power)) + 1e-8));
            }
            validate(network.weights);
            validate(secondMoment);
            // Preserve train.py's StepLR: 0.9 decay every 50 optimizer steps.
            if (++step % 50 == 0) learningRate *= 0.9f;
        }
        if (!std::isfinite(epochLoss)) throw std::runtime_error("训练损失出现无效数值。");
        if (progress) progress({epoch, options.epochs, epochLoss / samples.size(), learningRate});
    }
    if (cancelled && cancelled()) throw TrainingCancelled();
    return network;
}

void saveModel(const Network& network, const std::filesystem::path& destination)
{
    static_assert(sizeof(float) == 4 && ParameterCount * sizeof(float) == 11728);
    validate(network.weights);
    const std::filesystem::path temporary = destination.wstring() + L".tmp";
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(network.weights.data()), sizeof(network.weights));
        output.close();
        if (!output) throw std::runtime_error("模型写入失败，已有模型保持不变。");
        if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("模型替换失败，请检查文件是否只读或被占用；已有模型保持不变。");
    } catch (...) {
        std::error_code error;
        std::filesystem::remove(temporary, error);
        throw;
    }
}
} // namespace humanTrajectory
