#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include "dsp/IridiumLimiter.h"

class IridiumAudioProcessorEditor;

// ─── Parameter IDs ──────────────────────────────────────────────────────────
namespace ParamIDs
{
    inline constexpr const char* inputGain = "inputGain";
    inline constexpr const char* output    = "output";
    inline constexpr const char* reduction = "reduction";
}

// ─── Plugin Processor ───────────────────────────────────────────────────────
class IridiumAudioProcessor final : public juce::AudioProcessor
{
public:
    IridiumAudioProcessor()
        : AudioProcessor (BusesProperties().withInput  ("Stereo In",  juce::AudioChannelSet::stereo())
                                             .withOutput ("Stereo Out", juce::AudioChannelSet::stereo())),
          apvts (*this, nullptr, "Parameters", createParameterLayout())
    {
    }

    // ── Parameters ──────────────────────────────────────────────────────────

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
    {
        using namespace juce;
        AudioProcessorValueTreeState::ParameterLayout layout;

        layout.add (std::make_unique<AudioParameterFloat> (ParamIDs::inputGain, "Input",
                                                           NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 0.0f,
                                                           AudioParameterFloatAttributes{}
                                                               .withLabel ("dB")
                                                               .withCategory (AudioProcessorParameter::inputGain)));

        layout.add (std::make_unique<AudioParameterFloat> (ParamIDs::output, "Output",
                                                           NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 0.0f,
                                                           AudioParameterFloatAttributes{}
                                                               .withLabel ("dB")
                                                               .withCategory (AudioProcessorParameter::outputGain)));

        layout.add (std::make_unique<AudioParameterFloat> (ParamIDs::reduction, "Reduction",
                                                           NormalisableRange<float> (0.0f, 60.0f, 0.1f), 0.0f,
                                                           AudioParameterFloatAttributes{}
                                                               .withLabel ("dB")
                                                               .withCategory (AudioProcessorParameter::inputMeter)
                                                               .withAutomatable (false)));

        return layout;
    }

    // ── Audio processing ────────────────────────────────────────────────────

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override;

    // ── Editor（v0.0.1：自定义简单 GUI）────────────────────────────────────
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    // ── State ───────────────────────────────────────────────────────────────
    void getStateInformation (juce::MemoryBlock& destData) override
    {
        auto root = std::make_unique<juce::XmlElement> ("Iridium");
        root->setAttribute ("uiZoom", juce::String (uiZoom, 3));
        root->addChildElement (apvts.copyState().createXml().release());
        copyXmlToBinary (*root, destData);
    }
    void setStateInformation (const void* data, int sizeInBytes) override
    {
        auto xml = getXmlFromBinary (data, sizeInBytes);
        if (xml == nullptr) return;
        if (xml->hasTagName ("Iridium"))
        {
            uiZoom = (float) xml->getDoubleAttribute ("uiZoom", 1.0);
            if (auto* child = xml->getFirstChildElement())
                apvts.replaceState (juce::ValueTree::fromXml (*child));
        }
        else
        {
            apvts.replaceState (juce::ValueTree::fromXml (*xml));   // 兼容旧格式（纯 apvts 状态）
        }
    }

    // ── Identity ────────────────────────────────────────────────────────────
    const juce::String getName() const override { return "Iridium"; }
    bool acceptsMidi() const override    { return false; }
    bool producesMidi() const override   { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    // 供编辑器读取当前增益衰减（dB，≤0）
    float getGainReductionDB() const { return limiter.getGainReductionDB(); }

    // 供编辑器读取输入 / 输出峰值电平（dB，表头显示）
    float getInputPeakDB()  const { return inPeakDB; }
    float getOutputPeakDB() const { return outPeakDB; }

    // UI 缩放（1.0 = 基准 760×500；随宿主状态持久化）
    float uiZoom = 1.0f;

    // ── Programs（AudioProcessor 的纯虚函数，单默认程序）────────────────────
    int getNumPrograms() override                          { return 1; }
    int getCurrentProgram() override                       { return 0; }
    void setCurrentProgram (int) override                  {}
    const juce::String getProgramName (int) override       { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}

    juce::AudioProcessorValueTreeState apvts;

private:
    // 16× 过采样（factor=4 → 2^4=16）：抗混叠，非真峰值检测
    juce::dsp::Oversampling<float> oversampling { 2, 4,
        juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false };
    IridiumLimiter limiter;

    juce::AudioParameterFloat* inputGainParam = nullptr;
    juce::AudioParameterFloat* outputParam    = nullptr;
    juce::AudioParameterFloat* reductionParam = nullptr;

    double baseSampleRate = 48000.0;

    float inPeakDB  = -60.0f;
    float outPeakDB = -60.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IridiumAudioProcessor)
};
