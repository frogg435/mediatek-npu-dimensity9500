# 06 · 性能基准

所有测量都在同一台设备、同一编译产物下完成。方法：预热 1 次后连续迭代 N 次取平均，
不同配置**交替多轮**测量以抵消热降频。

## 6.1 NPU vs CPU（MobileNetV1）

输入 224×224，float32，`ITER=30~120`。

| 模型 | 计算量 | NPU | CPU (XNNPACK 8T) | 加速 |
|---|---|---|---|---|
| MobileNetV1 0.25 | ~46 MFLOP | 0.968 ms | 0.888 ms | **0.9×** |
| MobileNetV1 1.0 | ~569 MFLOP | 1.355 ~ 1.891 ms | 5.003 ms | **~3.5×** |

### 观察：小模型 NPU 没有优势

MobileNetV1 0.25 只有 46 MFLOP，延迟完全被 **IO 拷贝 + 同步开销**支配
（NPU 需要把 602 KB 输入搬进去、读 4 KB 出来，还要等 event）。
计算量上去之后 NPU 才体现价值。

### 有效算力估算

MobileNetV1 1.0 ≈ 569 MFLOP，1.4 ms → **~400 GFLOP/s**（fp16 等效）。
这远低于 NPU 990 的理论峰值，原因见 6.3。

## 6.2 数值正确性

同一输入下，NPU 输出与 CPU（原始未编译模型）对比：

| 指标 | 值 |
|---|---|
| 最大绝对误差 | 0.001758 |
| 平均绝对误差 | 0.000046 |
| 余弦相似度 | 0.998898 |
| argmax | 一致 |
| top-5 | 集合相同（第 3/4 名因舍入互换） |

误差来源是编译时的 `--relax-fp32`（fp32 → fp16 放宽），属预期行为。

用 `--num-mdla=4` 编译后精度还更好：最大误差 **0.000360**，余弦 **0.99989642**，top-5 完全一致。

## 6.3 编译器 flags 调优

通过 `MTKNN_ADAPTER_COMPILER_OPTIONS` 注入（产物元数据里能看到注入后的字符串，证明生效）。

MobileNetV1 1.0，交替 5 轮，每轮 120 次：

| 版本 | 编译参数 | 中位延迟 | 相对基线 | 精度(余弦) |
|---|---|---|---|---|
| 基线 | `--relax-fp32 --opt-accuracy --opt 3` | 1.675 ms | 1.00× | 0.99890 |
| **mc4** | 基线 + **`--num-mdla=4`** | **1.400 ms** | **1.20×** | **0.99990** |
| bw | 基线 + `--opt-bw` | 1.22 ~ 1.68 波动 | 不稳定 | — |
| full | 完整 OptionsAres 串 | 1.15 ~ 1.51 波动 | 不稳定 | — |

**只有 `--num-mdla=4` 有稳定收益，而且同时提升了精度。**

其余 flag 组合（包括看起来很唬人的完整内部模板）都落在噪声范围内，
推测这些内部 flag 在公开 adapter 里被忽略或未实现。

复现：

```bash
export MTKNN_ADAPTER_COMPILER_OPTIONS="--num-mdla=4"
export MTKNN_ADAPTER_MDLA_CORE_NUM=4
export MTKNN_MDLA_ENABLE_MULTI_CORE=1
export MTKNN_ADAPTER_DLA_DIR=$PWD/dla_tmp TMPDIR=$PWD/dla_tmp
```

### 关于「153 倍」

[LiteRT #6462](https://github.com/google-ai-edge/LiteRT/issues/6462) 报告内部编译
比公开参数快约 153 倍。本文只拿到 1.2 倍。差距的来源基本清楚：

> 真正的加速来自 **Google 内部的编译器构建**，而不是参数。
> 公开 SDK 的 adapter 里只保留了模板字符串，实际执行路径是裁剪过的。

继续逐个试剩余 flag 预期收益有限。

## 6.4 Real-ESRGAN x4v3 超分

输入 2364×1773，分 128×128 块 → 每块输出 512×512，共 266 块。

| 执行方式 | 每块 | 整图 266 块 |
|---|---|---|
| **NPU** | **16.42 ms** | **4.37 秒** |
| CPU (XNNPACK) | 241.68 ms | 64.3 秒 |

**加速 14.7×。**

超分模型计算量大（210 个算子、卷积密集），所以 NPU 收益远高于 MobileNet。
这正是 NPU 该发力的场景。

### 端到端耗时构成

整图 4.37 秒里：

- NPU 推理：266 × 16.42 ms ≈ 4.37 s
- 分块/拼接（PIL + numpy）：约占 0.1 s 量级
- 磁盘读写（798 MB 中间数据）：已包含在上面

## 6.5 CPU 频率：改不了，也不需要

```bash
$ for c in 0 4 7; do cat /sys/devices/system/cpu/cpu$c/cpufreq/scaling_cur_freq; done
2000000
1700000
2750000

$ cat /sys/devices/system/cpu/cpu7/cpufreq/scaling_available_frequencies
4210000 4184000 4050000 ... 300000      # 最高 4.21 GHz

$ echo performance > /sys/.../scaling_governor
bash: Permission denied
```

D9500 的 prime core 能到 4.21 GHz，但**用户态无法设置调速器或频率**（需 root）。

### 但对 NPU 负载无意义

实测 NPU 推理期间 CPU 频率：

```
1100000
 900000
 300000
```

**跌到 300 MHz ~ 1.1 GHz** —— NPU 有独立时钟域，CPU 只负责下发任务。

**结论：对 NPU 负载，拉高 CPU 频率没有意义。** CPU 频率只影响
CPU 回退算子、IO 拷贝这类环节。

（已用 `termux-wake-lock` 防止休眠降频。）

## 6.6 测量方法说明

- 所有计时都是 `clock_gettime(CLOCK_MONOTONIC)` 包住 `LiteRtRunCompiledModel` 循环
- 首次调用作为预热排除
- 不同配置交替多轮，取中位数而非单次值——单次值波动可达 ±30%
- 设备无主动散热时长时间跑会降频，`mc4` 那组 5 轮全部稳定在 1.27~1.45 ms，
  说明测量可信
