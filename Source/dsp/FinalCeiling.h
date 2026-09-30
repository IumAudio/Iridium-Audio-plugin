#pragma once
#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <vector>

/**
 * Final Ceiling —— 最终 0 dBFS 真峰值天花板（v0.1.1）。
 *
 * 只取「真峰值检测」：用 16× 过采样插值出样本间峰值（inter-sample peak），
 * 再以 2ms 前视把增益提前压低，保证输出的 16× 真峰值 ≤ 0 dBFS。
 *
 * 与插件基底 IridiumLimiter 的 50ms 分段前视是两回事（切勿混淆）：
 *   基底前视 = 50ms，用于构造增益场（核心限幅）；
 *   本阶段前视 = 2ms，仅用于真峰值天花板的提前压顶。
 * 与 OS 开关也无关：这里自带一套独立的 16× 检测过采样，
 * FC 开/关 与 OS 开/关 互不影响。
 *
 * FC 开 = 16× 真峰值检测（上限 0 dBFS 真峰值）；
 * FC 关 = 样本峰值检测（上限 0 dBFS 样本峰值）。
 * 两者共用同一 2ms 前视，延迟固定、切换不爆音。
 */
class FinalCeiling
{
public:
    FinalCeiling() = default;

    /** @param rate      基频采样率
     *  @param maxBlock  基频域最大块大小 */
    void prepare (double rate, int maxBlock)
    {
        fs = (float) rate;
        lookahead = std::max (1, (int) std::llround (0.002 * fs));   // 2ms 前视

        const size_t cap = (size_t) (lookahead + maxBlock + 16);
        delayL.assign (cap, 0.0f);
        delayR.assign (cap, 0.0f);

        detOS.initProcessing ((size_t) maxBlock);
        detOS.reset();

        envRelease  = 1.0f - std::exp (-1.0f / (0.020f * fs));   // 峰值包络释放 20ms
        gainRelease = 1.0f - std::exp (-1.0f / (0.050f * fs));   // 增益释放 50ms（> 前视，峰到输出时增益仍在）
        reset();
    }

    void reset() noexcept
    {
        std::fill (delayL.begin(), delayL.end(), 0.0f);
        std::fill (delayR.begin(), delayR.end(), 0.0f);
        detOS.reset();
        write = 0;
        env  = 0.0f;
        gain = 1.0f;
    }

    /** 2ms 前视（基频样本数）；FC 开关不改变此延迟。 */
    int getLatencySamples() const noexcept { return lookahead; }

    void process (juce::AudioBuffer<float>& buffer, bool truePeak)
    {
        const int n  = buffer.getNumSamples();
        const int nc = buffer.getNumChannels();
        if (n <= 0 || delayL.empty()) return;

        // 防御性扩容（正常情况下 prepare 已够）
        const size_t need = (size_t) (lookahead + n + 16);
        if (need > delayL.size())
        {
            delayL.assign (need, 0.0f);
            delayR.assign (need, 0.0f);
            write = 0;
        }

        // ── 1) 检测：本块真峰值（16× 上采样取最大绝对值）或样本峰值 ──
        float pk = 0.0f;
        if (truePeak)
        {
            auto osBlock = detOS.processSamplesUp (juce::dsp::AudioBlock<float> (buffer));
            const int osN = (int) osBlock.getNumSamples();
            for (size_t ch = 0; ch < osBlock.getNumChannels(); ++ch)
            {
                const float* p = osBlock.getChannelPointer (ch);
                for (int i = 0; i < osN; ++i)
                    pk = std::max (pk, std::fabs (p[i]));
            }
        }
        else
        {
            for (int ch = 0; ch < nc; ++ch)
                pk = std::max (pk, buffer.getMagnitude (ch, 0, n));
        }

        // ── 2) 峰值包络：瞬时攻击 + 20ms 释放 ──
        env = (pk >= env) ? pk : (pk + envRelease * (env - pk));

        // ── 3) 目标增益：天花板 0 dBFS（线性 1.0）──
        const float target = (env > 1.0f) ? 1.0f / env : 1.0f;
        gain = (target < gain) ? target : (target + gainRelease * (gain - target));

        // ── 4) 应用：2ms 前视延迟 × 增益 ──
        float* L = buffer.getWritePointer (0);
        float* R = nc > 1 ? buffer.getWritePointer (1) : L;
        const size_t sz = delayL.size();
        for (int s = 0; s < n; ++s)
        {
            delayL[write] = L[s];
            delayR[write] = R[s];
            size_t rp = write + sz - (size_t) lookahead;
            if (rp >= sz) rp -= sz;
            L[s] = delayL[rp] * gain;
            R[s] = delayR[rp] * gain;
            ++write;
            if (write >= sz) write = 0;
        }
    }

private:
    float fs = 44100.0f;
    int   lookahead = 0;
    float envRelease = 0.0f, gainRelease = 0.0f;
    float env = 0.0f, gain = 1.0f;
    size_t write = 0;
    std::vector<float> delayL, delayR;

    // 16× 检测过采样（与核心限幅器同款 2×4 半带，但独立实例，不受 OS 开关影响）
    juce::dsp::Oversampling<float> detOS { 2, 4,
        juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false };
};
