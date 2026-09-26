#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <cmath>
#include <cstring>
#include "PluginProcessor.h"
#include "BinaryData.h"

// ─── Loading splash：WebView2 启动黑屏期间显示的铱元素牌 ────────────────────
// 以铂银 + 虹彩的 77 号元素 Ir 卡片盖住黑屏期，页面发出 `iridiumReady` 后消失。
class IridiumLoadingOverlay final : public juce::Component, private juce::Timer
{
public:
    IridiumLoadingOverlay()
    {
        setOpaque (true);
        setInterceptsMouseClicks (false, false);
        startTimerHz (30);
    }

    void dismiss()
    {
        stopTimer();
        setVisible (false);
    }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        g.fillAll (juce::Colour (0xff0a0b10));       // 对齐 HTML --bg

        const float pulse = 0.5f + 0.5f * std::sin (phase * 0.12f);
        const auto silver = juce::Colour (0xffc8d2e0);   // 铂银
        const auto irid   = juce::Colour (0xff9fb8d8);   // 虹彩蓝
        const auto dim    = juce::Colour (0xff9aa0b4);
        const auto white  = juce::Colour (0xfff4f6fb);

        const float cx = area.getCentreX();
        const float cy = area.getCentreY() - 4.0f;

        // 环境光晕（虹彩蓝）
        {
            juce::ColourGradient glow (irid.withAlpha (0.20f + 0.16f * pulse), cx, cy,
                                       irid.withAlpha (0.0f), cx, cy + area.getHeight() * 0.45f, true);
            g.setGradientFill (glow);
            g.fillEllipse (cx - 240.0f, cy - 240.0f, 480.0f, 480.0f);
        }

        // 元素牌
        const float tx = cx - 64.0f, ty = cy - 92.0f, tw = 128.0f, th = 184.0f;
        g.setColour (juce::Colour (0xff1b1d24));     // --panel
        g.fillRoundedRectangle (tx, ty, tw, th, 10.0f);
        g.setColour (silver.withAlpha (0.9f));
        g.drawRoundedRectangle (tx, ty, tw, th, 10.0f, 1.6f);

        // 原子序数 77
        g.setColour (dim);
        g.setFont (juce::Font (13.0f, juce::Font::plain));
        g.drawText ("77", juce::Rectangle<float> (tx + 10.0f, ty + 6.0f, tw - 20.0f, 20.0f),
                    juce::Justification::topLeft, false);

        // 符号 Ir
        g.setColour (silver);
        g.setFont (juce::Font (88.0f, juce::Font::plain));
        g.drawText ("Ir", juce::Rectangle<float> (tx, ty + 22.0f, tw, 106.0f),
                    juce::Justification::centred, false);

        // 名称 + 原子量
        g.setColour (dim);
        g.setFont (juce::Font (9.0f, juce::Font::plain));
        g.drawText ("IRIDIUM", juce::Rectangle<float> (tx, ty + 128.0f, tw, 14.0f),
                    juce::Justification::centred, false);
        g.setFont (juce::Font (10.0f, juce::Font::plain));
        g.drawText ("192.217", juce::Rectangle<float> (tx, ty + 144.0f, tw, 16.0f),
                    juce::Justification::centred, false);

        // 牌底虹彩光晕（铱盐虹彩色）
        {
            auto sheen = juce::Rectangle<float> (cx - 46.0f, cy + 92.0f, 92.0f, 18.0f);
            juce::ColourGradient sg (irid.withAlpha (0.42f + 0.26f * pulse), sheen.getCentreX(), sheen.getCentreY(),
                                     irid.withAlpha (0.0f), sheen.getCentreX(), sheen.getY() + sheen.getHeight() * 2.4f, false);
            g.setGradientFill (sg);
            g.fillEllipse (sheen);
        }

        // LOADING 文本
        const int dots = 1 + (int)(pulse * 3.0f);
        g.setColour (white.withAlpha (0.8f));
        g.setFont (juce::Font (11.0f, juce::Font::plain));
        g.drawText ("LOADING" + juce::String::repeatedString (".", dots),
                    juce::Rectangle<float> (cx - 120.0f, cy + 132.0f, 240.0f, 18.0f),
                    juce::Justification::centred, false);

        // 品牌
        g.setColour (dim);
        g.setFont (juce::Font (9.5f, juce::Font::plain));
        g.drawText ("IRIDIUM  ·  GAIN-FIELD LIMITER",
                    juce::Rectangle<float> (0.0f, area.getBottom() - 24.0f, area.getWidth(), 12.0f),
                    juce::Justification::centred, false);
    }

private:
    void timerCallback() override
    {
        ++phase;
        repaint();
    }

    int phase = 0;
};

class IridiumAudioProcessorEditor final : public juce::AudioProcessorEditor
                                        , private juce::Timer
{
public:
    explicit IridiumAudioProcessorEditor (IridiumAudioProcessor& p);
    ~IridiumAudioProcessorEditor() override = default;

    void resized() override;
    void timerCallback() override;

private:
    IridiumAudioProcessor& proc;
    std::unique_ptr<juce::WebBrowserComponent> wv;
    std::unique_ptr<IridiumLoadingOverlay> loadingOverlay;
    std::vector<std::byte> htmlBytes;
    int loadDelay = 10;
    int loadingTicks = 0;
    int initSyncDelay = 0;
    int syncTick = 0;
    int lastOpenCount = 0;   // DJAnta 跳转去重

    void pushParams();   // 参数 → JS（初始 + 周期，反映宿主自动化）
    void pushMeters();   // 表头 → JS（每帧）
    void hideLoading();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IridiumAudioProcessorEditor)
};
