# CLAUDE.md — Iridium 项目

## 项目概述
基于「局部极值约束与零点锚定的前视增益场构造器」的新式 limiter（限幅器）VST3 插件。
JUCE 8.0.14 + C++20，复用 Potassium（上一个插件）的完整编译架构（CMake + WebView2 UI）。

核心区别（相对 waveshaper）：输出 y = f(x) · g(t)，g(t) 是随时间和上下文连续绘制的增益场，由局部极值与过零点共同构造。

## 标准文件路径
- 项目需求: `docs/requirements.md`
- 技术规范: `docs/technical-spec.md`
- 执行计划: `docs/development-plan.md`
- 算法设计文档（首个里程碑交付物）: `docs/algorithm-design.md`
- 开发日志: `devlog/YYYY-MM-DD.md`

## 构建命令
```bash
# 中文路径 bug 规避：源码同步到纯英文副本 /c/IridiumProj/ 再编译
cmake -G "Visual Studio 18 2026" -DJUCE_ROOT="C:/H/Program Files/juce-8.0.14-windows/JUCE" /c/IridiumProj
cmake --build . --config Release
```
- VST3 输出: `/c/IridiumBuild/Iridium_artefacts/Release/VST3/Iridium.vst3`
- 部署: `C:/Program Files/Common Files/VST3/`

## 源文件（规划）
- 入口: `Source/createPluginFilter.cpp`
- 主处理器: `Source/PluginProcessor.{h,cpp}`
- 编辑器: `Source/PluginEditor.{h,cpp}`
- DSP 模块: `Source/dsp/*.h`
- UI: `Source/ui/IridiumUI.html`

## 工作原则
1. 稳步推进：一个里程碑一个里程碑来，先出算法设计文档，用户确认后才写代码
2. 中文路径 bug：源码改完同步到 `/c/IridiumProj/Source/` 再编译
3. 编译前关闭 DAW / PluginDoctor（文件锁）
4. 每天结束时自动更新 `devlog/YYYY-MM-DD.md`（已完成 + 待办）
5. 不主动 push / 上传，等用户命令
