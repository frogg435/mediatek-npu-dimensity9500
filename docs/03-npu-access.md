# 03 · NPU 访问分析

这一章记录**失败路径**——它们决定了为什么必须走 AOT。
理解这些限制，才能理解后面「为什么 AOT 能行」。

## 3.1 路径一：NNAPI（官方 NDK 接口）

`libneuralnetworks.so` 是 Android NDK 公共库，能正常 dlopen：

```c
ANeuralNetworks_getDeviceCount(&n);   // n = 1
ANeuralNetworks_getDevice(0, &dev);
ANeuralNetworksDevice_getType(dev, &type);   // type = CPU
```

**只返回 1 个 CPU 参考实现设备，加速器不暴露。** 名字和版本查询返回空串。

原因链条：

1. `/vendor/lib64/hw/` 里**没有** NNAPI HAL 的 `.so`，说明走的是 AIDL 直连
2. NNAPI runtime 加载了 `/system/lib64/libneuralnetworks_packageinfo.so`
   ——这是 AOSP 用来**按 UID 反查包名做准入判断**的组件
3. MTK Neuron 设备在注册给 NNAPI 时被按包名过滤：`com.termux` 不在 vivo 白名单

## 3.2 路径二：Neuron SDK 直连

`/system_ext/lib64` 在 linker 允许路径内，可以**直接 dlopen** 厂商运行时：

```c
dlopen("/system_ext/lib64/libneuronservice_adapter.mtk.so", RTLD_NOW);
Neuron_getDeviceCount(&n);   // n = 2
```

**拿到了 2 个 NPU 设备句柄。** 完整的 Neuron C API 都可用（89 个函数）。

### 但设备端编译只支持两个算子

用 `NeuronModel_getSupportedOperationsForDevices` 逐个算子探测
（按各算子真实参数个数构造模型，避免误判）：

```
算子      dev[0]  dev[1]
ADD         OK      --
MUL         OK      --
SUB          --     --
DIV          --     --
MAXIMUM      --     --
MINIMUM      --     --
RELU         --     --
RELU1        --     --
RELU6        --     --
TANH         --     --
LOGISTIC     --     --
SOFTMAX      --     --
CONV_2D      --     --     ← 超分必需
```

`ADD` 能完整跑通 model → compile → execution → compute，数值校验 PASS
（见附录的最小示例）。**但卷积不支持，等于跑不了任何真实网络。**

## 3.3 交叉验证：Google 官方源码也没有 conv

LiteRT 的 MediaTek 后端只实现了以下算子
（`litert/vendors/mediatek/compiler/legalizations/`）：

```
add, batch_matmul, concat, fully_connected, gelu, layer_norm, mean, mul,
quantize, reshape, resize_bilinear, resize_nearest_neighbor, rms_norm,
rsqrt, softmax, split, squared_difference, strided_slice, sub,
transpose, transpose_conv
```

**没有 `conv2d`。** 与设备实测完全吻合。

## 3.4 根因：magic number 分档

MTK 用 magic number 对能力分级下发。设备查询：

```c
Neuron_getNeuroPilotMagicNumber(&v);   // v = 300
```

对应 SDK 头文件里的定义：

```c
kMinMagicNumberForNeuronService = 300;   // 我们在这档
kMinMagicNumberForNP10          = 400;   // 更高特性档
```

`Neuron_getVersion` 返回 521，`Neuron_getL1MemorySizeKb` 返回 7168 KB
（4 个 MDLA 核心共享 7 MB L1）。

### 换客户端库无效

用官方 NeuroPilot SDK 里更新的 v9.0.3 运行时再测：

| 运行时 | magic | version | 可用算子 |
|---|---|---|---|
| 设备 `/system_ext/lib64/libneuronservice_adapter.mtk.so` | 300 | 521 | ADD/MUL |
| SDK v9.0.3 `usdk/lib64/libneuronusdk_adapter.so` | **300** | 66057 | ADD/MUL |

**换更新的客户端库，magic 仍是 300。限制在 HAL 服务端固件，不在客户端。**

## 3.5 服务端固件从哪来

```bash
$ cat /vendor/etc/init/neuronservice-mtk-default.rc
service neuralnetworks_hal_mtk_neuron_service /vendor/bin/hw/...
    class hal
    user system
    group system
    disabled

on property:ro.vendor.mtk_nn_baseline_support=1
    start neuralnetworks_hal_mtk_neuron_service
```

服务默认 `disabled`，靠属性拉起。它也创建了 `/data/vendor/nn` 和
`/data/vendor/nn/debug_plus`（均被 SELinux 挡住，不可访问）。

## 3.6 一个意外发现：驱动会崩

测试 `TRANSPOSE` 时，调用 `NeuronModel_getSupportedOperationsForDevices`
直接触发 **SIGSEGV**（两个设备都崩）。

一个成熟驱动不会这样。说明 vivo 这套接口**只针对白名单 app 的固定模型路径打磨过**，
第三方任意调用根本不在测试覆盖范围内。

## 3.7 结论与转折

在线编译路线彻底不可行：

| 层 | 状态 |
|---|---|
| NNAPI | 只给 CPU 参考实现 |
| Neuron SDK 直连 | 能连能算，但只有 ADD/MUL |
| 卷积 | 不支持 |

**但是**——两条 LiteRT issue 提示了另一条路：

- [LiteRT #8098](https://github.com/google-ai-edge/LiteRT/issues/8098)：
  Conv2D 在 Neuron SDK 8.2.19 上**可以**合法化
- [LiteRT #6462](https://github.com/google-ai-edge/LiteRT/issues/6462)：
  **同款 MT6993 设备**上 AOT 编译报告 "1586 / 1586 ops offloaded to 1 partitions"

差异在于：**AOT（离线预编译）走的是另一条代码路径**。
在线编译被裁剪，但生成的字节码设备端照样能执行。

这就是 [04 · AOT 编译](04-aot-compilation.md) 要解决的问题。
