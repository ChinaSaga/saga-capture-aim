#pragma once
// ============================================================================
//  锁定选靶：同人物聚类 + 锁定粘滞
//
//  为什么需要它：原版是「每帧从零重选最近目标」，准星附近的头一旦被折叠/遮挡消失，
//  就会跳到旁边那个人身上；同一个人的头(1)/身(2)还会因为优先级互相抢锁 → 疯狂摇晃。
//
//  这里做三件事：
//    1) 把 优先/其次/再次 三档的框按「垂直相邻 + 水平重叠」合并成一个「人」
//    2) 锁定单位从"框"升级为"人"：这个人的头没了 → 继续用这个人的身体
//    3) 跨帧认人（IoU/中心距）+ 失锁帧数确认 + 抢锁门槛 → 同一个人的锁不会被轻易抢走
//
//  纯逻辑，不依赖引擎/UI/线程；等价性验证记录见 docs/performance/AIMLOCK_PERFORMANCE.md。
//  参数按需求先硬编码在 Params 的默认值里。
// ============================================================================
#include <algorithm>
#include <cmath>

namespace aimlock
{
struct Box
{
    double x = 0, y = 0, w = 0, h = 0;   // 图像坐标下的框
    double dx = 0, dy = 0;               // 相对准星的偏移（已含「锁定位置」偏移）
    double distance = 0;                 // |(dx, dy)|
    double normRatio = 0;                // 相对矩形中心的归一化比例
    float  conf = 0;
    int    lockType = 99;                // 1优先 / 2其次 / 3再次 / 99其它
};

struct Params
{
    int    lockRange        = 640;   // 整数_锁定范围
    int    prevNearestRange = 0;     // 上一帧选中目标的距离（原版 局_画框最近范围）
    bool   moveInBox        = false; // 文本_框内移动
    int    holdFrames       = 3;     // 连续多少帧认不出上一帧那个人，才允许换目标
    double stealUp          = 0.75;  // 抢锁：类别更优 → 新目标锚点距离 ≤ 当前 × 0.75
    double stealSame        = 0.60;  // 抢锁：同类别   → 新目标锚点距离 ≤ 当前 × 0.60
    double stealDown        = 0.45;  // 抢锁：类别更差 → 新目标锚点距离 ≤ 当前 × 0.45
    // ⚠ 抢锁一律用「锚点距离」（该人最大那个框=身体，位置稳定）而不是代表部位的距离：
    //   头被折叠时代表部位会在头/身之间变，距离会从 50 跳到 110，按它比较就会来回抢锁 = 摇晃
    double sameBodyOverlap  = 0.50;  // 同一个人：水平重叠 ≥ 窄框的 50%
    double sameBodyTopK     = 0.40;  // 同一个人：小框底最多低于大框顶 大框高×0.4
    double sameBodyGap      = 0.30;  // 同一个人：小框底最多高于大框顶 大框高×0.3
    double matchIou         = 0.20;  // 跨帧认框：IoU ≥ 0.2
    double matchDistK       = 0.60;  // 跨帧认框：中心距 ≤ max(24px, 0.6 × 框宽)
};

// 跨帧锁定记忆（每个推理线程一份，调用方自己保存）
struct Memory
{
    Box part[4];        // 上一帧锁定那个人的各部位框
    int count = 0;
    int lost  = 0;      // 连续多少帧没认出那个人
};

struct Result
{
    int  index = -1;    // 选中的候选框下标；-1 = 这一帧不移动
    bool held  = false; // true = 正在「失锁确认」里按住不动
};

// 两个框是不是「同一个人」的两个部位（头挂在身子上）：
//   水平：小框要有 ≥50% 落在大框的 x 区间里
//   垂直：小框要「挂」在大框顶部附近（小框底 ≈ 大框顶）。
//         用这个而不是"有重叠就算"，是为了避免把画面上上下叠着的两个人误判成一个人。
inline bool sameBody(const Box& a, const Box& b, const Params& p)
{
    // 变量名别用 small / near / far / min / max：Windows 头文件里是宏（small = char）
    const Box& smallBox = (a.h <= b.h) ? a : b;
    const Box& bigBox   = (a.h <= b.h) ? b : a;

    const double overlapX = (std::min)(smallBox.x + smallBox.w, bigBox.x + bigBox.w) - (std::max)(smallBox.x, bigBox.x);
    if (smallBox.w <= 0.0 || overlapX < smallBox.w * p.sameBodyOverlap) return false;

    const double delta = (smallBox.y + smallBox.h) - bigBox.y;   // 小框底 − 大框顶
    return delta >= -bigBox.h * p.sameBodyGap && delta <= bigBox.h * p.sameBodyTopK;
}

// 跨帧认同一个框：类别相同，且 IoU 够大 或 中心离得够近
inline bool sameBoxAcrossFrames(const Box& a, const Box& b, const Params& p)
{
    if (a.lockType != b.lockType) return false;

    const double ix = (std::min)(a.x + a.w, b.x + b.w) - (std::max)(a.x, b.x);
    const double iy = (std::min)(a.y + a.h, b.y + b.h) - (std::max)(a.y, b.y);
    const double inter = (ix > 0.0 && iy > 0.0) ? ix * iy : 0.0;
    const double uni = a.w * a.h + b.w * b.h - inter;
    if (uni > 0.0 && inter / uni >= p.matchIou) return true;

    const double ax = a.x + a.w * 0.5, ay = a.y + a.h * 0.5;
    const double bx = b.x + b.w * 0.5, by = b.y + b.h * 0.5;
    const double limit = (std::max)(24.0, p.matchDistK * (std::max)(a.w, b.w));
    const double dx = ax - bx, dy = ay - by;
    return (dx * dx + dy * dy) <= limit * limit;
}

// 选靶：返回这一帧该锁哪个框（-1 = 不移动），并维护跨帧记忆
inline Result select(const Box* box, int count, const Params& p, Memory& mem)
{
    Result r;
    if (count <= 0)
    {
        ++mem.lost;
        if (mem.lost > p.holdFrames) mem.count = 0;
        else if (mem.count > 0) r.held = true;
        return r;
    }
    if (count > 99) count = 99;

    // 类别序号：1 最好，2/3 次之，其它最末（数字越小越优先）
    const auto typeRank = [](int lockType) { return (lockType >= 1 && lockType <= 3) ? lockType : 4; };

    // ---------------- 把框按「同一个人」聚类（只合并 1/2/3 三档）----------------
    // 注意：不能"只要几何上贴合就并起来"——画面上上下叠着的两个人会顺着
    // 头→别人的身→别人的头 串成一串，锁就跑到别人身上了。
    // 这里按「贴合程度」（小框底到大框顶的距离）从好到差贪心挂靠，
    // 且每个框最多挂一个上级、最多被一个下级挂，形成 头→上半身→全身 这样的链。
    struct Pair { int part, body; double score; };   // Only the populated prefix is read.
    Pair pairs[256];
    int pairCount = 0;
    // A body link needs two different eligible types. Homogeneous frames
    // cannot produce a pair, so avoid their quadratic pair enumeration.
    bool mixedTypes = false;
    int firstType = 4;
    for (int i = 0; i < count; ++i)
    {
        const int type = box[i].lockType;
        if (type > 3) continue;
        if (firstType == 4) firstType = type;
        else if (type != firstType) { mixedTypes = true; break; }
    }
    if (mixedTypes)
    {
        for (int i = 0; i < count && pairCount < 256; ++i)
        {
            if (box[i].lockType > 3) continue;
            for (int j = i + 1; j < count && pairCount < 256; ++j)
            {
                if (box[j].lockType > 3 || box[j].lockType == box[i].lockType) continue;
                if (!sameBody(box[i], box[j], p)) continue;
                const bool iSmall = (box[i].h <= box[j].h);
                Pair& pr = pairs[pairCount++];
                pr.part = iSmall ? i : j;
                pr.body   = iSmall ? j : i;
                pr.score = std::fabs((box[pr.part].y + box[pr.part].h) - box[pr.body].y);
            }
        }
    }
    int clusterBest[99], clusterAnchor[99];
    int firstMember[99], nextMember[99];
    int peopleCount = 0;
    if (pairCount == 0)
    {
        // No body links: every box is already a complete singleton group.
        peopleCount = count;
        for (int i = 0; i < count; ++i)
        {
            clusterBest[i] = clusterAnchor[i] = firstMember[i] = i;
            nextMember[i] = -1;
        }
    }
    else
    {
        std::sort(pairs, pairs + pairCount,
            [](const Pair& a, const Pair& b) { return a.score < b.score; });

        int group[99], up[99], down[99];
        for (int i = 0; i < count; ++i) { group[i] = i; up[i] = -1; down[i] = -1; }
        for (int k = 0; k < pairCount; ++k)
        {
            const Pair& pr = pairs[k];
            if (up[pr.part] >= 0 || down[pr.body] >= 0) continue;
            up[pr.part] = pr.body;
            down[pr.body] = pr.part;
            int ri = pr.part; while (group[ri] != ri) ri = group[ri];
            int rj = pr.body; while (group[rj] != rj) rj = group[rj];
            if (ri != rj) group[ri] = rj;
        }

        int rootToSlot[99], lastMember[99];
        std::fill_n(rootToSlot, count, -1);
        for (int i = 0; i < count; ++i)
        {
            int root = i; while (group[root] != root) root = group[root];
            int slot = rootToSlot[root];
            if (slot < 0)
            {
                slot = peopleCount++;
                rootToSlot[root] = slot;
                clusterBest[slot] = i;
                clusterAnchor[slot] = i;
                firstMember[slot] = i;
            }
            else nextMember[lastMember[slot]] = i;
            lastMember[slot] = i;
            nextMember[i] = -1;
            // Preserve representative/anchor comparisons and encounter order.
            const Box& cur = box[i];
            const Box& best = box[clusterBest[slot]];
            if (cur.lockType < best.lockType ||
                (cur.lockType == best.lockType && cur.distance < best.distance))
                clusterBest[slot] = i;
            if (cur.w * cur.h > box[clusterAnchor[slot]].w * box[clusterAnchor[slot]].h)
                clusterAnchor[slot] = i;
        }
    }

    // 选中某个人：记住他的所有部位框，供下一帧认人
    auto pick = [&](int slot)
    {
        mem.count = 0;
        for (int i = firstMember[slot]; i >= 0 && mem.count < 4; i = nextMember[i])
            mem.part[mem.count++] = box[i];
        mem.lost = 0;
        r.index = clusterBest[slot];
        r.held = false;
    };

    // ---------------- 锁定粘滞第 1 步：上一帧锁的那个人这一帧还在不在 ----------------
    int locked = -1;
    if (mem.count > 0)
    {
        for (int k = 0; k < peopleCount && locked < 0; ++k)
        {
            for (int i = firstMember[k]; i >= 0 && locked < 0; i = nextMember[i])
            {
                for (int m = 0; m < mem.count; ++m)
                    if (sameBoxAcrossFrames(box[i], mem.part[m], p)) { locked = k; break; }
            }
        }
    }
    if (locked >= 0 && box[clusterBest[locked]].distance >= (double)p.lockRange)
        locked = -1;                                       // 已经脱离锁定范围 → 重新选

    if (locked >= 0)
    {
        // 认出来了：默认继续锁这个人（头没了就自动改用这个人的身体），
        // 只有别的「人」明显更优（抢锁门槛）才换。比较一律用锚点距离（见 Params 注释）。
        const Box& hold = box[clusterBest[locked]];
        const double holdDist = box[clusterAnchor[locked]].distance;
        int steal = -1;
        for (int k = 0; k < peopleCount; ++k)
        {
            if (k == locked) continue;
            const Box& c = box[clusterBest[k]];
            if (c.distance >= (double)p.lockRange) continue;
            const double need = (typeRank(c.lockType) < typeRank(hold.lockType)) ? p.stealUp
                              : (typeRank(c.lockType) == typeRank(hold.lockType) ? p.stealSame : p.stealDown);
            if (box[clusterAnchor[k]].distance > holdDist * need) continue;    // 没赢够身位，不抢
            if (steal < 0) { steal = k; continue; }
            const Box& s = box[clusterBest[steal]];
            if (typeRank(c.lockType) < typeRank(s.lockType) ||
                (typeRank(c.lockType) == typeRank(s.lockType) &&
                 box[clusterAnchor[k]].distance < box[clusterAnchor[steal]].distance)) steal = k;
        }
        pick(steal >= 0 ? steal : locked);
        return r;
    }

    // ---------------- 没认出来：先按住几帧，再按原版瀑布重选 ----------------
    ++mem.lost;
    if (mem.count > 0 && mem.lost <= p.holdFrames)
    {
        r.held = true;      // 失锁确认中：不动，避免瞬间跳到旁边那个人
        return r;
    }

    mem.count = 0;

    // Only a genuine re-selection needs A/B/C/D. Tracked and held frames
    // have already returned without computing these unused rankings.
    int pA = -1, pB = -1, pC = -1, pD = -1;
    double dA = 1e18, dB = 1e18, dC = 1e18, dD = 1e18;
    for (int k = 0; k < peopleCount; ++k)
    {
        const Box& t = box[clusterBest[k]];
        if (t.lockType == 1 && t.distance < dA) { dA = t.distance; pA = k; }
        if (t.lockType == 2 && t.distance < dB) { dB = t.distance; pB = k; }
        if (t.lockType == 3 && t.distance < dC) { dC = t.distance; pC = k; }
        if (t.distance < dD) { dD = t.distance; pD = k; }
    }
    const Box empty;
    const Box& tA = (pA >= 0) ? box[clusterBest[pA]] : empty;
    const Box& tB = (pB >= 0) ? box[clusterBest[pB]] : empty;
    const Box& tC = (pC >= 0) ? box[clusterBest[pC]] : empty;
    const Box& tD = (pD >= 0) ? box[clusterBest[pD]] : empty;
    if (pA >= 0 && tA.distance < (double)p.prevNearestRange && tA.conf > tB.conf) { pick(pA); return r; }
    if (pB >= 0 && tB.distance < (double)p.prevNearestRange && tB.conf > tA.conf) { pick(pB); return r; }
    if (pA >= 0 && tA.distance < (double)p.lockRange) { pick(pA); return r; }
    if (pB >= 0 && tB.distance < (double)p.lockRange) { pick(pB); return r; }
    if (pC >= 0 && tC.distance < (double)p.lockRange) { pick(pC); return r; }

    if (p.moveInBox)
    {
        if (pA >= 0 && tA.normRatio <= 1.0) { pick(pA); return r; }
        if (pB >= 0 && tB.normRatio <= 1.0) { pick(pB); return r; }
        if (pC >= 0 && tC.normRatio <= 1.0) { pick(pC); return r; }
    }

    if (pD >= 0 && tD.distance < (double)p.lockRange) pick(pD);
    return r;
}

} // namespace aimlock
