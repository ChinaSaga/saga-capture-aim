#pragma once
#include "Inference.h"
#include <mat.h>
#include <opencv2/core.hpp>
#include <immintrin.h>
#include <algorithm>
#include <vector>
#include <cstring>
class NcnnPostprocess {
struct InternalObj
{
    cv::Rect_<float> rect;
    int   label;
    float prob;
};

std::vector<InternalObj>   g_proposals;
std::vector<InternalObj>   g_objects;
std::vector<InternalObj*>  g_ptrs;
std::vector<float*>        g_score_rows;  // 动态行指针缓存

// 缓存上次尺寸，避免重复预分配
int g_last_num_anchors = 0;
int g_last_num_classes = 0;

void ensure_buffers(int num_anchors, int num_classes)
{
    if (num_anchors != g_last_num_anchors || num_classes != g_last_num_classes)
    {
        g_last_num_anchors = num_anchors;
        g_last_num_classes = num_classes;

        g_proposals.reserve(num_anchors);
        g_objects.reserve(num_anchors);
        g_ptrs.reserve(num_anchors);

        // 行指针缓存
        g_score_rows.resize(num_classes);
    }
}


    std::vector<size_t> classOffsets_;
    std::vector<size_t> classCursor_;
public:
    void reset() { g_last_num_anchors = 0; g_last_num_classes = 0; }
    int run(ncnn::Mat& out, float scale, int pad_x, int pad_y, int img_w, int img_h, float conf_thres, float nms_thres, DetectObject* out_objects) {
        const int num_anchors = out.w;
        const int num_classes = out.h - 4;
        if (num_classes <= 0 || num_anchors <= 0) return -4;

        ensure_buffers(num_anchors, num_classes);
        for (int c = 0; c < num_classes; ++c)
            g_score_rows[c] = out.row(4 + c);

        const float scale_inv = 1.0f / scale;
        const float fw = static_cast<float>(img_w);
        const float fh = static_cast<float>(img_h);

        // ── 4. 收集候选框 ──────────────────
        float* row0 = out.row(0);
        float* row1 = out.row(1);
        float* row2 = out.row(2);
        float* row3 = out.row(3);
        g_proposals.clear();
        const auto appendCandidate = [&](int i, float score, int label) {
            const float bw = row2[i];
            const float bh = row3[i];
            const float rx = (row0[i] - bw * 0.5f - pad_x) * scale_inv;
            const float ry = (row1[i] - bh * 0.5f - pad_y) * scale_inv;
            g_proposals.emplace_back(cv::Rect_<float>(rx, ry, bw * scale_inv, bh * scale_inv), label, score);
        };

        // Keep a block of anchors' maxima in registers across all classes,
        // then materialize only candidates which pass the threshold.
        int anchor = 0;
#if defined(__AVX2__)
        const __m256 threshold = _mm256_set1_ps(conf_thres);
        const auto appendBlock = [&](int base, __m256 scores, __m256i labels) {
            const unsigned accepted = static_cast<unsigned>(_mm256_movemask_ps(
                _mm256_cmp_ps(scores, threshold, _CMP_NLT_UQ)));
            if (!accepted) return;
            alignas(32) float blockScores[8];
            alignas(32) int blockLabels[8];
            _mm256_store_ps(blockScores, scores);
            _mm256_store_si256(reinterpret_cast<__m256i*>(blockLabels), labels);
            for (unsigned lane = 0; lane < 8; ++lane)
                if (accepted & (1u << lane)) appendCandidate(base + lane, blockScores[lane], blockLabels[lane]);
        };
        // Four vectors use each class row's cache lines before moving to the
        // next row, while keeping the maxima in registers across all classes.
        for (; anchor + 32 <= num_anchors; anchor += 32) {
            __m256 s0 = _mm256_loadu_ps(g_score_rows[0] + anchor);
            __m256 s1 = _mm256_loadu_ps(g_score_rows[0] + anchor + 8);
            __m256 s2 = _mm256_loadu_ps(g_score_rows[0] + anchor + 16);
            __m256 s3 = _mm256_loadu_ps(g_score_rows[0] + anchor + 24);
            __m256i c0 = _mm256_setzero_si256(), c1 = c0, c2 = c0, c3 = c0;
            const auto update = [](__m256& score, __m256i& label, const float* row, __m256i nextClass) {
                const __m256 next = _mm256_loadu_ps(row);
                const __m256 gt = _mm256_cmp_ps(next, score, _CMP_GT_OQ);
                score = _mm256_max_ps(next, score);
                label = _mm256_blendv_epi8(label, nextClass, _mm256_castps_si256(gt));
            };
            for (int c = 1; c < num_classes; ++c) {
                const float* row = g_score_rows[c] + anchor;
                const __m256i label = _mm256_set1_epi32(c);
                update(s0, c0, row, label);
                update(s1, c1, row + 8, label);
                update(s2, c2, row + 16, label);
                update(s3, c3, row + 24, label);
            }
            appendBlock(anchor, s0, c0);
            appendBlock(anchor + 8, s1, c1);
            appendBlock(anchor + 16, s2, c2);
            appendBlock(anchor + 24, s3, c3);
        }
        for (; anchor + 8 <= num_anchors; anchor += 8) {
            __m256 scores = _mm256_loadu_ps(g_score_rows[0] + anchor);
            __m256i labels = _mm256_setzero_si256();
            for (int c = 1; c < num_classes; ++c) {
                const __m256 next = _mm256_loadu_ps(g_score_rows[c] + anchor);
                const __m256 gt = _mm256_cmp_ps(next, scores, _CMP_GT_OQ);
                scores = _mm256_max_ps(next, scores);
                labels = _mm256_blendv_epi8(labels, _mm256_set1_epi32(c), _mm256_castps_si256(gt));
            }
            appendBlock(anchor, scores, labels);
        }
#endif
        for (; anchor < num_anchors; ++anchor) {
            float score = g_score_rows[0][anchor];
            int label = 0;
            for (int c = 1; c < num_classes; ++c) {
                const float next = g_score_rows[c][anchor];
                if (next > score) { score = next; label = c; }
            }
            if (!(score < conf_thres)) appendCandidate(anchor, score, label);
        }

        // ── 5. NMS ─────────────────────────
        g_objects.clear();
        const float nms_thres_plus1 = 1.0f + nms_thres;

        // Stable counting partition: each proposal is visited twice, rather
        // than scanning the entire proposal list once per model class.
        classOffsets_.assign(num_classes + 1, 0);
        for (const auto& obj : g_proposals) ++classOffsets_[obj.label + 1];
        for (int c = 0; c < num_classes; ++c) classOffsets_[c + 1] += classOffsets_[c];
        classCursor_ = classOffsets_;
        g_ptrs.resize(g_proposals.size());
        for (auto& obj : g_proposals) g_ptrs[classCursor_[obj.label]++] = &obj;
        for (int c = 0; c < num_classes && g_objects.size() < 99; ++c)
        {
            const size_t n = classOffsets_[c + 1] - classOffsets_[c];
            if (n == 0) continue;
            auto* classPtrs = g_ptrs.data() + classOffsets_[c];

            if (n <= 16)
            {
                // 小数组插入排序, 免 std::sort 开销
                for (size_t i = 1; i < n; ++i)
                {
                    InternalObj* key = classPtrs[i];
                    const float kp = key->prob;
                    size_t j = i;
                    while (j > 0 && classPtrs[j - 1]->prob < kp)
                    {
                        classPtrs[j] = classPtrs[j - 1];
                        --j;
                    }
                    classPtrs[j] = key;
                }
            }
            else
            {
                std::sort(classPtrs, classPtrs + n,
                    [](const InternalObj* a, const InternalObj* b) {
                        return a->prob > b->prob;
                    });
            }

            // Only compare an examined candidate with already accepted boxes.
            // Suppressing every later candidate does unnecessary work when the
            // application stops after the first 99 accepted results.
            const size_t classBegin = g_objects.size();
            for (size_t i = 0; i < n; ++i)
            {
                const auto& r2 = classPtrs[i]->rect;
                const float a2 = r2.area();
                bool suppressed = false;
                for (size_t j = classBegin; j < g_objects.size(); ++j)
                {
                    const auto& r1 = g_objects[j].rect;

                    // 内联 IoU(避免 cv::Rect 临时对象)
                    float xx1 = r1.x > r2.x ? r1.x : r2.x;
                    float yy1 = r1.y > r2.y ? r1.y : r2.y;
                    float xx2 = r1.x + r1.width < r2.x + r2.width
                        ? r1.x + r1.width : r2.x + r2.width;
                    float yy2 = r1.y + r1.height < r2.y + r2.height
                        ? r1.y + r1.height : r2.y + r2.height;
                    float w = xx2 - xx1;
                    float h = yy2 - yy1;
                    if (w <= 0.f || h <= 0.f) continue;

                    // ia / (a1 + a2 - ia) > nms_thres  的乘法等价形式
                    float ia = w * h;
                    if (ia * nms_thres_plus1 > nms_thres * (r1.area() + a2)) {
                        suppressed = true;
                        break;
                    }
                }
                if (suppressed) continue;
                g_objects.push_back(*classPtrs[i]);
                if (g_objects.size() == 99) break; // Caller only consumes this exact prefix.
            }
        }

        // ── 6. 输出(边界裁剪)─────────────
        int count = 0;
        for (const auto& obj : g_objects)
        {
            if (count >= 99) break;
            float x1 = obj.rect.x;
            float y1 = obj.rect.y;
            if (x1 < 0.f) x1 = 0.f;
            if (y1 < 0.f) y1 = 0.f;
            float x2 = x1 + obj.rect.width;
            float y2 = y1 + obj.rect.height;
            if (x2 > fw) x2 = fw;
            if (y2 > fh) y2 = fh;

            out_objects[count].x = x1;
            out_objects[count].y = y1;
            out_objects[count].width = x2 - x1;
            out_objects[count].height = y2 - y1;
            out_objects[count].label = obj.label;
            out_objects[count].prob = obj.prob;
            ++count;
        }
        return count;
    }
};
