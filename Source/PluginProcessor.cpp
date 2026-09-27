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

    limiter.prepare (osRate, maxOsBlock, 50.0);              // OS 开：16× 域
    limiterBase.prepare (sampleRate, samplesPerBlock, 50.0); // OS 关：基频域

    // 延迟 = 50ms 前视（两模式相同，基频样本）+ 过采样滤波器延迟（仅 OS 开）
    osLatency       = (int) oversampling.getLatencyInSamples();
    baseLookLatency = limiter.getLatencySamples() / (int) osFactor;

    const bool osOn = osParam != nullptr && osParam->get();
    lastOSState = osOn;
    setLatencySamples (osOn ? (osLatency + baseLookLatency) : baseLookLatency);
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
        setLatencySamples (osOn ? (osLatency + baseLookLatency) : baseLookLatency);
    }

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

    // 2) 限幅器（分段前视增益场；Ceiling 固定 0 dB；OS 开关决定是否 16× 过采样）
    IridiumLimiter& activeLimiter = osOn ? limiter : limiterBase;
    {
        auto block = juce::dsp::AudioBlock<float> (buffer);
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

    // 3) 末级安全限幅（FC 开关控制 headroom）：
    //    FC 开（默认）headroom≈0（0.0001，听感 0dB 余量）——陡峭平滑拐角替代硬切，THD 更低；
    //    FC 关 headroom 调大（0.01，~0.087dB）——软拐角、更温和。
    const float ceilLin  = 1.0f;                          // Ceiling 固定 0 dB
    const float headroom = fcOn ? 0.0001f : 0.01f;        // FC 开≈0；FC 关=调大
    const float tau      = fcOn ? 0.05f   : 0.22f;        // FC 开更陡（平滑拐角）；FC 关更软
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        float* d = buffer.getWritePointer (ch);
        for (int s = 0; s < buffer.getNumSamples(); ++s)
        {
            const float x = d[s];
            const float a = std::fabs (x);
            if (a > ceilLin)
            {
                const float sign   = (x > 0.0f) ? 1.0f : -1.0f;
                const float excess = a - ceilLin;
                d[s] = sign * (ceilLin + headroom * (1.0f - std::exp (-excess / tau)));
            }
        }
    }

    // 4) Output trim（末级后置增益；Link 开时 = −Input，即推起 Input 同时拉低 Output）
    const float inDB      = inputGainParam->get();
    const float outDB     = linkOn ? -inDB : outputParam->get();
    const float outputLin = std::pow (10.0f, outDB / 20.0f);
    buffer.applyGain (outputLin);

    // 输出峰值表
    {
        float pk = 0.0f;
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            pk = std::max (pk, buffer.getMagnitude (ch, 0, buffer.getNumSamples()));
        outPeakDB = 20.0f * std::log10 (pk + 1e-9f);
    }

    // 5) 增益衰减表（活跃限幅器）
    const float grDB     = activeLimiter.getGainReductionDB();     // ≤ 0 dB
    lastGRDB             = grDB;
    const float grAmount = juce::jlimit (0.0f, 60.0f, -grDB);      // 正 dB = 衰减量
    reductionParam->setValueNotifyingHost (reductionParam->convertTo0to1 (grAmount));
}
