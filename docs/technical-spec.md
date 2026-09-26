# Iridium — 技术规范

## 框架
- JUCE 8.0.14（`C:/H/Program Files/juce-8.0.14-windows/JUCE`）
- C++20
- VST3 only，Stereo
- UI：WebView2 加载 HTML（`Source/ui/IridiumUI.html`）

## 构建
```bash
cmake -G "Visual Studio 18 2026" -DJUCE_ROOT="C:/H/Program Files/juce-8.0.14-windows/JUCE" /c/IridiumProj
cmake --build . --config Release
```
- 中文路径 bug 规避：源码在英文副本 `/c/IridiumProj/` 编译
- AVX2 可选：`-DIRIDIUM_AVX2=ON`；默认 SSE2 兼容

## 编译架构（复用 Potassium）
- `juce_add_plugin(...)`：COMPANY "Ium Audio"、厂商码 "IumA"、插件码待定（建议 "Iri1"）
- UI：`juce_add_binary_data` + WebView2 静态链接
- 优化：MSVC `/O2 /fp:fast`；AVX2 时 `/arch:AVX2`

## 参数列表（待算法设计文档确定后补全）
| ID | 名称 | 范围 | 默认 | 说明 |
|----|------|------|------|------|
| ceiling | Ceiling | 待定 | 待定 | 输出上限（目标阈值） |
| （后续补充） | | | | |

## DSP 模块文件（规划）
```
Source/dsp/
  OversamplingStage.h  ← 16x 过采样
  ZeroCrossingStage.h  ← 过零检测（过采样域）
  ExtremumStage.h      ← 局部极值检测
  GainFieldStage.h     ← 核心：前视增益场构造器
  ...
```

## 已知技术陷阱（来自 Potassium 实战）
1. JUCE `juce::dsp::Oversampling` 的 factor 是**指数**（2^factor），16x 过采样需 factor=4
2. 限幅/增益类处理有恒定延迟必须上报 PDC（`getLatencySamples()`）
3. 增益阶跃会造成咔嗒声，需在过零点锚定 + 平滑
4. 过零检测必须基于过采样后的波形（16x），否则零点不精确
