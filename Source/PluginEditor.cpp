#include "PluginEditor.h"

IridiumAudioProcessorEditor::IridiumAudioProcessorEditor (IridiumAudioProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    auto* data = reinterpret_cast<const std::byte*> (BinaryData::IridiumUI_html);
    htmlBytes.assign (data, data + BinaryData::IridiumUI_htmlSize);

    wv = std::make_unique<juce::WebBrowserComponent> (
        juce::WebBrowserComponent::Options {}
            .withBackend (juce::WebBrowserComponent::Options::Backend::webview2)
            .withWinWebView2Options (juce::WebBrowserComponent::Options::WinWebView2 {}
                .withUserDataFolder (juce::File::getSpecialLocation (juce::File::tempDirectory)))
            .withResourceProvider ([this] (const juce::String& url) -> std::optional<juce::WebBrowserComponent::Resource> {
                if (url == "/" || url.endsWith ("/") || url.contains ("index.html"))
                {
                    juce::String html = juce::String::fromUTF8 (reinterpret_cast<const char*> (htmlBytes.data()),
                                                                (size_t) htmlBytes.size());
                    html = html.replace ("__KZOOM__", juce::String (proc.uiZoom, 3));   // 初始缩放（首帧即正确，无闪烁）
                    std::vector<std::byte> bytes ((size_t) html.getNumBytesAsUTF8());
                    std::memcpy (bytes.data(), html.toUTF8(), bytes.size());
                    return juce::WebBrowserComponent::Resource { std::move (bytes), juce::String ("text/html") };
                }
                return {};
            })
            .withEventListener (juce::Identifier ("iridiumReady"), [this] (const juce::var&) {
                hideLoading();
            })
    );
    addAndMakeVisible (wv.get());

    // 启动黑屏期盖牌，页面发出 iridiumReady 后消失
    loadingOverlay = std::make_unique<IridiumLoadingOverlay>();
    addAndMakeVisible (loadingOverlay.get());
    loadingOverlay->setBounds (getLocalBounds());

    setSize (juce::roundToInt (760.0f * proc.uiZoom), juce::roundToInt (500.0f * proc.uiZoom));
    startTimerHz (30);
}

void IridiumAudioProcessorEditor::resized()
{
    wv->setBounds (getLocalBounds());
    if (loadingOverlay != nullptr)
        loadingOverlay->setBounds (getLocalBounds());
}

void IridiumAudioProcessorEditor::timerCallback()
{
    // 安全兜底：绝不让盖牌卡死（例如 WebView2 加载失败）
    if (loadingOverlay != nullptr && loadingOverlay->isVisible() && ++loadingTicks > 30 * 8)
        hideLoading();

    if (loadDelay > 0)
    {
        if (--loadDelay == 0)
        {
            wv->goToURL (juce::WebBrowserComponent::getResourceProviderRoot());
            initSyncDelay = 8;
        }
        return;
    }

    if (initSyncDelay > 0)
    {
        if (--initSyncDelay == 0)
            pushParams();
        return;
    }

    // 周期全量同步参数（反映宿主自动化 / 外部改动），同步帧跳过轮询防回弹
    if (++syncTick >= 18)
    {
        syncTick = 0;
        pushParams();
        return;
    }

    // ── 正常运行：表头每帧推 + 轮询 JS 读回参数改动 ──
    pushMeters();

    wv->evaluateJavascript ("JSON.stringify(window._S)",
        [this] (const juce::WebBrowserComponent::EvaluationResult& r) {
            auto* vp = r.getResult();
            if (vp == nullptr) return;
            auto raw = vp->toString();
            if (raw.isEmpty()) return;
            hideLoading();   // 页面已活，撤掉盖牌（事件之外的兜底）
            auto v = juce::JSON::parse (raw);
            if (v.isVoid() || !v.isObject()) return;
            juce::MessageManager::callAsync ([this, v] {
                auto safeV = [] (const juce::var& x) -> float {
                    auto d = (double) x;
                    return std::isfinite (d) ? (float) d : 0.0f;
                };
                auto set = [&] (const char* id, float val) {
                    auto* x = proc.apvts.getParameter (id);
                    if (x == nullptr) return;
                    float nrm = x->convertTo0to1 (val);
                    if (std::abs (x->getValue() - nrm) > 0.0001f)   // 0.01dB 步进需更细阈值（0.01/48≈0.0002）
                        x->setValueNotifyingHost (nrm);
                };
                auto setBool = [&] (const char* id, bool b) {
                    auto* x = proc.apvts.getParameter (id);
                    if (x == nullptr) return;
                    float target = b ? 1.0f : 0.0f;
                    if (std::abs (x->getValue() - target) > 0.001f)
                        x->setValueNotifyingHost (target);
                };
                if (v.hasProperty ("inputGain")) { lastInputGain = safeV (v["inputGain"]); set (ParamIDs::inputGain, lastInputGain); }
                if (v.hasProperty ("output"))    { lastOutput    = safeV (v["output"]);    set (ParamIDs::output,    lastOutput); }
                if (v.hasProperty ("fc"))        setBool (ParamIDs::fc,    (int) v["fc"] != 0);
                if (v.hasProperty ("os"))        setBool (ParamIDs::os,    (int) v["os"] != 0);
                if (v.hasProperty ("link"))      setBool (ParamIDs::link,  (int) v["link"] != 0);
                if (v.hasProperty ("zoom")) {
                    float z = safeV (v["zoom"]);
                    if (std::isfinite (z) && std::abs (z - proc.uiZoom) > 0.005f) {
                        proc.uiZoom = juce::jlimit (0.7f, 2.0f, z);
                        setSize (juce::roundToInt (760.0f * proc.uiZoom), juce::roundToInt (500.0f * proc.uiZoom));
                    }
                }
                if (v.hasProperty ("_openLink")) {
                    int c = (int) v["_openLink"];
                    if (c != lastOpenCount) {
                        lastOpenCount = c;
                        juce::URL ("https://space.bilibili.com/3493259676486408?spm_id_from=333.1007.0.0").launchInDefaultBrowser();
                    }
                }
            });
        }
    );
}

void IridiumAudioProcessorEditor::pushParams()
{
    auto pv = [&] (const char* id) -> float {
        auto* p = proc.apvts.getRawParameterValue (id);
        return p != nullptr ? p->load() : 0.0f;
    };
    auto on = [&] (const char* id) -> const char* {
        return pv (id) > 0.5f ? "1" : "0";
    };
    // 只在宿主自动化改动参数时才回写数值，避免覆盖用户滚轮/拖拽刚写入的 0.01 步进
    const float inVal  = pv (ParamIDs::inputGain);
    const float outVal = pv (ParamIDs::output);
    const bool  syncIn  = std::abs (inVal  - lastInputGain) > 0.005f;
    const bool  syncOut = std::abs (outVal - lastOutput)    > 0.005f;
    if (syncIn)  lastInputGain = inVal;
    if (syncOut) lastOutput    = outVal;

    juce::String js;
    js << "var S=window._S;";
    if (syncIn)  js << "S.inputGain=" << juce::String (inVal,  2) << ";";
    if (syncOut) js << "S.output="    << juce::String (outVal, 2) << ";";
    js << "S.fc="   << on (ParamIDs::fc)   << ";"
       << "S.os="   << on (ParamIDs::os)   << ";"
       << "S.link=" << on (ParamIDs::link) << ";"
       << "S.zoom=" << juce::String (proc.uiZoom, 3) << ";"
       << "window._render()";
    wv->evaluateJavascript (js);
}

void IridiumAudioProcessorEditor::pushMeters()
{
    auto safe = [] (float v) { return std::isfinite (v) ? v : -60.0f; };
    juce::String js;
    js << "var S=window._S;"
       << "S.inPeak="  << juce::String (safe (proc.getInputPeakDB()),  1) << ";"
       << "S.outPeak=" << juce::String (safe (proc.getOutputPeakDB()), 1) << ";"
       << "S.gr="      << juce::String (safe (std::abs (proc.getGainReductionDB())), 1) << ";"
       << "S.lufs="    << juce::String (safe (proc.getShortLUFS()), 1) << ";"
       << "window._render()";
    wv->evaluateJavascript (js);
}

void IridiumAudioProcessorEditor::hideLoading()
{
    if (loadingOverlay != nullptr && loadingOverlay->isVisible())
        loadingOverlay->dismiss();
}
