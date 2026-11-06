// Build with HumanTrajectoryModel.cpp and Mouse.cpp, /arch:AVX2 /fp:precise.
// Synthetic data only. Each run uses a new directory under the system temp path.
#include "../src/HumanTrajectory.cpp"
#include "../src/Inference.h"
#include <cassert>
#include <limits>
#include <thread>

static std::function<void()> timerAction;
static void CALLBACK onTimer(HWND, UINT, UINT_PTR timer, DWORD)
{
    KillTimer(nullptr, timer);
    timerAction();
}

static void writeValues(const fs::path& path, const ht::Parameters& values)
{
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(values.data()), sizeof(values));
    assert(output.good());
}

static ht::Sample referenceSample()
{
    ht::Sample sample;
    sample.input = {0.75f, -0.35f};
    for (size_t i = 0; i < 20; ++i) sample.target[i] = (int(i) - 10) * 0.125f;
    return sample;
}

static void testGradientAndAdam(const fs::path& directory)
{
    ht::Network initial(42);
    ht::Parameters gradient{};
    const auto sample = referenceSample();
    ht::accumulateGradient(initial, sample, gradient);
    size_t checked = 0;
    for (size_t i = 0; i < ht::ParameterCount; i += 17) {
        ht::Network positive = initial, negative = initial;
        positive.weights[i] += 0.001f;
        negative.weights[i] -= 0.001f;
        double difference = (ht::meanLoss(positive, {sample}) - ht::meanLoss(negative, {sample})) / 0.002;
        assert(std::abs(difference - gradient[i]) < 0.001);
        ++checked;
    }
    ht::TrainOptions options;
    options.seed = 42;
    options.epochs = 1;
    const auto updated = ht::train({sample}, options);
    for (size_t i = 0; i < ht::ParameterCount; ++i) {
        const double expected = initial.weights[i] - 0.001 * gradient[i] / (std::abs(gradient[i]) + 1e-8);
        assert(std::abs(expected - updated.weights[i]) < 2e-7);
    }
    writeValues(directory / L"initial.bin", initial.weights);
    writeValues(directory / L"gradient.bin", gradient);
    writeValues(directory / L"adam.bin", updated.weights);
    printf("PASS: %zu finite-difference gradients and all 2932 Adam first-step parameters.\n", checked);
}

static void testCsvAndModel(const fs::path& directory)
{
    const auto csv = directory / L"人手数据.txt";
    assert(!ht::appendRecording(csv, std::vector<ht::Point>(9)));
    assert(!fs::exists(csv));
    std::vector<ht::Point> points;
    for (int i = 0; i < 25; ++i) points.push_back({100 + 3 * i, 100 + i});
    assert(ht::appendRecording(csv, points));
    assert(ht::appendRecording(csv, points));
    auto samples = ht::loadSamples(csv);
    assert(samples.size() == 4);
    assert(samples[0].input[0] == 54 && samples[0].input[1] == -18);
    assert(samples[1].input[0] == -54 && samples[1].input[1] == -18);
    assert(samples[0].target[0] == 0 && samples[0].target[18] == 54);

    const auto malformed = directory / L"bad.csv";
    for (const std::string bad : {std::string(), std::string("bad,data\n"),
        std::string("\"1,2\",\"1,2\",\"1,2\",\"1,2\",\"1,2\",\"1,2\",\"1,2\",\"1,2\",\"1,2\",\"1,2\",\"1,2")}) {
        { std::ofstream output(malformed); output << bad; }
        bool rejected = false;
        try { ht::loadSamples(malformed); } catch (const std::runtime_error&) { rejected = true; }
        assert(rejected);
    }

    ht::TrainOptions options;
    options.seed = 42;
    const double before = ht::meanLoss(ht::Network(options.seed), samples);
    int epochs = 0;
    auto trained = ht::train(samples, options, [&](const ht::Progress& progress) {
        assert(progress.epoch == ++epochs);
        assert(std::isfinite(progress.loss));
    });
    const double after = ht::meanLoss(trained, samples);
    assert(epochs == 500 && after < before * 0.01);
    const auto model = directory / L"mouse.bin";
    ht::saveModel(trained, model);
    assert(fs::file_size(model) == 11728);
    // Decode the exported bytes with the real AVX2 inference implementation.
    assert(saga::mc_create(model.string().c_str()));
    for (const auto& sample : samples) {
        float actual[20]{};
        saga::mc_calc(sample.input[0], sample.input[1], actual);
        const auto expected = trained.predict(sample.input);
        for (size_t i = 0; i < 20; ++i)
            assert(std::abs(actual[i] - expected[i]) < 0.001f);
    }
    bool cancelled = false;
    try { ht::train(samples, options, {}, [] { return true; }); }
    catch (const ht::TrainingCancelled&) { cancelled = true; }
    assert(cancelled);
    const auto oldSize = fs::file_size(model);
    trained.weights[0] = std::numeric_limits<float>::quiet_NaN();
    bool rejected = false;
    try { ht::saveModel(trained, model); } catch (const std::runtime_error&) { rejected = true; }
    assert(rejected && fs::file_size(model) == oldSize);
    assert(!fs::exists(model.wstring() + L".tmp"));
    printf("PASS: CSV append/mirror/validation, 500 CPU epochs (loss %.3f -> %.6f), AVX2 model compatibility, cancellation.\n", before, after);
}

static void testModelReplacement(const fs::path& directory)
{
    const auto first = directory / L"reload-first.bin";
    const auto second = directory / L"reload-second.bin";
    ht::Parameters weights{};
    for (size_t i = ht::B3; i < weights.size(); ++i) weights[i] = 1;
    writeValues(first, weights);
    for (size_t i = ht::B3; i < weights.size(); ++i) weights[i] = 2;
    writeValues(second, weights);
    assert(saga::mc_create(first.string().c_str()));
    std::atomic<bool> stop{false};
    std::atomic<size_t> predictions{0};
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) readers.emplace_back([&] {
        while (!stop.load()) {
            float values[20]{};
            saga::mc_calc(10, 20, values);
            assert(values[0] == 1 || values[0] == 2);
            for (float value : values) assert(value == values[0]);
            ++predictions;
        }
    });
    for (int i = 0; i < 200; ++i)
        assert(saga::mc_create((i % 2 ? first : second).string().c_str()));
    stop = true;
    for (auto& reader : readers) reader.join();
    assert(predictions > 0);
    const auto invalid = directory / L"reload-invalid.bin";
    { std::ofstream output(invalid); output << "incomplete"; }
    assert(!saga::mc_create(invalid.string().c_str()));
    weights[0] = std::numeric_limits<float>::quiet_NaN();
    writeValues(invalid, weights);
    assert(!saga::mc_create(invalid.string().c_str()));
    float values[20]{};
    saga::mc_calc(10, 20, values);
    for (float value : values) assert(value == 1);
    puts("PASS: concurrent inference sees complete old/new weights; invalid replacement preserves the working model.");
}

static void testNativeCollector(const fs::path& directory)
{
    fs::create_directories(directory);
    for (size_t previous = 0; previous < 2; ++previous) {
        Collector collector(directory);
        assert(collector.csv.filename() == L"人手数据.txt");
        assert(collector.count == previous);
        timerAction = [&] {
            assert(collector.width > 500 && collector.height > 300);
            auto click = [&](ht::Point point) {
                SendMessageW(collector.window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(point.x, point.y));
            };
            // First reject a short path and then hit the newly spawned blue ball.
            click(collector.red);
            click(collector.blue);
            assert(collector.count == previous && !collector.recording);
            click(collector.red);
            const ht::Point red = collector.red, blue = collector.blue;
            for (int i = 1; i <= 24; ++i)
                SendMessageW(collector.window, WM_MOUSEMOVE, 0,
                    MAKELPARAM(red.x + (blue.x - red.x) * i / 24, red.y + (blue.y - red.y) * i / 24));
            click(blue);
            assert(collector.count == previous + 1 && !collector.recording);
            assert(ht::loadSamples(collector.csv).size() == (previous + 1) * 2);
            SendMessageW(collector.window, WM_KEYDOWN, VK_ESCAPE, 0);
        };
        assert(SetTimer(nullptr, 0, 200, onTimer));
        assert(collector.run(GetModuleHandleW(nullptr)) == 0);
        // Blank lines from viewing/editing the text must not inflate the total.
        std::ofstream blank(collector.csv, std::ios::app);
        blank << "\n \t\n";
    }
    assert(Collector(directory).count == 2);
    puts("PASS: native collector reopens with cumulative count; one saved trajectory counts once; short paths and blank lines do not count.");
}

static void testButtonLauncher(const fs::path& source)
{
    const auto directory = executablePath().parent_path() / L"人手数据";
    // The test executable must be compiled in a fresh scratch directory.
    assert(!fs::exists(directory));
    fs::create_directory(directory);
    // An old recording must pass the button's preflight and migrate in the child.
    fs::copy_file(source / L"人手数据.txt", directory / L"mouse_data.csv");
    wchar_t system[MAX_PATH]{};
    assert(GetSystemDirectoryW(system, _countof(system)));
    assert(SetCurrentDirectoryW(system));
    assert(SetEnvironmentVariableW(L"PATH", system)); // No Python/Conda on PATH.
    assert(SetEnvironmentVariableW(L"CONDA_PREFIX", nullptr));
    startHumanTrajectoryTool(nullptr, true);
    assert(toolProcess.handle);
    const auto model = directory / L"mouse.bin";
    const auto deadline = GetTickCount64() + 30000;
    while (!fs::exists(model) && GetTickCount64() < deadline && toolProcess.running()) Sleep(50);
    const bool saved = fs::exists(model) && fs::file_size(model) == 11728;
    Sleep(200);
    const bool retained = toolProcess.running();
    if (toolProcess.handle) {
        TerminateProcess(toolProcess.handle, 0); // Close only this test's child.
        WaitForSingleObject(toolProcess.handle, 5000);
    }
    assert(saved && retained);
    assert(!fs::exists(directory / L"mouse_data.csv"));
    assert(ht::loadSamples(directory / L"人手数据.txt").size() == 4);
    assert(Collector(directory).count == 2);
    assert(!fs::exists(directory / L"train.py"));
    assert(!fs::exists(directory / L"collect_mouse_data.py"));
    puts("PASS: button launches same EXE; default EXE-relative folder; CPU training without Python; results window retained.");
}

int main()
{
    SetProcessDPIAware();
    int result = 0;
    if (runHumanTrajectoryCommandLine(GetModuleHandleW(nullptr), result)) return result;
    const auto directory = fs::temp_directory_path() /
        (L"saga-native-hand-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directory(directory);
    testGradientAndAdam(directory);
    testCsvAndModel(directory);
    testModelReplacement(directory);
    testNativeCollector(directory / L"collector");
    testButtonLauncher(directory);
    // Verify the interprocess lock without relying on the parent UI handle.
    { DataLock lock(directory);
      bool blocked = false;
      try { DataLock second(directory); } catch (const std::runtime_error&) { blocked = true; }
      assert(blocked); }
    DataLock availableAgain(directory);
    printf("PASS: exclusive data access is released after the tool closes.\n");
    printf("FIXTURES=%s\n", directory.string().c_str());
}
