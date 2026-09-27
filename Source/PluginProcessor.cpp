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

    // 短期响度 LUFS：K加权（BS.1770 48kHz 标准系数）+ 400ms 均方块 + 3s 滑窗
    {
        using C = juce::dsp::IIR::Coefficients<float>;
        // 阶1 高频搁架（+4dB @>1.5kHz）与 阶2 高通（@38Hz）——48kHz 系数（其它采样率近似）
        k1L.coefficients = new C (1.53512485958697f, -2.69169618940638f, 1.19839281085285f,
                                  1.0f, -1.69065929318241f, 0.73248077421585f);
        k1R.coefficients = k1L.coefficients;
        k2L.coefficients = new C (1.0f, -2.0f, 1.0f,
                                  1.0f, -1.99004745483398f, 0.99007225036621f);
        k2R.coefficients = k2L.coefficients;
        juce::dsp::ProcessSpec specM { sampleRate, (juce::uint32) samplesPerBlock, 1 };
        k1L.prepare (specM); k1R.prepare (specM); k2L.prepare (specM); k2R.prepare (specM);
        lufsBlockLen = std::max (1, (int) std::llround (0.4 * sampleRate));
        lufsBlockCnt = 0;
        lufsBlockAcc = 0.0;
        lufsBlocks.clear();
        shortLufs = -70.0f;
    }

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

    // 3) 末级软拐角限幅（逐样本 waveshape：硬顶在 Ceiling；FC 关 headroom 放大 → 更开放）
    //    FC 开 headroom=0 → 精确硬顶 0 dB；FC 关 headroom=0.5（≈+3.5dB）→ 软拐角、峰值放出。
    const float ceilLin  = 1.0f;                          // Ceiling 固定 0 dB
    const float headroom = fcOn ? 0.0f  : 0.5f;           // FC 开=硬顶；FC 关=余量放大
    const float tau      = fcOn ? 0.05f : 0.5f;           // FC 开更陡；FC 关更软
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

    // 6) 短期响度 LUFS（输出信号：K加权 → 400ms 均方块 → 3s 滑窗 → LUFS）
    {
        float* lch = buffer.getWritePointer (0);
        float* rch = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : lch;
        for (int n = 0; n < buffer.getNumSamples(); ++n)
        {
            const float yl = k2L.processSample (k1L.processSample (lch[n]));
            const float yr = k2R.processSample (k1R.processSample (rch[n]));
            lufsBlockAcc += 0.5 * ((double) yl * yl + (double) yr * yr);   // L+R 声道均值
            if (++lufsBlockCnt >= lufsBlockLen)
            {
                lufsBlocks.push_back (lufsBlockAcc / (double) lufsBlockCnt);
                if ((int) lufsBlocks.size() > 8) lufsBlocks.pop_front();    // 8×400ms ≈ 3.2s
                double sum = 0.0;
                for (double b : lufsBlocks) sum += b;
                shortLufs = (float) (-0.691 + 10.0 * std::log10 (sum / (double) lufsBlocks.size() + 1e-12));
                lufsBlockAcc = 0.0;
                lufsBlockCnt = 0;
            }
        }
    }
}
