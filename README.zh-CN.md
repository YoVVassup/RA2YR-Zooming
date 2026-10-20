# ViewCtrl — 《命令与征服：尤里的复仇》缩放插件

[English](README.md) · [Русский](README.ru-RU.md) · **简体中文**

[![许可协议：GPL v3](https://img.shields.io/github/license/YoVVassup/RA2YR-Zooming)](LICENSE.txt)
[![语言](https://img.shields.io/badge/language-C%2B%2B-00599C?logo=cplusplus&logoColor=white)](#-技术实现)
[![平台](https://img.shields.io/badge/platform-Windows%20x86-0078D6?logo=windows&logoColor=white)](#-系统需求)
[![游戏](https://img.shields.io/badge/game-C%26C%3A%20Yuri%27s%20Revenge-E6A23C)](#-系统需求)
[![框架](https://img.shields.io/badge/framework-Syringe-7B68EE)](#-技术实现)
[![测试](https://img.shields.io/badge/tests-69%20%E7%94%A8%E4%BE%8B%20%7C%20653%20%E6%96%AD%E8%A8%80-4CAF50)](#-状态)
[![Fork of](https://img.shields.io/badge/fork%20of-ChoyoTsumu%2FRA2--about-1f6feb?logo=github&logoColor=white)](https://github.com/ChoyoTsumu/RA2-about)

---

## 🎯 概述

《命令与征服：尤里的复仇》战术视图缩放插件。在渲染层放大战术视图，通过驱动游戏摄像机保持光标下的点固定不动，并在游戏把屏幕坐标转换为游戏空间的精确位置反向变换坐标——框选与点击准确落在你实际看到的位置上。

基于 **Windows API** 构建，使用 **Syringe**（Ares、Phobos 等 YR 扩展采用的同一注入框架）完成注入与生命周期管理。仅使用代码钩子——游戏可执行文件与资源从不在磁盘上被改写。

---

## ✨ 特性

- 🔍 **Ctrl + 鼠标滚轮**缩放战术视图：每级 ×1.15，范围 1.0×–4.0×，平滑且与帧率无关的插值
- 🎯 光标下的点保持固定：每次缩放步进都会移动游戏摄像机，画面不会偏移
- ↩️ **双击 Ctrl**（400 毫秒内）将缩放重置为 1.0×；回到 1.0× 时自动撤销累积的摄像机偏移
- 🧭 缩放时使用**方向键**平移视图
- 🖼 渲染层放大：保存视图矩形，以最近邻方式拉伸覆盖在游戏已绘制的帧之上
- 🧰 仅放大战术视图——侧边栏、HUD 与物理光标从不参与重映射
- 🧮 坐标仅在游戏转换它们的位置被变换（战术点击、框选矩形、右键拖动）
- 📦 不依赖 GScript：缩放系数完全由 ViewCtrl 持有——当模组自带缩放器时，ViewCtrl 会让位而非与其竞争（见状态）
- ⚡ 性能开销极小

---

## ✅ 状态

- 🧪 **单元测试：** 69 个测试用例 / 653 条断言 — `tests\bin\ViewCtrlTests.exe`
- 📊 **行覆盖率：** `src/` 99.7%（OpenCppCoverage 对 doctest 套件实测；剩余部分是少数防御性 OS 失败分支）
- ⚠️ **自动检测竞争性缩放器**（详见 [docs/conflict-detection.md](docs/conflict-detection.md)）：当模组自行缩放视图时——`GScript.ext`（例如 Tiberium Crisis）、Telescope v1.4、Kaleidoscope、MOExperience 或 Ra2.Vampire.Zoom——`GameInt` 会发现并让 ViewCtrl 保持惰性。检测分层：**L1** 模块文件名，**L2** `.syhks00` 钩子表指纹，**L3** 运行时字节扫描检测外来 `E9` 补丁，**L4** 反应式视图尺寸监视，**L5** 专用导出探测——被改名或运行时打补丁的竞争者仍会被捕获
- 🔬 **静态审计：** 每个钩子地址均已对照 YR Hook Encyclopedia 验证——对 Syringe 存根安全（被盗窗口内无相对跳转，恢复地址位于指令边界），钩子注册表无冲突；引导钩子 `0x52CAE9` 与 Ares 共享（`_YR_PostGameInit`）且可正确组合
- 🖥 **渲染顺序经反汇编验证：** 放大发生在游戏绘制到 `DSurface::Composite` 之后、光标位块传输之前、`Frame::Present` 之前——到达屏幕的是放大后的像素
- 🛡 **钩子具备 SEH 保护：** 每个钩子函数体都包裹在 `__try/__except` 中，对畸形游戏内存的访问异常会被吞掉并重放原始指令字节，而不是让游戏崩溃；测试构建中移除这些保护，以便测试仍能捕获真实缺陷
- 🎮 **已在游戏内验证：** Yuri's Revenge 1.001（模组化安装，1920×1080）：缩放范围与锚点、点击/框选变换、重置与撤销

---

## 💻 系统需求

- 操作系统：Windows 7 或更新版本（32 位进程）
- 游戏版本：Yuri's Revenge 1.001
- 运行时：Syringe 注入框架（用于加载 DLL）

---

## 📥 安装

1. 确认游戏通过 Syringe 启动（`Syringe.exe` 或基于它的模组启动器）
2. 将编译好的 DLL 放入游戏根目录
3. 在 Syringe 加载器配置中注册该 DLL
4. 启动游戏——Release 构建默认安静运行且不写文件；如需不重新编译即可获得诊断日志，请在启动前设置环境变量 `VIEWCTRL_LOG=1`（日志输出到调试器输出 / DebugView）。Debug 构建始终在游戏可执行文件旁写入 `ViewCtrl.log`

---

## 🔨 构建

`YRpp`（Syringe 头文件）是 git 子模块，因此请递归克隆：

```bash
git clone --recurse-submodules https://github.com/YoVVassup/RA2YR-Zooming.git
```

若已有克隆，请初始化一次子模块：

```bash
git submodule update --init
```

然后构建 `Release|Win32`（v143 工具集）：

```bash
msbuild viewctrl.vcxproj /p:Configuration=Release /p:Platform=Win32
```

测试位于 `tests\test.vcxproj`，仅以 `Debug|Win32` 构建：

```bash
msbuild tests\test.vcxproj /p:Configuration=Debug /p:Platform=Win32
tests\bin\ViewCtrlTests.exe
```

---

## 🎛 操作

| 操作 | 效果 |
|------|------|
| **Ctrl + 滚轮上/下** | 放大 / 缩小（每级 ×1.15，限制 1.0×–4.0×） |
| **双击 Ctrl**（400 毫秒内） | 将缩放重置为 1.0× 并撤销摄像机偏移 |
| **方向键**（缩放时） | 平移视图 |

不按 Ctrl 时滚轮原样传递给游戏。所有缩放参数均为编译期常量（`src\Zoomer.hpp`）。如需调整，请修改源代码并重新编译。

---

## 🤝 兼容性

- 仅使用钩子，从不改写可执行文件——可与其他 Syringe DLL（Ares、Phobos 等）共存加载；Syringe 在共享站点上串联多个 DLL
- `0x52CAE9` 的 5 个字节与 Ares 的 `_YR_PostGameInit` 共享；ViewCtrl 在其后执行原始字节，两个处理器可组合
- 早期构建曾把缩放系数委托给 GScript；ViewCtrl 不再读写 GScript——若 GScript 自身缩放功能处于激活状态，两者之一会被自动禁用（见 [docs/conflict-detection.md](docs/conflict-detection.md)）
- 显示层缩放器（cnc-ddraw 分支、Vampire.Zoom）改变的是窗口尺寸而非视图矩形——它们可以共存加载，但叠加的放大效果通常不是你想要的
- 若遇到与特定模组的兼容性问题，请附上 `ViewCtrl.log`（来自 Debug 构建）提交 Issue（Release 构建完全编译掉文件日志；可用 `VIEWCTRL_LOG=1` 开启临时日志）

---

## ⚙️ 技术实现

- **注入框架：** Syringe `DEFINE_HOOK` — 9 个钩子，所有地址集中于 `src\GameAddrs.hpp` 并附有来源注释（YRpp / ReSource / Encyclopedia）

| 地址 | 字节 | 用途 |
|------|------|------|
| `0x52CAE9` | 5 | 游戏初始化后的引导钩子（`InitGame`）——与 Ares 共享，可组合 |
| `0x4F44AF` | 5 | 渲染前：恢复干净的视图备份，推进缩放插值 |
| `0x4F451B` | 5 | 渲染后：放大已绘制的视图 |
| `0x6D864E` / `0x6D868A` | 6 | `ClampCoordMap` — 视图宽高读取被限制在放大的源矩形内（回退到最近一次有效尺寸） |
| `0x692325` | 6 | `DisplayClass::ProcessClickCoords` — 战术点击变换 |
| `0x6D9F80` / `0x6D9FC0` | 6 | 框选矩形起点/终点变换 |
| `0x693791` | 6 | `ScrollMapEdge` — 右键拖动滚动速度变换 |

- **渲染路径（经反汇编验证）：** `MainLoop` → `GScreenClass::Render`（`0x4F4480`）将帧绘制到 `DSurface::Composite` → ViewCtrl 放大 → `wwmouse->DrawCursorBuffered` 位块传输未缩放的光标 → 虚函数 `RenderFrame` → `Frame::Present`
- **输入：** 子类化游戏窗口过程（`SetWindowLongPtrW`）；仅当按住 Ctrl 且光标位于战术视图上时滚轮才进行缩放，其余全部转发给游戏；光标自身位置从不重映射
- **摄像机：** 每次缩放步进都移动游戏摄像机（`TacticalMapClass::SetCameraPosition`，`0x6D6000`），使焦点保持在光标下；回到 1.0× 时撤销偏移
- **视图矩形：** `DSurface::ViewBounds`，轮询间隔不超过 250 毫秒，且仅在未处于放大状态时接受；矩形本身不包含侧边栏
- **坐标变换：** 游戏把屏幕坐标转换为游戏空间的位置会被反向缩放；`Zoomer::UnMagnify` 是渲染器拉伸的精确逆变换
- **健壮性：** 每个钩子函数体均用 SEH（`src\Seh.h`）包裹，异常被吞掉并重放原始字节；Release 日志默认关闭，可在运行时通过 `VIEWCTRL_LOG=1` 开启

---

## 📚 文档

- [docs/conflict-detection.md](docs/conflict-detection.md) — 竞争性缩放器的检测方式（L1–L5）以及哪些情况*不算*冲突
- [docs/research-landscape.md](docs/research-landscape.md) — 所有已知 YR 缩放实现的调研及与 ViewCtrl 的关系

---

## 🏅 致谢

**Fork 溯源**

- 本仓库是 [ChoyoTsumu/RA2-about](https://github.com/ChoyoTsumu/RA2-about) 的 fork——视图缩放的原始概念与核心实现由 **ChoyuTsumu** 完成，**Sovietianqi** 协助
- 本 fork：[YoVVassup/RA2YR-Zooming](https://github.com/YoVVassup/RA2YR-Zooming)——重构为渲染层 Syringe 插件（摄像机驱动、坐标变换、单元测试、钩子静态审计）；由 **YoWassup** 维护

**用于验证逻辑的研究材料**

- [YRpp](https://github.com/Phobos-developers/YRpp) — Syringe / C&C 头文件（本项目的 git 子模块）
- [RA2YR_ReSource](https://github.com/Ritanlisa/RA2YR_ReSource) — 通过逆向工程获得的 `gamemd.exe` 符号与类数据库，用于命名及交叉核对地址
- YR Hook Encyclopedia — 来自 Ares、Phobos、Kratos 等的钩子地址注册表，用于冲突检查与 Syringe 安全性审计
- [Ares](https://ares-developers.github.io/Ares-docs/) — Syringe 注入框架与参考扩展；与我们共享引导钩子 `0x52CAE9`
- [Phobos](https://github.com/Phobos-developers/Phobos)、[Kratos](https://github.com/ra2diy/KratosPP) — 钩子地址交叉核对

欢迎贡献与 pull request。

---

## 📜 许可协议

本项目采用 GNU General Public License v3.0 许可协议。详见 [LICENSE.txt](LICENSE.txt)。
