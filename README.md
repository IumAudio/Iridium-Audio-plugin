# Iridium

前视增益场限幅器 · Forward-looking Gain-Field Limiter（VST3）

输出 **y = f(x)·g(t)**：把信号按**过零点**切成「半周期」，每段恒定增益（由局部极值约束），增益**只在过零点切换**（零点锚定，无咔哒），全程**无 Attack / Release 时间常数**。它是一张随时间与上下文连续绘制的增益场，而不是无记忆的 waveshaper。

## 特性

- 分段前视增益场（两段前视，峰值精准贴顶）
- 16× 过采样（抗混叠）
- Final Ceiling 真峰值天花板（16× 检测 + 2ms 前视，上限 0 dBFS 真峰值）
- **LUFS 短期响度表**（ITU-R BS.1770 K 加权）
- 输入/输出精度 0.01 dB（Shift+滚轮精细微调）
- WebView2 界面 · 铱（Ir）元素美学 · 界面尺寸可调
- 原子装饰动画（点击原子核彩蛋）· DJAnta 主页快捷按钮 · K–Pg 边界彩蛋

## 版本更新

完整历史见 [CHANGELOG.md](CHANGELOG.md)；安装包见 [Releases](https://github.com/IumAudio/Iridium-Audio-plugin/releases)。

## 下载与安装

最新安装包见 [Releases](https://github.com/IumAudio/Iridium-Audio-plugin/releases)。

1. 解压 `Iridium-v0.1.0-win-x64.zip`
2. 把 `Iridium.vst3` 整个文件夹复制到 `C:\Program Files\Common Files\VST3\`
3. 打开 DAW（REAPER / Ableton / Cubase / Studio One 等）扫描新插件即可

卸载：删除 `C:\Program Files\Common Files\VST3\Iridium.vst3` 文件夹。

## 控制

| 旋钮 | 作用 |
| --- | --- |
| **INPUT** | 输入增益 ±24 dB（精度 0.01 dB） |
| **OUTPUT** | 末级输出微调 Trim ±24 dB（Ceiling 固定 0 dB） |

- **FC**（Output 下）：Final Ceiling，开 = 16× 真峰值限幅（上限 0 dBFS 真峰值），关 = 样本峰值安全（上限 0 dBFS）
- **OS**（Input 下）：OverSampling，开 = 16× 过采样，关 = 基频处理（更省 CPU；与 FC 独立互不影响）
- **LINK**（两旋钮间）：联动 Input/Output，推起 Input 同时拉低 Output
- **LUFS 表**（Input 电平下方）：短期响度数字表
- 旋钮滚轮微调：普通每格 0.5 dB，**Shift+滚轮 = 0.01 dB**；Ctrl+Z 撤回 / Ctrl+Y 重做
- 左表：输入电平 + LUFS；右表：输出电平 + GR 增益衰减
- 原子模型（Input 电平旁）：默认静止，点击原子核开启动画（彩蛋）
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
