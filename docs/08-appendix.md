# 08 · 附录

## 8.1 踩坑清单（按遇到顺序）

| # | 现象 | 原因 | 解法 |
|---|---|---|---|
| 1 | `/dev/mali0` 报 EACCES | 文件沙箱假象 | 在真实 shell 验证 |
| 2 | `/vendor/lib64/libOpenCL.so` dlopen 失败 | linker namespace | 复制到 `/data`（`opencl-vendor-driver`） |
| 3 | 设 `LD_LIBRARY_PATH` 后 ocl-icd 加载失败 | `libbinder_ndk.so` verneed 解析被干扰 | **不设该变量** |
| 4 | ncnn 进程 SIGABRT | OpenMP 亲和性断言 | `export KMP_AFFINITY=disabled` |
| 5 | NNAPI 只有 CPU 设备 | 按包名白名单过滤 | 无法绕过（需非 root 之外的方案） |
| 6 | Neuron 只有 ADD/MUL | magic=300 档位，服务端固件限制 | 走 AOT 路线 |
| 7 | `TRANSPOSE` 调用 SIGSEGV | 驱动未覆盖第三方调用 | 避开 |
| 8 | `qemu-x86_64` 被 SIGSYS 杀 | Android seccomp 禁 `setgid` | 用 `proot -q qemu-x86_64` 包一层 |
| 9 | pip DNS 解析失败 | sysroot 缺 `resolv.conf` | 写入 `nameserver 8.8.8.8` |
| 10 | lld 找不到 `ld-linux` | 绝对路径符号链接 | 改相对链接 |
| 11 | `Exec format error: apply_plugin_main` | 裸 ELF 被 subprocess 执行 | shell 包装器转给 qemu |
| 12 | `Failed to make DLA temporary directory (errno=13)` | Android 无 `/tmp` | 设 `MTKNN_ADAPTER_DLA_DIR` / `TMPDIR` |
| 13 | `Need to set MTKNN_ADAPTER_DLA_PLATFORM` | 手工建模型时未设 | 设 `mt6993-a` |
| 14 | `restoreFromCompiledNetwork` rc=4 | bytecode 是 LiteRT 私有容器 | 用 LiteRT 运行时 |
| 15 | `You should provide the DispatchLibraryDir option` | 环境未给插件目录 | `LiteRtEnvOption` tag=1 |
| 16 | `504 kLiteRtStatusErrorCompilation` | 显式请求了 NPU accelerator | **设成 CPU 位** |
| 17 | `CreateManagedTensorBufferFromRequirements` 返回 1 | 该 API 在此环境不可用 | 用显式 tensor type 版本 |
| 18 | 超分输出一片灰 | 选了本身平坦的区域 | 换纹理区，或用 CPU 对照验证 |

## 8.2 Neuron SDK 最小可用示例（设备端）

在设备上直接跑一个 ADD 算子，验证 NPU 通道可用：

```c
// 编译: clang neuron_add.c -o neuron_add -ldl
// 运行: ./neuron_add
#define LIB "/system_ext/lib64/libneuronservice_adapter.mtk.so"

typedef struct { int32_t type; uint32_t dims; const uint32_t *d; float s; int32_t zp; } OT;
#define NEURON_TENSOR_FLOAT32 3
#define NEURON_INT32          1
#define NEURON_ADD            0
#define NEURON_NO_ACTIVATION  0

void *h = dlopen(LIB, RTLD_NOW);
int (*Model_create)(void**);
int (*Model_addOperand)(void*, const OT*);
int (*Model_setVal)(void*, int32_t, const void*, size_t);
int (*Model_addOp)(void*, int32_t, uint32_t, const uint32_t*, uint32_t, const uint32_t*);
int (*Model_identify)(void*, uint32_t, const uint32_t*, uint32_t, const uint32_t*);
int (*Model_finish)(void*);
int (*Comp_create)(void*, void**);
int (*Comp_finish)(void*);
int (*Exec_create)(void*, void**);
int (*Exec_setInput)(void*, int32_t, const OT*, const void*, size_t);
int (*Exec_setOutput)(void*, int32_t, const OT*, void*, size_t);
int (*Exec_compute)(void*);
// ... dlsym 各符号 ...

uint32_t dims[2] = {1, 4};
OT t   = { NEURON_TENSOR_FLOAT32, 2, dims, 0, 0 };
OT tAct= { NEURON_INT32, 0, NULL, 0, 0 };

void *m, *c, *e;  int32_t act = NEURON_NO_ACTIVATION;
Model_create(&m);
Model_addOperand(m, &t);       // 0 输入 A
Model_addOperand(m, &t);       // 1 输入 B
Model_addOperand(m, &tAct);    // 2 act
Model_addOperand(m, &t);       // 3 输出 C
Model_setVal(m, 2, &act, 4);
uint32_t ins[3] = {0, 1, 2}, outs[1] = {3};
Model_addOp(m, NEURON_ADD, 3, ins, 1, outs);   // 注意：ADD 是 3 个输入
Model_identify(m, 2, ins, 1, outs);
Model_finish(m);
Comp_create(m, &c);  Comp_finish(c);           // ← 在线编译，conv 在这里会被拒
Exec_create(c, &e);
Exec_setInput(e, 0, &t, A, sizeof(A));
Exec_setInput(e, 1, &t, B, sizeof(B));
Exec_setOutput(e, 0, &t, C, sizeof(C));
Exec_compute(e);
// C[i] == A[i] + B[i]
```

**关键点**：

- MTK 的算子编号与 NNAPI 一致（`ADD=0`、`CONV_2D=3`）
- 但 `NEURON_TENSOR_INT32 = 4`（NNAPI 是 2），`NEURON_TENSOR_QUANT8_ASYMM = 5`（NNAPI 是 1）
- `ADD`/`MUL` 的第三个输入是 activation 标量，漏掉会导致 `inputCount` 错位
- `pad` 和 `stride` 在 MTK 实现里是 **INT32 标量**，不是 NNAPI 的数组

## 8.3 LiteRT C API 签名速查

```c
LiteRtStatus LiteRtCreateEnvironment(int num_options,
                                     const LiteRtEnvOption* options,
                                     LiteRtEnvironment* environment);

LiteRtStatus LiteRtCreateModelFromFile(LiteRtEnvironment environment,
                                       const char* model_path,
                                       LiteRtModel* model);

LiteRtStatus LiteRtCreateOptions(LiteRtOptions* options);

LiteRtStatus LiteRtSetOptionsHardwareAccelerators(LiteRtOptions options,
                                                  LiteRtHwAcceleratorSet accelerators);

LiteRtStatus LiteRtCreateCompiledModel(LiteRtEnvironment environment,
                                       LiteRtModel model,
                                       LiteRtOptions compilation_options,
                                       LiteRtCompiledModel* compiled_model);

LiteRtStatus LiteRtGetCompiledModelInputTensorLayout(
    LiteRtCompiledModel cm, LiteRtParamIndex signature_index,
    LiteRtParamIndex input_index, LiteRtLayout* layout);

LiteRtStatus LiteRtGetCompiledModelOutputTensorLayouts(
    LiteRtCompiledModel cm, LiteRtParamIndex signature_index,
    size_t num_layouts, LiteRtLayout* layouts, bool update_allocation);

LiteRtStatus LiteRtCreateManagedTensorBuffer(
    LiteRtEnvironment env, LiteRtTensorBufferType buffer_type,
    const LiteRtRankedTensorType* tensor_type, size_t buffer_size,
    LiteRtTensorBuffer* buffer);

LiteRtStatus LiteRtRunCompiledModel(LiteRtCompiledModel compiled_model,
                                    LiteRtParamIndex signature_index,
                                    size_t num_input_buffers,
                                    LiteRtTensorBuffer* input_buffers,
                                    size_t num_output_buffers,
                                    LiteRtTensorBuffer* output_buffers);
```

## 8.4 环境变量总表

### LiteRT / MediaTek adapter

| 变量 | 值示例 | 作用 |
|---|---|---|
| `MTKNN_ADAPTER_DLA_PLATFORM` | `mt6993-a` | **必需**，目标平台 |
| `MTKNN_ADAPTER_COMPILER_OPTIONS` | `--num-mdla=4` | 注入编译器 flags |
| `MTKNN_ADAPTER_MDLA_CORE_NUM` | `4` | MDLA 核心数 |
| `MTKNN_MDLA_ENABLE_MULTI_CORE` | `1` | 多核开关 |
| `MTKNN_ADAPTER_DLA_DIR` | 可写目录 | DLA 输出目录 |
| `MTKNN_ADAPTER_CONFIG_TARGET` | `MDLA` | 限制编译目标 |
| `MTKNN_ADAPTER_SET_DEBUG_LEVEL` | `0~3` | 日志级别 |
| `MTKNN_VERBOSE_LEVEL` | `0~3` | 详细程度 |
| `MTKNN_ADAPTER_GRAPH_DUMP_PATH` | 文件路径 | 图 dump |

### 运行环境

| 变量 | 作用 |
|---|---|
| `DISPATCH_DIR` | `libLiteRtDispatch_MediaTek.so` 所在目录 |
| `ACCEL` | accelerator 位（本文环境用 `1` = CPU） |
| `KMP_AFFINITY=disabled` | 配合 `taskset` 使用 ncnn 时必须 |
| `DUMP` | 导出推理输出（调试用） |
| `ITER` | 迭代次数（基准测试用） |

## 8.5 文件清单

### 设备端（`npu-test/`）

| 文件 | 说明 |
|---|---|
| `NPU-NOTES.md` | 完整研究笔记（含所有中间结论） |
| `neuron_add.c` | Neuron SDK 最小推理示例（ADD） |
| `nprobe.c` | 运行时探测（magic/版本/L1/算子矩阵），可传库路径 |
| `probe3.c` | 算子支持度矩阵探测（按真实签名构造模型） |
| `device_restore.c` | 尝试用 Neuron API 直接 load AOT blob |
| `npu_run2.c` | **LiteRT NPU 运行器**（含计时/导出） |

### AOT 工具链（`x86env/`）

| 路径 | 说明 |
|---|---|
| `sysroot/` | x86_64 glibc（Debian bookworm 解包） |
| `python/` | x86_64 Python + LiteRT + MTK SDK |
| `aar/libs/` | 从 Maven 解出的 arm64 运行库 |
| `run_aot.py` | AOT 编译脚本 |
| `build_x86.sh` | 交叉编译封装 |
| `aot_sr/` `aot_mc4/` | 编译好的模型 |

### 超分应用（`sr-npu/`）

| 文件 | 说明 |
|---|---|
| `sr_tile.py` | 切块 / 拼接 |
| `sr_run.c` | 分块 NPU 推理器 |
| `full_sr.png` | 9456×7092 结果 |
| `compare_best.png` | 画质对比 |

## 8.6 参考资料

- [LiteRT MediaTek 后端源码](https://github.com/google-ai-edge/LiteRT/tree/main/litert/vendors/mediatek)
- [LiteRT #6462 — AOT 编译优化差距](https://github.com/google-ai-edge/LiteRT/issues/6462)
- [LiteRT #8098 — Conv2D per-channel 量化 bias 问题](https://github.com/google-ai-edge/LiteRT/issues/8098)
- [MediaTek NeuroPilot 与 LiteRT](https://developers.google.com/edge/litert/next/mediatek)
- [termux-packages #27640 — Mali OpenCL on Termux](https://github.com/termux/termux-packages/issues/27640)
- [phone2a-apu-unlock — /dev/apusys 需要的 sepolicy](https://github.com/Peterc3-dev/phone2a-apu-unlock)
- [python-build-standalone](https://github.com/astral-sh/python-build-standalone)

## 8.7 后续可做

| 方向 | 说明 | 预期收益 |
|---|---|---|
| 内部编译 flags | 逐个试 SDK 模板里剩余约 19 个 MDLA flag | 低（公开 adapter 可能忽略） |
| 多 MDLA 并行 | 已用 `--num-mdla=4`，可试 `--mdla-multicore-barrier` 不同取值 | 中 |
| 量化模型 | int8 量化后再 AOT，理论上吞吐更高 | 高 |
| 减少 IO 拷贝 | 用 `NeuronExecution_setInputFromMemory` / AHWB 零拷贝 | 中（小模型收益明显） |
| 更多模型 | 换 ESRGAN 变体、RIFE 补帧、去噪模型 | — |
| Android 工程化 | 把运行时封成 AAR 依赖，做成正式 App | — |
