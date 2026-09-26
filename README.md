# Iridium

前视增益场限幅器 · Forward-looking Gain-Field Limiter（VST3）

输出 **y = f(x)·g(t)**：把信号按**过零点**切成「半周期」，每段恒定增益（由局部极值约束），增益**只在过零点切换**（零点锚定，无咔哒），全程**无 Attack / Release 时间常数**。它是一张随时间与上下文连续绘制的增益场，而不是无记忆的 waveshaper。

## 特性

- 分段前视增益场（两段前视，峰值精准贴顶）
- 16× 过采样（抗混叠）
- 末级软拐角安全限幅（软饱和替代硬切，更温和）
- WebView2 界面 · 铱（Ir）元素美学 · 界面尺寸可调
- DJAnta 主页快捷按钮 · K–Pg 边界彩蛋

## 版本更新

完整历史见 [CHANGELOG.md](CHANGELOG.md)；安装包见 [Releases](https://github.com/IumAudio/Iridium-Audio-plugin/releases)。

## 下载与安装

最新安装包见 [Releases](https://github.com/IumAudio/Iridium-Audio-plugin/releases)。

1. 解压 `Iridium-v0.0.7-win-x64.zip`
2. 把 `Iridium.vst3` 整个文件夹复制到 `C:\Program Files\Common Files\VST3\`
3. 打开 DAW（REAPER / Ableton / Cubase / Studio One 等）扫描新插件即可

卸载：删除 `C:\Program Files\Common Files\VST3\Iridium.vst3` 文件夹。

## 控制

| 旋钮 | 作用 |
| --- | --- |
| **INPUT** | 输入增益 ±24 dB |
| **OUTPUT** | 末级输出微调 Trim ±24 dB（Ceiling 固定 0 dB） |

- 左表：输入电平；右表：输出电平 + GR 增益衰减
- 底部 − / + ：调节界面缩放

## 系统要求

- Windows 10 / 11（x64）
- 支持 VST3 的宿主

## 构建（开发者）

JUCE 8.0.14 + C++20 + CMake。中文路径有 bug，需先同步源码到纯英文副本再编译（详见 `CLAUDE.md`）。

```bash
cmake -G "Visual Studio 18 2026" -DJUCE_ROOT="<juce路径>" /c/IridiumProj
cmake --build . --config Release
```

VST3 输出：`/c/IridiumBuild/Iridium_artefacts/Release/VST3/Iridium.vst3`

## 许可

[GPL-3.0](LICENSE) · Copyright © 2026 Ium Audio

## 作者

DJAnta（bilibili 3493259676486408）· Ium Audio
