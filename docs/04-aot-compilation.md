# 04 · AOT 编译

目标：在手机上离线编译出 MT6993 的 NPU 字节码。

核心矛盾：**MediaTek 的 host 端编译器只有 x86_64 Linux 版本**，而手机是 aarch64。
本章记录如何在手机上自举一个 x86_64 环境跑通它。

## 4.1 为什么必须 AOT

设备端在线编译（`NeuronCompilation_finish`）对卷积返回错误码 6，
但离线编译走的是另一条路径，可以产出完整字节码。见 [03 · NPU 访问分析](03-npu-access.md)。

## 4.2 环境自举总览

```
Termux (aarch64 Android)
└── qemu-user-x86-64 + proot          ← 模拟 x86_64 执行
    └── Debian glibc sysroot           ← 手动解包，不装发行版
        ├── clang --target=x86_64      ← 交叉编译本地小工具
        └── python-build-standalone    ← x86_64 Python
            └── ai-edge-litert         ← LiteRT AOT 流水线
                └── MediaTek SDK       ← 手动装配
```

模拟速度实测约 **22 倍慢**（sha256 基准），对一次性的编译任务可以接受。

## 4.3 第 1 步：x86_64 执行环境

```bash
pkg install qemu-user-x86-64 proot
```

### 坑 1：`setgid` 被 Android seccomp 拦截

直接用 `qemu-x86_64 ./busybox echo hi` 会被 **SIGSYS** 杀掉。
`-strace` 显示最后一个系统调用是 `setgid`：

```
setgid(10298)   ← 在这里被 seccomp 终止
```

Android 的 `untrusted_app` seccomp 策略禁止改 uid/gid。

**解法：用 `proot` 包一层**，由 proot 通过 ptrace 拦截并模拟这类调用：

```bash
proot -q qemu-x86_64 -0 -r / ./x86env/busybox echo hi
# → hello x86_64 via proot
```

## 4.4 第 2 步：glibc sysroot（不装发行版）

不需要完整的 Debian rootfs——AOT 只需要 glibc 运行时和开发文件。
**在原生 aarch64 侧下载 x86_64 的 deb 包**（速度快），解包即可：

```bash
G=https://deb.debian.org/debian/pool/main/g
curl -O $G/glibc/libc6_2.36-9+deb12u14_amd64.deb
curl -O $G/glibc/libc6-dev_2.36-9+deb12u14_amd64.deb
curl -O $G/gcc-12/libgcc-s1_12.2.0-14+deb12u1_amd64.deb
curl -O $G/gcc-12/libgcc-12-dev_12.5.0-11_amd64.deb
curl -O $G/gcc-12/libstdc++6_12.2.0-14+deb12u1_amd64.deb
# 以及 linux-libc-dev、zlib1g
for d in *.deb; do dpkg-deb -x "$d" sysroot/; done
```

### 坑 2：`/etc/resolv.conf` 缺失导致 DNS 全挂

sysroot 里没有 `resolv.conf`，pip 报：

```
Failed to resolve 'pypi.org' ([Errno -3] Temporary failure in name resolution)
```

补上即可：

```bash
mkdir -p sysroot/etc
echo -e "nameserver 8.8.8.8\nnameserver 1.1.1.1" > sysroot/etc/resolv.conf
```

### 坑 3：ld-linux 绝对符号链接

`sysroot/lib64/ld-linux-x86-64.so.2` 是指向绝对路径的符号链接，lld 无法在 sysroot 内解析：

```
ld.lld: error: cannot find /lib64/ld-linux-x86-64.so.2 inside .../sysroot
```

改成相对链接：

```bash
ln -sf ../lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 sysroot/lib64/ld-linux-x86-64.so.2
```

## 4.5 第 3 步：交叉编译（可选）

Termux 的 clang 带 X86 后端，配合 sysroot 就能编译 x86_64 程序：

```bash
clang --target=x86_64-linux-gnu --sysroot=$PWD/sysroot -fuse-ld=lld \
  -O2 -o prog prog.c \
  -L$PWD/sysroot/lib/x86_64-linux-gnu \
  -L$PWD/sysroot/usr/lib/x86_64-linux-gnu \
  -L$PWD/sysroot/usr/lib/gcc/x86_64-linux-gnu/12
```

运行：

```bash
qemu-x86_64 -L $PWD/sysroot ./prog
```

这一条不是必须的（后面用 Python 就够了），但调试 MediaTek 的 host 库时很有用。

## 4.6 第 4 步：x86_64 Python

用 [python-build-standalone](https://github.com/astral-sh/python-build-standalone)，
免去在模拟环境里跑 apt：

```bash
curl -LO https://github.com/astral-sh/python-build-standalone/releases/download/20261013/\
cpython-3.12.15+20261013-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz
tar xzf cpython-*.tar.gz          # → ./python/
qemu-x86_64 -L sysroot ./python/bin/python3 -V
# Python 3.12.15
```

## 4.7 第 5 步：LiteRT AOT 工具链

```bash
qemu-x86_64 -L sysroot ./python/bin/python3 -m pip install ai-edge-litert
```

会自动拉取 numpy / flatbuffers / ml_dtypes / protobuf 等依赖。

### 坑 4：`apply_plugin_main` 是裸 ELF

AOT 流水线内部会用 `subprocess` 直接执行一个 x86_64 ELF：

```
OSError: [Errno 8] Exec format error: .../ai_edge_litert/tools/apply_plugin_main
```

没有 binfmt 就无法直接执行。**解法：换成 shell 包装器转发给 qemu**：

```bash
mv apply_plugin_main apply_plugin_main.x86
cat > apply_plugin_main <<'EOF'
#!/data/data/com.termux/files/usr/bin/bash
exec /data/data/com.termux/files/usr/bin/qemu-x86_64 \
  -L "$X86ENV/sysroot" \
  "$X86ENV/python/lib/python3.12/site-packages/ai_edge_litert/tools/apply_plugin_main.x86" "$@"
EOF
chmod +x apply_plugin_main
```

## 4.8 第 6 步：装配 MediaTek SDK

`ai-edge-litert-sdk-mediatek` 的 `setup.py` 会在安装时从 S3 下载 SDK，
但有两个问题：只支持 linux x86（我们确实是 x86，可以过），以及会走 pip 构建隔离
（在无 binfmt 环境会失败）。

**直接手动装配**。先看它在 `__init__.py` 里期望的路径：

```python
version_map = {
    "v8": "v8_0_10/host/lib",
    "v9": "v9_0_3/host/lib",
}
sdk_path = pathlib.Path(__file__).parent.resolve() / "data"
```

SDK 包本身（66 MB，S3 直链）含有四个版本：

```
neuro_pilot/
  v8_0_8/  v8_0_10/  v9_0_2/  v9_0_3/
    host/lib/libneuron_adapter.so         x86_64   AOT 编译器
    host/lib/libc++.so.1  libunwind.so.1
    usdk/lib64/libneuronusdk_adapter.so   aarch64  设备端运行时
    */include/neuron/api/NeuronAdapter.h  官方头文件（89 函数）
```

装配：

```bash
SP=python/lib/python3.12/site-packages
mkdir -p $SP/ai_edge_litert_sdk_mediatek
cp <sdk_pkg>/__init__.py $SP/ai_edge_litert_sdk_mediatek/
cp -r neuro_pilot $SP/ai_edge_litert_sdk_mediatek/data
```

## 4.9 第 7 步：编译

```python
from ai_edge_litert.aot.aot_compile import aot_compile
from ai_edge_litert.aot.vendors.mediatek import target as mtk_target

t = mtk_target.Target(mtk_target.SocModel.MT6993)
res = aot_compile("model.tflite", output_dir="out", target=t, keep_going=True)
```

### 坑 5：DLA 临时目录

首跑报错：

```
ERROR: Failed to make DLA temporary directory, (errno=13)
```

Android 没有 `/tmp`。指定可写目录：

```bash
export MTKNN_ADAPTER_DLA_DIR=$PWD/dla_tmp
export TMPDIR=$PWD/dla_tmp
```

### 成功输出

```
INFO: Neuron api version: 9.2.1
INFO: Applying plugin with SDK version: np9.2.1
INFO: SoC model mt6993 is supported
CompilationResult(models_with_backend=[MediaTekBackend], failed_backends=[])
产物: model_MediaTek_MT6993_apply_plugin.tflite
```

产物里带编译标记：

```json
{"CompilerName": "adapter", "CompilerVersion": "9.2.1", "Neuron SHA1": "..."}
```

## 4.10 环境变量速查

MediaTek adapter 通过大量 `MTKNN_*` 环境变量控制行为（从库中 `strings` 提取）：

| 变量 | 作用 |
|---|---|
| `MTKNN_ADAPTER_DLA_PLATFORM` | **必需**，平台串，MT6993 用 `mt6993-a` |
| `MTKNN_ADAPTER_COMPILER_OPTIONS` | **注入编译器 flags**（见 [06 · 基准](06-benchmarks.md)） |
| `MTKNN_ADAPTER_MDLA_CORE_NUM` | MDLA 核心数（4） |
| `MTKNN_MDLA_ENABLE_MULTI_CORE` | 多核开关 |
| `MTKNN_ADAPTER_DLA_DIR` | DLA 输出目录 |
| `MTKNN_ADAPTER_CONFIG_TARGET` | 限制目标（如 `MDLA`，可屏蔽 VP6） |
| `MTKNN_ADAPTER_SET_DEBUG_LEVEL` | 调试日志级别 |
| `MTKNN_ADAPTER_GRAPH_DUMP_PATH` | 图 dump |
| `MTKNN_MVPU_*` (~50 个) | VP6 DSP 相关调优 |

> 如果手工构建 Neuron 模型（不走 LiteRT），不设 `MTKNN_ADAPTER_DLA_PLATFORM` 会直接失败：
> `Need to set environment variable MTKNN_ADAPTER_DLA_PLATFORM with a platform string
> before calling NeuronCompilation_create`

## 4.11 host 库里的 MT6993 模板

从 `libneuron_adapter.so` 里能提取出 MT6993 的完整编译参数模板：

```
--platform-config=mt6993 --l1-size-kb=7168 --num-mdla=4
--mdla-multicore-barrier=0 --relax-fp32 --opt=3 --fc-to-conv
--broadcast-act-wgt=3 --stable-linearize -O3 --opt-bw
--mlo=mode=recompose,patterns=... --multi-phase-fusion=Advanced
```

两点值得注意：

- `--num-mdla=4` 印证了 4 个 MDLA 卷积核心
- `--l1-size-kb=7168` 与设备实测 `Neuron_getL1MemorySizeKb()` 返回的 7168 KB 一致
- 目标格式包含 `mdla1.5 / 1.7 / 2.0 / 3.0`；`mt6993-a` 实际映射到 **MDLA_6_0**

这些参数通过 `MTKNN_ADAPTER_COMPILER_OPTIONS` 可以注入（实测生效），
但社区报告（[LiteRT #6462](https://github.com/google-ai-edge/LiteRT/issues/6462)）指出
公开工具链的优化程度远不及 Google 内部构建——本文实测也确实只拿到 1.2 倍收益，
详见 [06 · 基准](06-benchmarks.md)。
