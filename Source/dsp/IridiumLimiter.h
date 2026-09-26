#pragma once
#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <vector>
#include <deque>

/**
 * Iridium limiter —— 分段前视增益场（v0.0.2）。
 *
 * 依据「局部极值约束 + 零点锚定 + 两个半周期前视」：
 *   - 把 mid 信号按过零点切成「半周期」段；
 *   - 每段增益 = ceiling ÷ 该段峰值（局部极值约束），段内恒定；
 *   - 增益只在过零点切换（零点锚定：x=0 处 y=x·g=0，无咔哒）；
 *   - 前视 = 一个半周期（固定 50ms 缓冲覆盖「两个半周期」）；
 *   - 增益切换为纯阶跃（v0.0.2 抹掉残留 release：无任何平滑 / attack / release）；
 *   - 峰值精确贴顶，微小过冲由处理器基频域 Final Ceiling 兜底。
 *
 * 全程 16× 过采样（抗混叠）。链接式立体声：增益由 max(|L|,|R|) 驱动、锚定在 mid 过零点。
 */
class IridiumLimiter
{
public:
    IridiumLimiter() = default;

    /** @param oversampledRate  16× 过采样后的采样率
     *  @param maxBlockSize     过采样域最大块大小
     *  @param bufferMs         前视缓冲（默认 50ms，覆盖「两个半周期」） */
    void prepare (double oversampledRate, int maxBlockSize, double bufferMs)
    {
        fs = (float) oversampledRate;
        lookahead = juce::jmax (16, (int) std::llround (bufferMs * 0.001 * fs));
        bufferSize = (lookahead + maxBlockSize + 64) * 2;
        delayL.assign (bufferSize, 0.0f);
        delayR.assign (bufferSize, 0.0f);
        grMeterRelease = 1.0f - std::exp (-1.0f / (0.100f * fs));  // GR 表显示释放 ~100ms（仅显示，不影响音频）
        reset();
    }

    void setCeiling (float dB) { ceilLin = std::pow (10.0f, dB / 20.0f); }

    void reset()
    {
        std::fill (delayL.begin(), delayL.end(), 0.0f);
        std::fill (delayR.begin(), delayR.end(), 0.0f);
        writePos = 0;
        prevMidSign = 0;
        prevOutMidSign = 0;
        segPeak = 0.0f;
        firstSegment = true;
        gainQueue.clear();
        currentGain = 1.0f;
        grPeak = 1.0f;
        lastGR = 0.0f;
    }

    void process (juce::dsp::AudioBlock<float>& block)
    {
        const int ns = (int) block.getNumSamples();

        // 块过大时扩容（防御性；正常情况下 prepare 已够用）
        const int need = (lookahead + ns + 64) * 2;
        if (need > (int) delayL.size())
        {
            bufferSize = need;
            delayL.assign (bufferSize, 0.0f);
            delayR.assign (bufferSize, 0.0f);
            writePos = 0;
        }

        float* L = block.getChannelPointer (0);
        float* R = block.getNumChannels() > 1 ? block.getChannelPointer (1) : L;

        for (int n = 0; n < ns; ++n)
        {
            const float xl = L[n];
            const float xr = R[n];

            // ── 输入域：半周期分段 + 峰值累积 + 前视增益队列 ──
            const float mid = (xl + xr) * 0.5f;
            const int s = (mid > 0.0f) ? 1 : (mid < 0.0f) ? -1 : 0;
            const bool inCrossing = (prevMidSign != 0 && s != prevMidSign);
            prevMidSign = s;

            segPeak = std::max (segPeak, std::max (std::fabs (xl), std::fabs (xr)));

            if (inCrossing)
            {
                const float gSeg = (segPeak > 1e-9f) ? juce::jlimit (1e-4f, 1.0f, ceilLin / segPeak) : 1.0f;
                if (firstSegment)
                {
                    firstSegment = false;
                    currentGain = gSeg;          // 首段直接应用（其输出尚未开始）
                }
                else
                {
                    gainQueue.push_back (gSeg);   // 其余段入队，等前视对齐
                }
                segPeak = 0.0f;
            }

            // ── 输出域：延迟一个半周期（前视），过零点纯阶跃切换增益 ──
            delayL[writePos] = xl;
            delayR[writePos] = xr;

            int rp = writePos - lookahead;
            if (rp < 0) rp += bufferSize;
            const float dl = delayL[rp];
            const float dr = delayR[rp];

            const float dmid = (dl + dr) * 0.5f;
            const int ds = (dmid > 0.0f) ? 1 : (dmid < 0.0f) ? -1 : 0;
            const bool outCrossing = (prevOutMidSign != 0 && ds != prevOutMidSign);
            prevOutMidSign = ds;

            if (outCrossing && !gainQueue.empty())
            {
                currentGain = gainQueue.front();
                gainQueue.pop_front();
            }

            // ── 应用 + Final Ceiling 硬顶 ──
            float yl = dl * currentGain;
            float yr = dr * currentGain;
            if (yl >  ceilLin) yl =  ceilLin; else if (yl < -ceilLin) yl = -ceilLin;
            if (yr >  ceilLin) yr =  ceilLin; else if (yr < -ceilLin) yr = -ceilLin;
            L[n] = yl;
            R[n] = yr;

            ++writePos;
            if (writePos >= bufferSize) writePos = 0;

            // ── GR 表：瞬时下降 + 显示缓慢回升（仅显示，不影响音频增益） ──
            if (currentGain < grPeak) grPeak = currentGain;
            else grPeak += grMeterRelease * (currentGain - grPeak);
        }

        lastGR = 20.0f * std::log10 (grPeak + 1e-20f);
    }

    float getGainReductionDB() const noexcept { return lastGR; }   // ≤ 0 dB
    int getLatencySamples() const noexcept { return lookahead; }   // 过采样域样本数

private:
    float fs = 44100.0f * 16.0f;
    float ceilLin = 1.0f;            // 0 dB（Ceiling 固定）
    float grMeterRelease = 0.0001f;
    float segPeak = 0.0f;
    float currentGain = 1.0f;
    float grPeak = 1.0f, lastGR = 0.0f;
    int prevMidSign = 0, prevOutMidSign = 0;
    bool firstSegment = true;
    int lookahead = 0, bufferSize = 0, writePos = 0;
    std::vector<float> delayL, delayR;
    std::deque<float> gainQueue;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IridiumLimiter)
};
