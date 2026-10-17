# tools · 可复用工具

本目录是研究过程中产出的可独立使用的工具，按用途分类。

> **验证环境**：vivo V2548A / MT6993 (天玑 9500) / Android 16 / 固件 `PD2548_A_16.0.14.6.W10` / 无 root。
> NPU 相关工具的行为高度依赖厂商固件，换机型请先用 `probe_runtime.c` 确认。

---

## GPU 超分（无需 root、无需 NPU）

### `sr-gpu.sh` —— 低干扰 GPU 超分封装

基于 `realesrgan-ncnn-vulkan`，默认三重限流避免卡系统。

```bash
pkg install realesrgan-ncnn-vulkan
cp sr-gpu.sh $PREFIX/bin/sr && chmod +x $PREFIX/bin/sr

sr photo.jpg                 # 单张，输出 photo_x4.jpg
sr -s 2 photo.jpg            # 2 倍
sr ~/DCIM/Camera             # 整目录
sr -m quality -s 2 photo.jpg # 真实照片质量模式
```

| 预设 | 模型 | 720p×4 实测 |
|---|---|---|
| `fast`（默认） | realesr-animevideov3 | **3.3 秒** |
| `general` | realesrnet-x4plus | ~15 秒 |
| `anime` | realesrgan-x4plus-anime | ~70 秒 |
| `quality` | realesrgan-x4plus | ~70 秒 |

内置限流：`taskset -c 0-5` + `nice -n 19` + `ionice -c 3`，
并自动设置 `KMP_AFFINITY=disabled`（否则 ncnn 会因 OpenMP 断言崩溃）。

详见 [`sr-gpu.README.md`](sr-gpu.README.md)。

---

## NPU 超分（需 AOT 编译的模型）

### `sr_tile.py` + `sr_run.c` —— 分块超分管线

模型输入固定 128×128，需要把大图切块、逐块跑 NPU、再拼接。

```bash
# 编译 runner（需要前面文档里的 arm64 LiteRT 运行库）
clang sr_run.c -o sr_run -L$X/aar/libs -lLiteRt -Wl,-rpath,$X/aar/libs

# 1) 切块（可选第 5 参数裁剪 x,y,w,h）
python3 sr_tile.py prep input.jpg patches.bin meta.json

# 2) NPU 推理（块数见 meta.json 的 count）
ACCEL=1 LD_LIBRARY_PATH=$X/aar/libs ./sr_run \
    model_MediaTek_MT6993_apply_plugin.tflite patches.bin out.bin 40

# 3) 拼接
python3 sr_tile.py stitch out.bin meta.json result.png
```

依赖：`pkg install python-numpy`

支持 `-- 128 倍数补边（边缘复制）、NCHW 输出转置、裁掉补边`。

---

## NPU 探测与验证

### `probe_runtime.c` —— 运行时能力探测

打印 magic number、SDK 版本、L1 内存、设备数。
可传入任意库路径，用于对比不同版本的厂商运行时。

```bash
clang probe_runtime.c -o probe_runtime -ldl
./probe_runtime                                              # 设备自带运行时
./probe_runtime /path/to/sdk/usdk/lib64/libneuronusdk_adapter.so   # 指定版本
```

典型输出：

```
magic=300 version=521 L1=7168KB
设备数=2
```

### `probe_op_support.c` —— 算子支持度矩阵

按各算子**真实参数个数**构造模型并查询支持情况，
避免因签名不符导致误判。

```bash
clang probe_op_support.c -o probe_op_support -ldl
./probe_op_support
```

典型输出（未启用 AOT 的设备端在线编译路径）：

```
算子      dev[0]  dev[1]
ADD         OK      --
MUL         OK      --
SUB          --     --
CONV_2D      --     --     ← 在线编译不支持
```

### `neuron_add.c` —— Neuron SDK 最小推理示例

完整走通 model → compile → execution → compute 的 NPU 加法，
用于验证 NPU 通道是否可用。

```bash
clang neuron_add.c -o neuron_add -ldl && ./neuron_add
# 结果: 11.0 22.0 33.0 44.0   验证: PASS
```

### `npu_run.c` —— LiteRT NPU 运行器

自包含（不依赖 LiteRT 头文件），支持计时与输出导出。

```bash
clang npu_run.c -o npu_run -L$X/aar/libs -lLiteRt -Wl,-rpath,$X/aar/libs

ACCEL=1 LD_LIBRARY_PATH=$X/aar/libs ./npu_run model_MT6993.tflite
ACCEL=1 ITER=100 LD_LIBRARY_PATH=$X/aar/libs ./npu_run model.tflite   # 测延迟
ACCEL=1 DUMP=out.bin LD_LIBRARY_PATH=$X/aar/libs ./npu_run model.tflite  # 导出
```

> `ACCEL=1` 表示请求 CPU 加速器——**不要设成 NPU**，否则返回 504。
> 原因见 [05 · 设备端运行时](../docs/05-device-runtime.md)。

---

## 环境变量速查

| 变量 | 用于 | 典型值 |
|---|---|---|
| `ACCEL` | `npu_run` / `sr_run` | `1`（CPU 位） |
| `ITER` | `npu_run` | `100` |
| `DUMP` | `npu_run` | 输出文件路径 |
| `DISPATCH_DIR` | `sr_run` | LiteRT dispatch 插件目录 |
| `KMP_AFFINITY` | `sr-gpu.sh` | `disabled` |
| `MTKNN_ADAPTER_DLA_PLATFORM` | AOT 编译 | `mt6993-a` |
| `MTKNN_ADAPTER_COMPILER_OPTIONS` | AOT 编译 | `--num-mdla=4` |
