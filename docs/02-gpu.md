# 02 · GPU 访问

Android 上从 Termux 调用 GPU 有两道坎：**linker namespace** 和 **库的架构差异**。
弄清楚这两点，后续 NPU 部分也会复用同样的思路。

## 2.1 核心障碍：linker namespace

Android 的 linker 对每个进程维护命名空间，只有 `permitted_paths` 内的库可被加载。实测：

```
permitted_paths = "/system/lib64/drm : /system/lib64/extractors : /system/lib64/hw :
                   /system_ext/lib64 : /system/framework : ... :
                   /vendor/framework : ... : /data : /mnt/expand : ..."
```

可以看到：

- ✅ `/system/lib64`、`/system_ext/lib64`、`/data` 都在允许列表内
- ❌ **`/vendor/lib64` 不在**

验证：`dlopen("/vendor/lib64/libOpenCL.so")` 直接失败

```
dlopen failed: library "/vendor/lib64/libOpenCL.so" ...
is not accessible for the namespace "(default)"
```

## 2.2 OpenCL：把厂商库搬进 `/data`

既然 `/data` 允许，解法就是**把 vendor 库复制到 `$PREFIX`**。Termux 已有现成包做这件事：

```bash
pkg install opencl-vendor-driver clinfo
clinfo | head -20
```

它在安装时把 `/vendor/lib64` 下的 OpenCL 相关库复制到
`$PREFIX/opt/vendor/lib`，patch runpath，并写入 ICD 文件：

```
$PREFIX/etc/OpenCL/vendors/vendor.icd
  → /data/data/com.termux/files/usr/opt/vendor/lib/libOpenCL.so
```

### ⚠️ 关键的坑：不要设 `LD_LIBRARY_PATH`

一旦 `export LD_LIBRARY_PATH`，ocl-icd 加载厂商库时会失败：

```
dlopen failed: cannot find "libbinder_ndk.so" from verneed[0]
  in DT_NEEDED list for ".../opt/vendor/lib/libOpenCL.so"
```

MTK 版 `libOpenCL.so` 依赖 `/system/lib64/libbinder_ndk.so`，
`LD_LIBRARY_PATH` 会干扰 namespace 的符号版本解析。**不设这个变量就一切正常。**

### 实测结果

```
Platform Name    ARM Platform
Platform Version OpenCL 3.0 v1.r54p1
Device Name      Mali-G1-Ultra MC12 r0p1
Max compute units 12
Global memory     11.04 GiB
```

关键扩展：`cl_khr_fp16`、`cl_arm_matrix_multiply`、`cl_khr_integer_dot_product`、
`cl_khr_subgroups`、`cl_khr_command_buffer`。

一个 1M 元素的浮点向量加法 kernel：

```
kernel time: 0.428 ms | effective BW: 29.39 GB/s | verify: PASS
```

## 2.3 Vulkan：开箱即用

和 OpenCL 不同，`/system/lib64/libvulkan.so` 在允许路径内，**可直接 dlopen**：

```
vkCreateInstance -> 0
vkEnumeratePhysicalDevices -> 0, count = 1
```

用 `realesrgan-ncnn-vulkan`（Termux 仓库有）实测：

```
[0 Mali-G1-Ultra MC12]  fp16-p/s/a=1/1/1  int8-p/s/a=1/1/1
[0 Mali-G1-Ultra MC12]  subgroup=16  basic=1  vote=1  ballot=1  shuffle=1
```

## 2.4 GPU 路线的性能参考

`realesrgan-ncnn-vulkan`，720p 输入 ×4：

| 模型 | 耗时 | 场景 |
|---|---|---|
| `realesr-animevideov3` | **3.3 秒** | 动漫/截图/插画 |
| `realesrnet-x4plus` | ~15 秒 | 折中 |
| `realesrgan-x4plus` | ~72 秒 | 真实照片 |

调优要点：

- **`-t 512`**：实测比自动挡快 18%（3957ms vs 4843ms）
- **`-j 1:1:1`**：默认的 `1:2:2` 会多抢 CPU
- **`taskset` 会崩**：ncnn 用 OpenMP，限制 CPU 亲和性触发 `kmp_affinity.cpp` 断言 → SIGABRT。
  必须同时 `export KMP_AFFINITY=disabled`

## 2.5 小结

| 库 | 位置 | 能否直接 dlopen |
|---|---|---|
| `/system/lib64/libvulkan.so` | 系统 | ✅ |
| `/system_ext/lib64/libneuron*.so` | system_ext | ✅ |
| `/vendor/lib64/libOpenCL.so` | vendor | ❌ 需复制到 `/data` |

**`/system_ext/lib64` 可直达这一点，是后面 NPU 研究能进行下去的前提。**
