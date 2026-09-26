#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::AudioProcessorEditor* IridiumAudioProcessor::createEditor()
{
    return new IridiumAudioProcessorEditor (*this);
}

void IridiumAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    baseSampleRate = sampleRate;

    juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) samplesPerBlock, 2 };
    oversampling.initProcessing (spec.maximumBlockSize);
    oversampling.reset();

    const double osFactor = (double) oversampling.getOversamplingFactor();  // 16
    const double osRate   = sampleRate * osFactor;
    const int    maxOsBlock = (int) ((juce::int64) samplesPerBlock * (juce::int64) osFactor);

    limiter.prepare (osRate, maxOsBlock, 50.0);  // 50ms 前视缓冲（覆盖两个半周期）

    // 总延迟 = 过采样滤波器延迟 + 限幅器前视（过采样样本 ÷ 16）
    const int osLat   = (int) oversampling.getLatencyInSamples();
    const int lookLat = limiter.getLatencySamples() / (int) osFactor;
    setLatencySamples (osLat + lookLat);

    inputGainParam = dynamic_cast<juce::AudioParameterFloat*> (apvts.getParameter (ParamIDs::inputGain));
    outputParam    = dynamic_cast<juce::AudioParameterFloat*> (apvts.getParameter (ParamIDs::output));
    reductionParam = dynamic_cast<juce::AudioParameterFloat*> (apvts.getParameter (ParamIDs::reduction));
    // Ceiling 固定 0 dB：限幅器内部目标恒定；Output 是末级 trim（后置增益）。
}

void IridiumAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    if (inputGainParam == nullptr || outputParam == nullptr || reductionParam == nullptr)
        return;  // 尚未 prepare

    // 1) 输入增益
    const float inputGainLin = std::pow (10.0f, inputGainParam->get() / 20.0f);
    buffer.applyGain (inputGainLin);

    // 输入峰值表（进限幅器前）
    {
        float pk = 0.0f;
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            pk = std::max (pk, buffer.getMagnitude (ch, 0, buffer.getNumSamples()));
        inPeakDB = 20.0f * std::log10 (pk + 1e-9f);
    }

    // 2) 16× 过采样（抗混叠）
    auto block   = juce::dsp::AudioBlock<float> (buffer);
    auto osBlock = oversampling.processSamplesUp (block);

    // 3) 限幅器（过采样域，分段恒定增益场，无 attack/release；Ceiling 固定 0 dB）
    limiter.process (osBlock);

    // 4) 降采样回基频
    oversampling.processSamplesDown (block);

    // 5) 末级安全限幅（v0.0.3）：软拐角替代硬切，更高质量、更温和。
    //    |x| ≤ ceiling 完全透明（合法峰值零干扰 → 比硬切更不「过」）；
    //    |x| > ceiling 柔和压回，渐近 ceiling + headroom（headroom≈0.013dB，软饱和、无硬切拐角）。
    //    每样本一次 exp，换取更少硬切失真（舍弃一点点性能）。
    const float ceilLin  = 1.0f;               // Ceiling 固定 0 dB
    const float headroom = 0.0015f * ceilLin;  // ~0.013 dB 残余余量（v0.0.7 微峰值再收一点点）
    const float tau      = 0.22f  * ceilLin;   // 软饱和尺度（v0.0.7 斜率更陡）
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        float* d = buffer.getWritePointer (ch);
        for (int s = 0; s < buffer.getNumSamples(); ++s)
        {
            const float x = d[s];
            const float a = std::fabs (x);
            if (a > ceilLin)
            {
                const float sign = (x > 0.0f) ? 1.0f : -1.0f;
                const float excess = a - ceilLin;
                d[s] = sign * (ceilLin + headroom * (1.0f - std::exp (-excess / tau)));
            }
        }
    }

    // 5.5) Output trim：末级后置增益（纯 trim，非 ceiling；Ceiling 固定 0 dB）
    const float outputLin = std::pow (10.0f, outputParam->get() / 20.0f);
    buffer.applyGain (outputLin);

    // 输出峰值表
    {
        float pk = 0.0f;
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            pk = std::max (pk, buffer.getMagnitude (ch, 0, buffer.getNumSamples()));
        outPeakDB = 20.0f * std::log10 (pk + 1e-9f);
    }

    // 6) 增益衰减表
    const float grDB     = limiter.getGainReductionDB();          // ≤ 0 dB
    const float grAmount = juce::jlimit (0.0f, 60.0f, -grDB);     // 正 dB = 衰减量
    reductionParam->setValueNotifyingHost (reductionParam->convertTo0to1 (grAmount));
}
