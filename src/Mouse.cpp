#include "Inference.h"
//  极致性能优化的神经网络推理 DLL (AVX2 + FMA 完整版)
//  编译: cl /O2 /arch:AVX2 /fp:fast /GS- /GL /LD /MT
//
//  优化要点：
//    1. 融合 Layer1 与 Layer2，消除 256 字节中间缓冲区
//    2. 偏置直接初始化为累加器（省去最后的加法）
//    3. Layer3 权重行 padding 至 96 字节，全部使用对齐加载
//    4. 关键循环手工展开，提升指令级并行度
//    5. 使用栈上的标量数组进行 store‑to‑load forwarding，延迟极低

#include <cstdio>
#include <immintrin.h>
#include <bit>
#include <cstdint>
#include <mutex>
#include <shared_mutex>

// -------------------------------------------------------------------
//  网络参数：32 字节对齐，专为 SIMD 设计
// -------------------------------------------------------------------
struct alignas(32) NetOpt {
    alignas(32) float w1_x[64];
    alignas(32) float w1_y[64];
    alignas(32) float b1[64];

    // 转置权重 [64][32]，每行 128 字节，完美 32 对齐
    alignas(32) float w2_t[64][32];
    alignas(32) float b2[32];

    // 修改为 [32][24]，每行 96 字节（32 的倍数），可全对齐加载
    alignas(32) float w3_t[32][24];
    alignas(32) float b3[20];          // 偏置仍为 20 个
};

static alignas(32) NetOpt g_net;
static int              g_loaded = 0;
static std::shared_mutex g_netMutex;

// ===================================================================
//  mc_create : 一次性文件读取 + 转置重排 + Layer3 padding
// ===================================================================
int saga::mc_create(const char* path)
{
    struct NetOrig {
        float w1[128];      // [64][2]
        float b1[64];
        float w2[2048];     // [32][64]
        float b2[32];
        float w3[640];      // [20][32]
        float b3[20];
    } net_orig;

    FILE* f = fopen(path, "rb");
    if (!f) return 0;

    if (fread(&net_orig, sizeof(NetOrig), 1, f) != 1) {
        fclose(f);
        return 0;
    }
    const bool exactSize = fgetc(f) == EOF && !ferror(f);
    fclose(f);
    if (!exactSize) return 0;
    // Bitwise validation remains reliable with the inference file's /fp:fast.
    const auto finite = [](const auto& values) {
        for (float value : values)
            if ((std::bit_cast<uint32_t>(value) & 0x7f800000u) == 0x7f800000u) return false;
        return true;
    };
    if (!finite(net_orig.w1) || !finite(net_orig.b1) || !finite(net_orig.w2) ||
        !finite(net_orig.b2) || !finite(net_orig.w3) || !finite(net_orig.b3)) return 0;
    NetOpt replacement{};

    // 第一层：分离 x / y 权重
    for (int i = 0; i < 64; ++i) {
        replacement.w1_x[i] = net_orig.w1[i * 2];
        replacement.w1_y[i] = net_orig.w1[i * 2 + 1];
        replacement.b1[i] = net_orig.b1[i];
    }

    // 第二层转置 [32][64] -> [64][32]
    for (int j = 0; j < 64; ++j) {
        for (int i = 0; i < 32; ++i) {
            replacement.w2_t[j][i] = net_orig.w2[i * 64 + j];
        }
    }
    for (int i = 0; i < 32; ++i) replacement.b2[i] = net_orig.b2[i];

    // 第三层转置 [20][32] -> [32][24]，并用 0 填充右侧 4 列
    for (int j = 0; j < 32; ++j) {
        for (int i = 0; i < 20; ++i) {
            replacement.w3_t[j][i] = net_orig.w3[i * 32 + j];
        }
        // 填充 0 以保证 32 字节对齐加载
        for (int i = 20; i < 24; ++i) {
            replacement.w3_t[j][i] = 0.0f;
        }
    }
    for (int i = 0; i < 20; ++i) replacement.b3[i] = net_orig.b3[i];

    std::unique_lock lock(g_netMutex);
    g_net = replacement;
    g_loaded = 1;
    return 1;
}

// ===================================================================
//  mc_calc : 极致优化推理
void saga::mc_calc(float x, float y, float* __restrict out)
{
    std::shared_lock lock(g_netMutex);
    if (!g_loaded) return;

    const __m256 vx = _mm256_set1_ps(x);
    const __m256 vy = _mm256_set1_ps(y);
    const __m256 vzero = _mm256_setzero_ps();

    // ---- Phase 1: 64 个 L1 输出一次算完(8 条独立链,完全并行) ----
    __declspec(align(32)) float l1[64];
    for (int g = 0; g < 8; ++g) {
        const int base = g * 8;
        __m256 t = _mm256_fmadd_ps(_mm256_load_ps(&g_net.w1_x[base]), vx,
            _mm256_fmadd_ps(_mm256_load_ps(&g_net.w1_y[base]), vy,
                _mm256_load_ps(&g_net.b1[base])));
        _mm256_store_ps(&l1[base], _mm256_max_ps(t, vzero));
    }

    // ---- Phase 2: L2 = FC(64->32)+ReLU,双累加器把串行链 64 -> 32 ----
    __m256 a0 = _mm256_load_ps(&g_net.b2[0]);    // a 组: 输入 0..31,带偏置
    __m256 a1 = _mm256_load_ps(&g_net.b2[8]);
    __m256 a2 = _mm256_load_ps(&g_net.b2[16]);
    __m256 a3 = _mm256_load_ps(&g_net.b2[24]);
    __m256 b0 = vzero, b1 = vzero, b2 = vzero, b3 = vzero;   // b 组: 输入 32..63,零起步

    for (int j = 0; j < 32; ++j) {
        __m256 va = _mm256_set1_ps(l1[j]);
        __m256 vb = _mm256_set1_ps(l1[j + 32]);
        a0 = _mm256_fmadd_ps(va, _mm256_load_ps(&g_net.w2_t[j][0]), a0);
        b0 = _mm256_fmadd_ps(vb, _mm256_load_ps(&g_net.w2_t[j + 32][0]), b0);
        a1 = _mm256_fmadd_ps(va, _mm256_load_ps(&g_net.w2_t[j][8]), a1);
        b1 = _mm256_fmadd_ps(vb, _mm256_load_ps(&g_net.w2_t[j + 32][8]), b1);
        a2 = _mm256_fmadd_ps(va, _mm256_load_ps(&g_net.w2_t[j][16]), a2);
        b2 = _mm256_fmadd_ps(vb, _mm256_load_ps(&g_net.w2_t[j + 32][16]), b2);
        a3 = _mm256_fmadd_ps(va, _mm256_load_ps(&g_net.w2_t[j][24]), a3);
        b3 = _mm256_fmadd_ps(vb, _mm256_load_ps(&g_net.w2_t[j + 32][24]), b3);
    }

    __m256 l2_0 = _mm256_max_ps(_mm256_add_ps(a0, b0), vzero);
    __m256 l2_1 = _mm256_max_ps(_mm256_add_ps(a1, b1), vzero);
    __m256 l2_2 = _mm256_max_ps(_mm256_add_ps(a2, b2), vzero);
    __m256 l2_3 = _mm256_max_ps(_mm256_add_ps(a3, b3), vzero);

    __declspec(align(32)) float l2[32];
    _mm256_store_ps(&l2[0], l2_0);
    _mm256_store_ps(&l2[8], l2_1);
    _mm256_store_ps(&l2[16], l2_2);
    _mm256_store_ps(&l2[24], l2_3);

    // ---- Phase 3: L3 = FC(32->20),同样双累加器,链 32 -> 16 ----
    __m256 o0a = _mm256_load_ps(&g_net.b3[0]), o0b = vzero;
    __m256 o1a = _mm256_load_ps(&g_net.b3[8]), o1b = vzero;
    __m128 o2a = _mm_load_ps(&g_net.b3[16]), o2b = _mm_setzero_ps();

    for (int j = 0; j < 16; ++j) {
        __m256 va = _mm256_set1_ps(l2[j]);
        __m256 vb = _mm256_set1_ps(l2[j + 16]);
        o0a = _mm256_fmadd_ps(va, _mm256_load_ps(&g_net.w3_t[j][0]), o0a);
        o0b = _mm256_fmadd_ps(vb, _mm256_load_ps(&g_net.w3_t[j + 16][0]), o0b);
        o1a = _mm256_fmadd_ps(va, _mm256_load_ps(&g_net.w3_t[j][8]), o1a);
        o1b = _mm256_fmadd_ps(vb, _mm256_load_ps(&g_net.w3_t[j + 16][8]), o1b);
        o2a = _mm_fmadd_ps(_mm256_castps256_ps128(va), _mm_load_ps(&g_net.w3_t[j][16]), o2a);
        o2b = _mm_fmadd_ps(_mm256_castps256_ps128(vb), _mm_load_ps(&g_net.w3_t[j + 16][16]), o2b);
    }

    _mm256_storeu_ps(&out[0], _mm256_add_ps(o0a, o0b));
    _mm256_storeu_ps(&out[8], _mm256_add_ps(o1a, o1b));
    _mm_storeu_ps(&out[16], _mm_add_ps(o2a, o2b));
}

// ===================================================================
//  mc_destroy
// ===================================================================
void saga::mc_destroy()
{
    std::unique_lock lock(g_netMutex);
    g_loaded = 0;
}
