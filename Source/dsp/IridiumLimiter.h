#pragma once
#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <vector>
#include <deque>

/**
 * Iridium limiter —— 分段前视增益场（v0.0.2，double 精度）。
 *
 * 依据「局部极值约束 + 零点锚定 + 两个半周期前视」：
 *   - 把 mid 信号按过零点切成「半周期」段；
 *   - 每段增益 = ceiling ÷ 该段峰值（局部极值约束），段内恒定；
 *   - 增益只在过零点切换（零点锚定：x=0 处 y=x·g=0，无咔哒）；
 *   - 前视 = 一个半周期（固定 50ms 缓冲覆盖「两个半周期」）；
 *   - 增益切换为纯阶跃（v0.0.2 抹掉残留 release：无任何平滑 / attack / release）。
 *
 * 全程 16× 过采样（抗混叠）。链接式立体声：增益由 max(|L|,|R|) 驱动、锚定在 mid 过零点。
 * 全程 float64（double）：压低量化噪声底。
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
        fs = oversampledRate;
        lookahead = juce::jmax (16, (int) std::llround (bufferMs * 0.001 * fs));
        bufferSize = (lookahead + maxBlockSize + 64) * 2;
        delayL.assign (bufferSize, 0.0);
        delayR.assign (bufferSize, 0.0);
        grMeterRelease = 1.0 - std::exp (-1.0 / (0.400 * fs));  // GR 表显示释放 ~400ms（仅显示，不影响音频）
        grMeterHold    = (int) std::llround (0.100 * fs);        // 峰值保持 ~100ms
        reset();
    }

    void setCeiling (double dB) { ceilLin = std::pow (10.0, dB / 20.0); }

    void reset()
    {
        std::fill (delayL.begin(), delayL.end(), 0.0);
        std::fill (delayR.begin(), delayR.end(), 0.0);
        writePos = 0;
        prevMidSign = 0;
        prevOutMidSign = 0;
        segPeak = 0.0;
        firstSegment = true;
        gainQueue.clear();
        currentGain = 1.0;
        grPeak = 1.0;
        grHoldCount = 0;
        lastGR = 0.0;
    }

    void process (juce::dsp::AudioBlock<double>& block)
    {
        const int ns = (int) block.getNumSamples();

        // 块过大时扩容（防御性；正常情况下 prepare 已够用）
        const int need = (lookahead + ns + 64) * 2;
        if (need > (int) delayL.size())
        {
            bufferSize = need;
            delayL.assign (bufferSize, 0.0);
            delayR.assign (bufferSize, 0.0);
            writePos = 0;
        }

        double* L = block.getChannelPointer (0);
        double* R = block.getNumChannels() > 1 ? block.getChannelPointer (1) : L;

        for (int n = 0; n < ns; ++n)
        {
            const double xl = L[n];
            const double xr = R[n];

            // ── 输入域：半周期分段 + 峰值累积 + 前视增益队列 ──
            const double mid = (xl + xr) * 0.5;
            const int s = (mid > 0.0) ? 1 : (mid < 0.0) ? -1 : 0;
            const bool inCrossing = (prevMidSign != 0 && s != prevMidSign);
            prevMidSign = s;

            segPeak = std::max (segPeak, std::max (std::fabs (xl), std::fabs (xr)));

            if (inCrossing)
            {
                const double gSeg = (segPeak > 1e-9) ? juce::jlimit (1e-4, 1.0, ceilLin / segPeak) : 1.0;
                if (firstSegment)
                {
                    firstSegment = false;
                    currentGain = gSeg;          // 首段直接应用（其输出尚未开始）
                }
                else
                {
                    gainQueue.push_back (gSeg);   // 其余段入队，等前视对齐
                }
                segPeak = 0.0;
            }

            // ── 输出域：延迟一个半周期（前视），过零点纯阶跃切换增益 ──
            delayL[writePos] = xl;
            delayR[writePos] = xr;

            int rp = writePos - lookahead;
            if (rp < 0) rp += bufferSize;
            const double dl = delayL[rp];
            const double dr = delayR[rp];

            const double dmid = (dl + dr) * 0.5;
            const int ds = (dmid > 0.0) ? 1 : (dmid < 0.0) ? -1 : 0;
            const bool outCrossing = (prevOutMidSign != 0 && ds != prevOutMidSign);
            prevOutMidSign = ds;

            if (outCrossing && !gainQueue.empty())
            {
                currentGain = gainQueue.front();
                gainQueue.pop_front();
            }

            // ── 应用 + 硬顶（v0.1.0 恢复：兜底保证 0 dB）──
            double yl = dl * currentGain;
            double yr = dr * currentGain;
            if (yl >  ceilLin) yl =  ceilLin; else if (yl < -ceilLin) yl = -ceilLin;
            if (yr >  ceilLin) yr =  ceilLin; else if (yr < -ceilLin) yr = -ceilLin;
            L[n] = yl;
            R[n] = yr;

            ++writePos;
            if (writePos >= bufferSize) writePos = 0;

            // ── GR 表：瞬时下降 + 峰值保持 ~100ms + 缓慢回升 ~400ms（仅显示，不影响音频增益） ──
            if (currentGain < grPeak)
            {
                grPeak = currentGain;
                grHoldCount = grMeterHold;
            }
            else if (grHoldCount > 0)
            {
                --grHoldCount;
            }
            else
            {
                grPeak += grMeterRelease * (currentGain - grPeak);
            }
        }

        lastGR = 20.0 * std::log10 (grPeak + 1e-20);
    }

    float getGainReductionDB() const noexcept { return (float) lastGR; }   // ≤ 0 dB（显示用途）
    int getLatencySamples() const noexcept { return lookahead; }   // 过采样域样本数

private:
    double fs = 44100.0 * 16.0;
    double ceilLin = 1.0;            // 0 dB（Ceiling 固定）
    double grMeterRelease = 0.0001;
    int grMeterHold = 0;
    double segPeak = 0.0;
    double currentGain = 1.0;
    double grPeak = 1.0, lastGR = 0.0;
    int grHoldCount = 0;
    int prevMidSign = 0, prevOutMidSign = 0;
    bool firstSegment = true;
    int lookahead = 0, bufferSize = 0, writePos = 0;
    std::vector<double> delayL, delayR;
    std::deque<double> gainQueue;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IridiumLimiter)
};
