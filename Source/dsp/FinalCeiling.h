#pragma once
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cmath>
#include <vector>

/**
 * Final Ceiling —— 前视平滑增益真峰限幅 + tanh 软顶兜底（16× 抗混叠）。
 *
 * 思路（先进低失真，非静态 waveshaper）：
 *   - 16× 上采样后，用「前视延迟 + 平滑增益包络」做主限幅：
 *     增益在峰值到来之前就缓降到位，峰值被整体衰减，几乎不产生非线性失真；
 *   - 再用 tanh 软顶兜底，只处理增益没抓干净的小残峰（软拐角，高次谐波少）；
 *   - 全程 16× 抗混叠：削波/增益调制产生的带外分量被降采样滤波挡掉。
 *
 * FC 开 = 上述真峰限幅器；FC 关 = 完全透明（仅过采样对齐延迟，延迟恒定）。
 */
class FinalCeiling
{
public:
    void prepare (double rate, int maxBlock)
    {
        detOS.initProcessing ((std::size_t) maxBlock);
        detOS.reset();

        osRate    = rate * (double) detOS.getOversamplingFactor();   // 16×
        lookahead = (int) std::llround (lookaheadSec * osRate);       // 前视（16× 域样本）

        const int osMax = maxBlock * (int) detOS.getOversamplingFactor();
        delayL.assign (lookahead + osMax + 64, 0.0);
        delayR.assign (lookahead + osMax + 64, 0.0);

        // 单极点平滑增益系数（16× 域内算，平滑粒度足够细）
        attackCoef  = 1.0 - std::exp (-1.0 / (attackSec  * osRate));
        releaseCoef = 1.0 - std::exp (-1.0 / (releaseSec * osRate));

        gain = 1.0;
        writePos = 0;

        // 延迟 = detOS 滤波延迟 + 2ms 前视（基频样本数，恒定）
        latency = (int) detOS.getLatencyInSamples() + (int) std::llround (lookaheadSec * rate);
    }

    void reset() noexcept
    {
        detOS.reset();
        std::fill (delayL.begin(), delayL.end(), 0.0);
        std::fill (delayR.begin(), delayR.end(), 0.0);
        gain = 1.0;
        writePos = 0;
    }

    int getLatencySamples() const noexcept { return latency; }

    void process (juce::AudioBuffer<double>& buffer, bool fcOn)
    {
        if (buffer.getNumSamples() <= 0) return;

        auto block  = juce::dsp::AudioBlock<double> (buffer);
        auto osBlock = detOS.processSamplesUp (block);   // 16×

        if (fcOn)
        {
            const int osN = (int) osBlock.getNumSamples();
            const int nch = (int) osBlock.getNumChannels();

            // 防御性扩容（正常情况下 prepare 已够用）
            const int need = lookahead + osN + 64;
            if (need > (int) delayL.size())
            {
                delayL.assign (need, 0.0);
                delayR.assign (need, 0.0);
                writePos = 0;
            }

            double* ch0 = osBlock.getChannelPointer (0);
            double* ch1 = nch > 1 ? osBlock.getChannelPointer (1) : ch0;

            double g = gain;
            int w = writePos;
            const int dsz = (int) delayL.size();

            for (int i = 0; i < osN; ++i)
            {
                const double xl = ch0[i];
                const double xr = ch1[i];

                // 1) 当前峰值 → 目标增益（左右联动取大）
                const double a = std::max (std::fabs (xl), std::fabs (xr));
                const double gTarget = (a > ceilLin) ? (ceilLin / a) : 1.0;

                // 2) 单极点平滑增益（攻击快 / 释放慢；正确方向 g += coef*(target-g)）
                g += (gTarget < g ? attackCoef : releaseCoef) * (gTarget - g);

                // 3) 前视延迟 + 应用平滑增益（增益在峰值到达前就缓降到位）
                delayL[w] = xl;
                delayR[w] = xr;
                int rp = w - lookahead;
                if (rp < 0) rp += dsz;

                // 4) tanh 软顶兜底（只处理没抓干净的小残峰）
                ch0[i] = tanhTop (delayL[rp] * g);
                ch1[i] = tanhTop (delayR[rp] * g);

                ++w;
                if (w >= dsz) w = 0;
            }

            gain = g;
            writePos = w;
        }

        detOS.processSamplesDown (block);
    }

private:
    /** tanh 软顶：阈值以下透明，阈值以上软拐角逼近 ceilLin+headroom。 */
    static double tanhTop (double x) noexcept
    {
        const double a = std::fabs (x);
        if (a > ceilLin)
        {
            const double sign   = (x > 0.0) ? 1.0 : -1.0;
            const double excess = a - ceilLin;
            return sign * (ceilLin + headroom * std::tanh (excess / headroom));
        }
        return x;
    }

    // ── 校准旋钮（PluginDoctor 实测微调）──────────────────────────────────
    static constexpr double ceilLin      = 1.0;    // 0 dB 天花板
    static constexpr double lookaheadSec = 0.002;  // 前视 2 ms（增益提前缓降）
    static constexpr double attackSec    = 0.0005; // 攻击 0.5 ms
    static constexpr double releaseSec   = 0.020;  // 释放 20 ms
    static constexpr double headroom     = 0.05;   // tanh 软顶余量 ≈ +0.4 dB

    double osRate = 768000.0;
    double attackCoef  = 0.0;
    double releaseCoef = 0.0;
    double gain = 1.0;
    int  lookahead = 0;
    int  writePos = 0;
    int  latency = 0;
    std::vector<double> delayL, delayR;

    juce::dsp::Oversampling<double> detOS { 2, 4,
        juce::dsp::Oversampling<double>::filterHalfBandPolyphaseIIR, true, false };
};
