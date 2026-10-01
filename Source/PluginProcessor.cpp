#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::AudioProcessorEditor* IridiumAudioProcessor::createEditor()
{
    return new IridiumAudioProcessorEditor (*this);
}

void IridiumAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    baseSampleRate = sampleRate;

    // 参数指针（先拿，供延迟计算用）
    inputGainParam = dynamic_cast<juce::AudioParameterFloat*> (apvts.getParameter (ParamIDs::inputGain));
    outputParam    = dynamic_cast<juce::AudioParameterFloat*> (apvts.getParameter (ParamIDs::output));
    reductionParam = dynamic_cast<juce::AudioParameterFloat*> (apvts.getParameter (ParamIDs::reduction));
    fcParam        = dynamic_cast<juce::AudioParameterBool*>  (apvts.getParameter (ParamIDs::fc));
    osParam        = dynamic_cast<juce::AudioParameterBool*>  (apvts.getParameter (ParamIDs::os));
    linkParam      = dynamic_cast<juce::AudioParameterBool*>  (apvts.getParameter (ParamIDs::link));

    juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) samplesPerBlock, 2 };
    oversampling.initProcessing (spec.maximumBlockSize);
    oversampling.reset();

    const double osFactor = (double) oversampling.getOversamplingFactor();  // 16
    const double osRate   = sampleRate * osFactor;
    const int    maxOsBlock = (int) ((juce::int64) samplesPerBlock * (juce::int64) osFactor);

    limiter.prepare (osRate, maxOsBlock, 50.0);              // OS 开：16× 域（前视 50ms）
    limiterBase.prepare (sampleRate, samplesPerBlock, 50.0); // OS 关：基频域（前视 50ms）
    finalCeiling.prepare (sampleRate, samplesPerBlock);      // Final Ceiling：16× 前视平滑增益 + tanh 软顶
    loudness.prepare (sampleRate);                           // LUFS K加权按宿主采样率重算

    // 延迟 = 50ms 前视（基频样本）+ OS 过采样滤波器延迟（仅 OS 开）+ Final Ceiling（16× 滤波 + 2ms 前视，恒定）
    osLatency       = (int) oversampling.getLatencyInSamples();
    baseLookLatency = limiter.getLatencySamples() / (int) osFactor;
    fcLatency       = finalCeiling.getLatencySamples();

    const bool osOn = osParam != nullptr && osParam->get();
    lastOSState = osOn;
    setLatencySamples ((osOn ? (osLatency + baseLookLatency) : baseLookLatency) + fcLatency);
    // Ceiling 固定 0 dB：限幅器内部目标恒定；Output 是末级 trim（后置增益）。
}

void IridiumAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    if (inputGainParam == nullptr || outputParam == nullptr || reductionParam == nullptr)
        return;  // 尚未 prepare

    const bool osOn   = osParam   != nullptr && osParam->get();
    const bool fcOn   = fcParam   != nullptr && fcParam->get();
    const bool linkOn = linkParam != nullptr && linkParam->get();

    // OS 切换 → 更新延迟上报（宿主需重同步；稳定后建议固定不切）
    if (osOn != lastOSState)
    {
        lastOSState = osOn;
        setLatencySamples ((osOn ? (osLatency + baseLookLatency) : baseLookLatency) + fcLatency);
    }

    // float → double（压低量化噪声底）
    const int nch = buffer.getNumChannels();
    const int ns  = buffer.getNumSamples();
    dBuffer.setSize (nch, ns, false, false, true);
    for (int ch = 0; ch < nch; ++ch)
    {
        const float* src = buffer.getReadPointer (ch);
        double*      dst = dBuffer.getWritePointer (ch);
        for (int i = 0; i < ns; ++i) dst[i] = (double) src[i];
    }

    // 1) 输入增益
    const double inputGainLin = std::pow (10.0, inputGainParam->get() / 20.0);
    dBuffer.applyGain (inputGainLin);

    // 输入峰值表（进限幅器前）
    {
        double pk = 0.0;
        for (int ch = 0; ch < nch; ++ch)
            pk = std::max (pk, dBuffer.getMagnitude (ch, 0, ns));
        inPeakDB = (float) (20.0 * std::log10 (pk + 1e-9));
    }

    // 2) 限幅器（分段前视增益场；Ceiling 固定 0 dB；OS 开关决定是否 16× 过采样）
    IridiumLimiter& activeLimiter = osOn ? limiter : limiterBase;
    {
        auto block = juce::dsp::AudioBlock<double> (dBuffer);
        if (osOn)
        {
            auto osBlock = oversampling.processSamplesUp (block);
            activeLimiter.process (osBlock);
            oversampling.processSamplesDown (block);
        }
        else
        {
            activeLimiter.process (block);
        }
    }

    // 3) Output trim（后置增益；Link 开时 = −Input，即推起 Input 同时拉低 Output）
    const double inDB      = inputGainParam->get();
    const double outDB     = linkOn ? -inDB : outputParam->get();
    const double outputLin = std::pow (10.0, outDB / 20.0);
    dBuffer.applyGain (outputLin);

    // 4) Final Ceiling：16× 前视平滑增益 + tanh 软顶（最终天花板；FC 开压顶 / 关透明）
    finalCeiling.process (dBuffer, fcOn);

    // double → float（写回宿主 buffer）
    for (int ch = 0; ch < nch; ++ch)
    {
        const double* src = dBuffer.getReadPointer (ch);
        float*        dst = buffer.getWritePointer (ch);
        for (int i = 0; i < ns; ++i) dst[i] = (float) src[i];
    }

    // 输出峰值表
    {
        float pk = 0.0f;
        for (int ch = 0; ch < nch; ++ch)
            pk = std::max (pk, buffer.getMagnitude (ch, 0, ns));
        outPeakDB = 20.0f * std::log10 (pk + 1e-9f);
    }

    // 增益衰减表（活跃限幅器）
    const float grDB     = activeLimiter.getGainReductionDB();     // ≤ 0 dB
    lastGRDB             = grDB;
    const float grAmount = juce::jlimit (0.0f, 60.0f, -grDB);      // 正 dB = 衰减量
    reductionParam->setValueNotifyingHost (reductionParam->convertTo0to1 (grAmount));

    // 短期响度 LUFS（输出信号，K加权按宿主采样率重算）
    {
        const float* lch = buffer.getReadPointer (0);
        const float* rch = buffer.getNumChannels() > 1 ? buffer.getReadPointer (1) : lch;
        loudness.process (lch, rch, ns);
    }
}
