# 09 · 数据来源与参考

本文档区分三类信息：**本机实测**、**官方源码/文档**、**社区讨论**。
读者应据此判断每条结论的可信度与适用范围。

---

## 9.0 先说清楚：哪些结论是我测的，哪些是查的

| 结论 | 来源类型 | 具体出处 |
|---|---|---|
| `/dev/mali0` 可打开、`/dev/dri/card0` 不可 | **实测** | `open()` 系统调用 + `ls -lZ` |
| linker namespace 的 `permitted_paths` 内容 | **实测** | dlopen 失败时 linker 打印的完整路径列表 |
| OpenCL 可在 Mali 上跑通 | **实测** | `clinfo` + 自写 kernel（29.39 GB/s，PASS） |
| NPU 只有 ADD/MUL | **实测** | `probe_op_support.c` 逐算子查询 |
| `magic=300` 是能力分档 | **实测 + 源码** | `Neuron_getNeuroPilotMagicNumber()` 返回值 + SDK 头文件常量 |
| AOT 能编译卷积 | **实测** | `aot_compile()` 成功产出 210 算子模型 |
| 设备端接受 AOT 字节码 | **实测** | dispatch 日志：版本校验通过、子图解析、IO 尺寸吻合 |
| `ACCEL=1` 才能跑通 | **实测**（试错） | NPU 位返回 504，CPU 位返回 0 |
| NPU 相对 CPU 快 14.7× | **实测** | `sr_run` 交替多轮计时 |
| 手机可自举 x86_64 工具链 | **实测** | qemu + proot + sysroot + pip 全流程 |
| 「AOT 可以绕过在线编译限制」这个方向 | **社区启发** | [LiteRT #6462](https://github.com/google-ai-edge/LiteRT/issues/6462)、[#8098](https://github.com/google-ai-edge/LiteRT/issues/8098) |
| 「NeuroPilot SDK 公开可分发」 | **社区启发** | [phone2a-apu-unlock](https://github.com/Peterc3-dev/phone2a-apu-unlock) |
| 「Termux 里 MTK Mali 能跑 OpenCL」 | **社区启发** | [termux-packages #27640](https://github.com/termux/termux-packages/issues/27640) |
| 「153 倍性能差距来自内部编译器」 | **社区引用** | [LiteRT #6462](https://github.com/google-ai-edge/LiteRT/issues/6462)，本文仅复现到 1.2 倍 |

---

## 9.1 决定性启发：三个网页改变了方向

以下三处外部信息直接改变了研究路径，值得单独说明。

### ① LiteRT #8098 —— 推翻了我的错误结论

**https://github.com/google-ai-edge/LiteRT/issues/8098**

我在设备上实测得出「NPU 不支持 CONV_2D」，并已开始写结论。
但这个 issue 描述了 MediaTek 插件在 **Neuron SDK 8.2.19** 上对 Conv2D 做
per-channel 量化 legalize 时踩的坑——说明 **Conv2D 是可以编译的**。

这条信息让我意识到：我测的是「设备端在线编译」，而存在另一条离线路径。
**如果没有看到这个 issue，本文会以一个错误结论收尾。**

该 issue 的具体内容（per-channel int32 bias 必须保持为 `NEURON_TENSOR_INT32`
并跳过 `NeuronModel_setOperandSymmPerChannelQuantParams`）也成为了我理解
MTK 操作数类型要求的参照。

### ② LiteRT #6462 —— 证明「同款设备上可行」

**https://github.com/google-ai-edge/LiteRT/issues/6462**

标题：*MediaTek AOT compilation via public API produces unoptimized bytecode
(missing 19 MDLA flags vs Google's official litertlm)*

正文中的环境信息：

```
Target device: Vivo V2509A — MT6993 / Dimensity 9500, Android 16
Packages: ai-edge-litert + ai-edge-litert-sdk-mediatek
```

**和本文的设备是同一颗 SoC。** 它报告 AOT 编译能报告
"1586 / 1586 ops offloaded to 1 partitions"。

这条信息给了两个关键判断：

1. AOT 路线上，卷积**确实**能卸载到 NPU
2. 公开工具的产物只有 `--relax-fp32`，而内部构建有 19 个 MDLA flags，差距约 153 倍

本文的 flags 调优（[06 · 基准](06-benchmarks.md)）只拿到 1.2 倍，
与该 issue 的说法一致：**真正的差距来自内部编译器构建，不是参数**。

> 注：本文的设备型号是 V2548A，与该 issue 的 V2509A 不同，但 SoC 相同（MT6993）。

### ③ phone2a-apu-unlock —— 提供整条路线的存在性证明

**https://github.com/Peterc3-dev/phone2a-apu-unlock**

这个项目本身是针对另一颗芯片（MT6886 / APU 650）的，但它的 README
包含了几条对本文至关重要的判断：

> "MediaTek's NeuroPilot stack (`ncc-tflite` compiler, `libneuron` runtime)
> exists, is **publicly redistributable through the Genio Yocto SDK**, and emits
> `mdla3.0` `.dla` files"

> "Both Google's LiteRT and PyTorch ExecuTorch ship official MediaTek backends.
> They **wrap the same closed runtime**; the APU is reachable from those stacks
> **once the runtime is in place**."

> "The publicly available NeuroPilot AOT path is reportedly crippled vs.
> Google-internal builds — see LiteRT issue #6462 (~153× speedup gap)"

第一句让我确认 SDK 是**合法公开分发**的，第二句让我决定走 LiteRT 路线而不是逆向 `.dla`，
第三句则预示了我后来只拿到 1.2 倍的结局。

该项目也确认了 `/dev/apusys` 需要 root + sepolicy 才能打开（KernelSU 模块），
与本文「未 root 环境下走不通直连」的判断一致。

---

## 9.2 官方源码与文档

### LiteRT 源码（GitHub / googlesource）

| 路径 | 用途 |
|---|---|
| `litert/vendors/mediatek/compiler/legalizations/` | 确认后端支持的 27 个算子中**没有 conv2d** |
| `litert/vendors/mediatek/neuron_adapter_api.h` | 拿到适配层 API、magic number 常量、扩展算子定义 |
| `litert/vendors/mediatek/dispatch/MediaTek_Neuro_Dispatcher.md` | 了解 dispatcher 的运行时职责与构建方式 |
| `litert/vendors/mediatek/dispatch/dispatch_api.cc` | 确认 dispatcher 如何加载 `libneuron_adapter.so` |
| `litert/tools/extract_bytecode.cc` | 官方字节码提取工具，确认 `bytecode_offset/size` 语义 |
| `litert/c/litert_environment_options.h` | `kLiteRtEnvOptionTagDispatchLibraryDir = 1` |
| `litert/c/litert_any.h` | `LiteRtAnyTypeString = 8`，结构体大小 16 |
| `litert/c/litert_layout.h` | `sizeof(LiteRtLayout) == 68` 等 ABI 断言 |
| `litert/c/litert_model_types.h` | `LiteRtRankedTensorType` 布局、`kLiteRtElementTypeFloat32 = 1` |
| `litert/c/litert_common.h` | 状态码枚举（`504 = kLiteRtStatusErrorCompilation`） |
| `litert/c/litert_compiled_model.h` | 各函数精确签名 |

> **说明**：这些头文件我并未直接 `#include`（依赖链太深），而是**读取源码后手动声明**。
> 结构体布局是从源码里的 `static_assert` 反推出来的（见
> [05 · 设备端运行时](05-device-runtime.md#53-关键结构体靠-abi-静态断言反推)）。

### 官方文档

| 链接 | 说明 |
|---|---|
| [MediaTek NeuroPilot with LiteRT](https://developers.google.com/edge/litert/next/mediatek) | **抓取失败**（网络问题），仅通过搜索摘要获知存在 |
| [MediaTek Genio · Neuron SDK 文档](https://genio.mediatek.com/doc/iot-aihub/ai_hub/ai-workflow/neuron-sdk.html) | 搜索命中，未深入使用 |
| [MediaTek Genio · NeuroPilot 安装](https://genio.mediatek.com/doc/ubuntu/bsp-installation/neuropilot.html) | 搜索命中，未深入使用 |

诚实说明：**Google 官方那篇 MediaTek 文档我没能成功抓取**（`web fetch failed`）。
本文关于 LiteRT AOT 的知识主要来自**源码阅读**和**issue**，而非官方教程。

### 厂商头文件（从 SDK 中获取）

`neuron_adapter_api.h` 与 `NeuronAdapter.h`（89 个函数）来自 NeuroPilot SDK：

```
neuro_pilot/v9_0_3/host/include/neuron/api/NeuronAdapter.h
```

其中的操作数类型枚举与 NNAPI **不一致**，是本文一个重要的坑点：

```c
NEURON_TENSOR_INT32   = 4     // NNAPI 是 2
NEURON_TENSOR_QUANT8_ASYMM = 5  // NNAPI 是 1
NEURON_TENSOR_FLOAT16 = 8     // NNAPI 是 5
```

---

## 9.3 社区讨论

### Termux / OpenCL

| 链接 | 启发 |
|---|---|
| [termux-packages #27640](https://github.com/termux/termux-packages/issues/27640) | `opencl-vendor-driver` 的用法；MTK 设备需把 `arm.mali.platform-V2-ndk.so` 复制成 `common-V4-ndk.so`；「无需设 `LD_LIBRARY_PATH`」的提示 |
| [MediaTek Genio 社区 · OpenCL on Termux](https://genio-community.mediatek.com/t/opencl-on-termux-with-mediatek-dimensity-8300-mali-gpu/1535/2) | 确认 Dimensity 系列在 Termux 跑 OpenCL 是可行的 |
| [Termux-Desktop · hw-acceleration.md](https://github.com/Ashar-me/Termux-Desktop/blob/main/hw-acceleration.md) | 硬件加速的通用思路 |
| [termux-mali-gpu-acceleration](https://github.com/Theguilherm3/termux-mali-gpu-acceleration) | GPU 兼容性清单 |

**注意**：本文的设备**不需要** #27640 里那个 `common-V4-ndk.so` 复制的步骤——
`libOpenCL.so` 的直接依赖里没有 `android.hardware.graphics.common`，
说明不同 MTK 机型的库排布不同。

### 其他

| 链接 | 启发 |
|---|---|
| [genio 社区 · Neuron Adapter restoreFromCompiledNetwork](https://genio-community.mediatek.com/t/neuron-adapter-api-failed-neuroncompilation-restorefromcompilednetwork/2617/3) | 了解 `restoreFromCompiledNetwork` 的常见失败模式 |
| [genio 社区 · NNAPI delegate vs Neuron delegate](https://genio-community.mediatek.com/t/what-do-the-parameters-in-the-tfliterunner-command-mean-and-why-does-nnapi-delegate-fail-while-neuron-delegate-succeeds/471/2) | 理解两个 delegate 的差异 |
| [ARM Mali-G1-Ultra MC12 规格](https://www.notebookcheck.net/Arm-Mali-G1-Ultra-MC12-Benchmarks-and-Specs.1157800.0.html) | 核对 GPU 型号 |

---

## 9.4 软件与制品来源

| 来源 | 用途 |
|---|---|
| [Maven · `com.google.ai.edge.litert:litert`](https://dl.google.com/dl/android/maven2/com/google/ai/edge/litert/) | Android arm64 版 `libLiteRt.so` |
| [Maven · `litert-npu-runtime-mediatek`](https://dl.google.com/dl/android/maven2/com/google/ai/edge/litert/) | `libLiteRtDispatch_MediaTek.so` |
| [PyPI · `ai-edge-litert`](https://pypi.org/project/ai-edge-litert/) | AOT 流水线（manylinux x86_64 wheel） |
| MTK NeuroPilot SDK（S3 直链） | 主机端编译器 `libneuron_adapter.so` —— 链接由 `ai-edge-litert-sdk-mediatek` 的 `setup.py` 给出 |
| [HuggingFace · `litert-community/real-esrgan-x4v3-litert`](https://huggingface.co/litert-community/real-esrgan-x4v3-litert) | 超分模型与**本文使用的公开测试图** |
| [python-build-standalone](https://github.com/astral-sh/python-build-standalone) | x86_64 Python |
| [Debian](https://deb.debian.org/debian/pool/) | glibc / libgcc / libstdc++ / zlib 的 amd64 包（构成 sysroot） |
| [Termux packages](https://github.com/termux/termux-packages) | `qemu-user-x86-64`、`proot`、`realesrgan-ncnn-vulkan`、`opencl-vendor-driver` |

---

## 9.5 明确未使用的来源

为了避免误导，列出**没有**依赖的东西：

- ❌ 没有使用任何逆向工程工具（IDA/Ghidra）分析厂商 `.so`
- ❌ 没有使用 root / Magisk / KernelSU
- ❌ 没有修改系统分区或 SELinux 策略
- ❌ 没有破解任何签名或加密
- ❌ 没有参考任何泄露的内部资料
- ❌ **没有使用 PC** —— 全部流程在手机本地完成

所有组件均来自公开渠道：Google 的 LiteRT（Apache-2.0）、MediaTek 公开分发的
NeuroPilot SDK、Maven 上的公开运行时、Debian/PyPI/HuggingFace 的公开软件包。

---

## 9.6 如何验证本文的结论

每条关键结论都有对应的可复现命令：

| 结论 | 验证方法 |
|---|---|
| GPU 可访问 | `clinfo`；或编译运行 [02 · GPU](02-gpu.md) 里的 vadd |
| NPU 算子受限 | `tools/probe_op_support.c` |
| 运行时档位 | `tools/probe_runtime.c`（看 magic number） |
| AOT 可用 | [04 · AOT](04-aot-compilation.md) 的编译流程 |
| NPU 执行正确性 | `tools/npu_run.c` 对比 CPU 输出 |
| 性能数字 | `tools/npu_run.c` 配 `ITER=N`，交替多轮 |

如果你在别的机型上跑出不同结果，**那是正常的**——见
[01 · 平台探测](01-platform.md#10-试验设备完整标识) 的适用范围说明。
欢迎提 issue 记录机型差异。
