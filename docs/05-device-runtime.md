# 05 · 设备端运行时

AOT 编译出来的 `.tflite` 里嵌的是 **LiteRT 私有的 dispatch bytecode**，不是 Neuron 原生格式。
尝试直接喂给 `NeuronModel_restoreFromCompiledNetwork` 返回 `rc=4`（OP_FAILED）。

正确的做法：**让 LiteRT 自己的运行时来跑它。**

## 5.1 运行时从哪来

LiteRT 的 Android 原生产物发布在 Maven：

```
com.google.ai.edge.litert:litert:2.3.0
  jni/arm64-v8a/libLiteRt.so                     5.63 MB   运行时
com.google.ai.edge.litert:litert-npu-runtime-mediatek:2.3.0
  jni/arm64-v8a/libLiteRtDispatch_MediaTek.so    0.33 MB   NPU 调度插件
  jni/arm64-v8a/libLiteRtCompilerPlugin_MediaTek.so
```

下载地址：

```
https://dl.google.com/dl/android/maven2/com/google/ai/edge/litert/<artifact>/<ver>/<artifact>-<ver>.aar
```

从 AAR 里解出 `jni/arm64-v8a/*.so` 即可，无需 Android 工程。

## 5.2 调用难点：没有可用的 C 头文件

LiteRT 的 C API 头文件依赖链极深（`litert_common.h` → `litert_any.h` →
`litert_environment_options.h` → `litert_custom_tensor_buffer.h` → ……），
在 Termux 里凑不齐。

**解法：自包含声明。** 所有句柄都是不透明指针，`LiteRtStatus` 是 int，
只要结构体布局对，直接声明原型就能用。

## 5.3 关键结构体：靠 ABI 静态断言反推

头文件里有这两段断言，直接给出了布局：

```c
static_assert(sizeof(LiteRtEnvOption) == 24, ...);
static_assert(offsetof(LiteRtEnvOption, value) == 8, ...);
static_assert(sizeof(LiteRtAny) == 16, ...);
static_assert(sizeof(LiteRtLayout) == 68, ...);
static_assert(offsetof(LiteRtLayout, dimensions) == 4, ...);
static_assert(offsetof(LiteRtLayout, strides) == 36, ...);
```

于是可以手写：

```c
typedef struct {
    int tag; int _pad0;
    int type; int _pad1;
    union { long long int_value; const char *str_value; const void *ptr_value; } value;
} EnvOption;                        // 24 字节

typedef struct {
    uint32_t rank_bits;             // rank:7 | has_strides:1
    int32_t  dimensions[8];
    uint32_t strides[8];
} LiteRtLayout;                     // 68 字节

typedef struct { int element_type; LiteRtLayout layout; } LiteRtRankedTensorType;
```

枚举值：

| 常量 | 值 |
|---|---|
| `kLiteRtEnvOptionTagCompilerPluginLibraryDir` | 0 |
| `kLiteRtEnvOptionTagDispatchLibraryDir` | 1 |
| `kLiteRtAnyTypeString` | 8 |
| `kLiteRtElementTypeFloat32` | 1 |
| `kLiteRtTensorBufferTypeHostMemory` | 1 |
| `kLiteRtHwAcceleratorCpu` | `1 << 0` |
| `kLiteRtHwAcceleratorNpu` | `1 << 2` |

状态码（`litert_common.h`）：

```
0 OK   1 InvalidArgument   2 MemoryAllocation   3 RuntimeFailure
504 = kLiteRtStatusErrorCompilation
```

## 5.4 完整的调用序列

```c
// 1) 环境：必须给出插件目录，否则 NPU 完全不可用
EnvOption eo[2] = {
  { .tag = 0, .type = 8, .value.str_value = dir },   // CompilerPluginLibraryDir
  { .tag = 1, .type = 8, .value.str_value = dir },   // DispatchLibraryDir
};
LiteRtCreateEnvironment(2, eo, &env);

// 2) 模型
LiteRtCreateModelFromFile(env, path, &model);

// 3) 选项 —— 这里是最关键的坑，见下节
LiteRtCreateOptions(&opts);
LiteRtSetOptionsHardwareAccelerators(opts, 1 /* CPU */);

// 4) 编译成可执行模型（此处会解析 bytecode、加载 NPU 插件）
LiteRtCreateCompiledModel(env, model, opts, &cm);

// 5) IO 缓冲区：用 layout 显式创建
LiteRtLayout il, ol;
LiteRtGetCompiledModelInputTensorLayout(cm, 0, 0, &il);
LiteRtGetCompiledModelOutputTensorLayouts(cm, 0, 1, &ol, 0);
LiteRtRankedTensorType it = { 1 /*Float32*/, il }, ot = { 1, ol };

size_t isz = 4, osz = 4;
for (unsigned i = 0; i < (il.rank_bits & 0x7f); i++) isz *= il.dimensions[i];
for (unsigned i = 0; i < (ol.rank_bits & 0x7f); i++) osz *= ol.dimensions[i];

LiteRtCreateManagedTensorBuffer(env, 1, &it, isz, &in_buf);
LiteRtCreateManagedTensorBuffer(env, 1, &ot, osz, &out_buf);

// 6) 填输入 → 执行 → 读输出
LiteRtLockTensorBuffer(in_buf, &addr, 1);  memcpy(...); LiteRtUnlockTensorBuffer(in_buf);
LiteRtRunCompiledModel(cm, 0, 1, &in_buf, 1, &out_buf);
LiteRtLockTensorBuffer(out_buf, &addr, 0); memcpy(...); LiteRtUnlockTensorBuffer(out_buf);
```

## 5.5 最关键的一个坑：不要请求 NPU

```c
LiteRtSetOptionsHardwareAccelerators(opts, 1 << 2);   // ❌ NPU
```

会得到：

```
INFO: [compiled_model.cc:1065] Accelerator returned null hooks.
  LiteRtCreateCompiledModel -> 504  (kLiteRtStatusErrorCompilation)
```

而实际上**已经**在跑 NPU 了——模型里嵌的 `DISPATCH_OP` 会自动被 dispatch delegate 接管：

```
VERBOSE: Replacing 2 out of 3 node(s) with delegate (DispatchDelegate) node
```

设成 CPU 或 CPU|GPU 都正常：

```c
LiteRtSetOptionsHardwareAccelerators(opts, 1);      // ✅ CPU
// LiteRtSetOptionsHardwareAccelerators(opts, 0x5); // ✅ CPU|GPU，也行
// LiteRtSetOptionsHardwareAccelerators(opts, 0);   // ❌ 反而失败
```

**结论：accelerator 位只影响 LiteRT 自己的加速器注册，NPU 由模型内嵌分区负责。
显式请求 NPU 会走另一条需要「hook 表」的路径，在 Termux 环境下拿不到 hook。**

## 5.6 另一个坑：缓冲区创建 API

```c
LiteRtCreateManagedTensorBufferFromRequirements(env, req, &buf);   // ❌ 返回 1 (InvalidArgument)
```

必须退回显式 tensor type 的版本：

```c
LiteRtCreateManagedTensorBuffer(env, kLiteRtTensorBufferTypeHostMemory, &tensor_type, size, &buf);
```

## 5.7 成功的日志

```
INFO: [accelerator_registry.cc:54] RegisterAccelerator: name=NpuAccelerator
INFO: [npu_registry.cc:30] NPU accelerator registered.
INFO: [dispatch_api.cc:133] Neuron SDK version: 9.2.0
INFO: [litert_dispatch_invocation_context.cc:266] Neuron SDK version check passed:
                                  bytecode 9.2.1, runtime 9.2.0
INFO: [schema_resolver.h:112] There are 2 subgraphs in the bytecode
INFO: Found graph: Partition_0 / Partition_1
WARNING: MediaTek compilation_get_input_padded_size  = 602112 bytes
WARNING: MediaTek compilation_get_output_padded_size = 4004 bytes
  LiteRtCreateCompiledModel -> 0
```

注意这里的关键信息：

- **版本校验通过**（bytecode 9.2.1 vs runtime 9.2.0）
- **子图解析正确**（2 个分区）
- **IO 尺寸与模型吻合**（602112 = 1×224×224×3 float；4004 = 1001 float）

说明设备端完全接受这份 AOT 字节码。

## 5.8 为什么不能直接喂 Neuron API

AOT 产物里的 bytecode 是 LiteRT 的**多分区容器**：

```
bytecode_offset = 1568
bytecode_size   = 1121956      （1568 + 1121956 = 文件总大小）
```

容器内部有 `Partition_0` / `Partition_1`、UUID、编译标记等结构，
两个分区的 offset/size 字段相同（都指向整个容器）。
这是 LiteRT 私有封装，不是 Neuron 的原生格式，所以：

```c
NeuronModel_restoreFromCompiledNetwork(&m, &c, blob, size);   // rc=4 (OP_FAILED)
```

失败是预期的。要让 LiteRT 运行时去解析它。
