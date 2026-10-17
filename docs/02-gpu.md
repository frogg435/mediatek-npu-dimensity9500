# 02 · 无 root 环境下调用 GPU

Android 上从 Termux（`untrusted_app` 域）调用 GPU，**不需要 root**，
但需要绕开两道限制：**linker namespace** 和 **DAC 权限**。

本章给出三条可用路径的完整实现，以及哪些路走不通。

---

## 2.1 权限边界：什么能碰、什么不能

先明确边界，避免在错误的方向浪费时间。

### ✅ 能直接用的（无需任何处理）

| 资源 | 位置 | 原因 |
|---|---|---|
| Vulkan loader | `/system/lib64/libvulkan.so` | `/system/lib64` 在 namespace 允许列表内 |
| EGL / GLES | `/system/lib64/libEGL.so`, `libGLESv2.so` | 同上 |
| GPU 设备节点 | `/dev/mali0` | 权限 `crw-rw-rw-`，无 SELinux 拦截 |

### ⚠️ 需要搬运的

| 资源 | 位置 | 问题 |
|---|---|---|
| 厂商 OpenCL | `/vendor/lib64/libOpenCL.so` | `/vendor/lib64` **不在** namespace 允许列表 |
| MTK 图形库 | `/vendor/lib64/egl/libGLES_mali.so` | 同上 |

### ❌ 做不到的（需 root）

| 目标 | 原因 |
|---|---|
| `/dev/dri/card0` | 属主 `system:system`，DAC 就过不去；且无 `renderD128` |
| 直接 ioctl GPU 内核驱动 | 无设备节点 + SELinux |
| 改 GPU 频率/调速器 | 需写 `/sys/class/devfreq`，权限拒绝 |
| 打开 `/dev/apusys`（NPU） | 节点不存在于用户态可见范围 |

---

## 2.2 linker namespace：核心障碍

Android 的 linker 对每个进程维护命名空间，只有 `permitted_paths` 内的库可被加载。
实测（从 dlopen 报错信息里拿到）：

```
permitted_paths = "/system/lib64/drm : /system/lib64/extractors : /system/lib64/hw :
                   /system_ext/lib64 : /system/framework : /system/app :
                   /vendor/framework : /vendor/app :
                   /data : /mnt/expand : /apex/com.android.runtime/lib64/bionic"
```

**关键观察**：

- ✅ `/system/lib64`、`/system_ext/lib64` → 直接用
- ❌ `/vendor/lib64` → **不在列表**
- ✅ **`/data`** → 在列表（这就是解法）

验证：

```python
import ctypes
ctypes.CDLL("/system/lib64/libvulkan.so")     # OK
ctypes.CDLL("/vendor/lib64/libOpenCL.so")     # ❌ is not accessible for the namespace
```

解法：**把 vendor 库复制到 `/data` 下**（Termux 的 `$PREFIX`），
再 patch 它的 `RUNPATH` 指向同目录，使依赖也在 `/data` 内解析。

---

## 2.3 方法一：Vulkan（最推荐）

Vulkan loader 在 `/system/lib64`，**开箱即用**。

### 探测（Python + ctypes）

```python
import ctypes
from ctypes import *

vk = ctypes.CDLL("/system/lib64/libvulkan.so")

class VkApplicationInfo(Structure):
    _fields_ = [("sType", c_int), ("pNext", c_void_p),
                ("pApplicationName", c_char_p), ("applicationVersion", c_uint32),
                ("pEngineName", c_char_p), ("engineVersion", c_uint32),
                ("apiVersion", c_uint32)]

class VkInstanceCreateInfo(Structure):
    _fields_ = [("sType", c_int), ("pNext", c_void_p), ("flags", c_uint32),
                ("pApplicationInfo", POINTER(VkApplicationInfo)),
                ("enabledLayerCount", c_uint32), ("ppEnabledLayerNames", c_void_p),
                ("enabledExtensionCount", c_uint32), ("ppEnabledExtensionNames", c_void_p)]

ai = VkApplicationInfo(0, None, b"probe", 1, b"none", 1, (1 << 22) | (3 << 12))  # Vulkan 1.3
ci = VkInstanceCreateInfo(1, None, 0, pointer(ai), 0, None, 0, None)

inst = c_void_p()
rc = vk.vkCreateInstance(byref(ci), None, byref(inst))     # 0 = VK_SUCCESS
n = c_uint32(0)
vk.vkEnumeratePhysicalDevices(inst, byref(n), None)        # n = 物理设备数
print(f"vkCreateInstance rc={rc}, 物理设备数={n.value}")
```

实测输出：

```
vkCreateInstance rc=0, 物理设备数=1
```

### 设备能力

用 `realesrgan-ncnn-vulkan` 的探测输出（ncnn 的 Vulkan 后端）：

```
[0 Mali-G1-Ultra MC12]  queueC=0[2]  queueG=0[2]  queueT=0[2]
[0 Mali-G1-Ultra MC12]  fp16-p/s/a=1/1/1  int8-p/s/a=1/1/1
[0 Mali-G1-Ultra MC12]  subgroup=16  basic=1  vote=1  ballot=1  shuffle=1
```

| 能力 | 值 | 含义 |
|---|---|---|
| `fp16-p/s/a` | 1/1/1 | fp16 存储/算术全支持 |
| `int8-p/s/a` | 1/1/1 | int8 存储/算术全支持 |
| `subgroup` | 16 | 子组大小 16，支持 vote/ballot/shuffle |
| `basic` | 1 | 支持基础 Vulkan compute |

### 直接上手：ncnn 工具链

Termux 仓库已有现成的 Vulkan 应用，无需自己写 compute shader：

```bash
pkg install realesrgan-ncnn-vulkan    # 超分
pkg install rife-ncnn-vulkan-nihui    # 补帧
```

### 无 root 下的调优参数

```bash
realesrgan-ncnn-vulkan -i in.png -o out.png \
  -n realesr-animevideov3 -s 4 -t 512 -j 1:1:1
```

| 参数 | 建议值 | 原因 |
|---|---|---|
| `-t` 分块 | **512** | 实测比自动挡快 18%（3957 ms vs 4843 ms） |
| `-j` 线程 | **1:1:1** | 默认 `1:2:2` 会多抢 CPU |
| `-n` 模型 | `realesr-animevideov3` | 比 `x4plus` 快约 20 倍 |

### ⚠️ 坑：`taskset` 会让进程崩溃

想限制 CPU 占用时如果用 `taskset`，ncnn 会因为 OpenMP 亲和性断言直接 SIGABRT：

```
OMP: Error #13: Assertion failure at kmp_affinity.cpp(4434).
```

**必须同时关闭 OpenMP 亲和性**：

```bash
export KMP_AFFINITY=disabled
taskset -c 0-5 realesrgan-ncnn-vulkan ...
```

完整的低干扰封装：

```bash
nice -n 19 ionice -c 3 taskset -c 0-5 \
  env KMP_AFFINITY=disabled \
  realesrgan-ncnn-vulkan -i in.png -o out.png -t 512 -j 1:1:1
```

---

## 2.4 方法二：OpenCL

Vulkan 覆盖了大多数场景，但想直接用 OpenCL C 写 kernel，或跑 clBLAS/clblast 类库时需要它。

### 步骤 1：搬库

Termux 有现成包：

```bash
pkg install opencl-vendor-driver clinfo
```

它在安装时做了三件事：

1. 把 `/vendor/lib64` 下的 OpenCL 相关库复制到 `$PREFIX/opt/vendor/lib`
2. `patchelf` 修正每个库的 `RUNPATH` 指向该目录
3. 写入 ICD 描述文件

```
$PREFIX/etc/OpenCL/vendors/vendor.icd
  → /data/data/com.termux/files/usr/opt/vendor/lib/libOpenCL.so
```

### 步骤 2：验证

```bash
clinfo | head -20
```

```
Number of platforms                               1
  Platform Name                                   ARM Platform
  Platform Vendor                                 ARM
  Platform Version                                OpenCL 3.0 v1.r54p1
  Device Name                                     Mali-G1-Ultra MC12 r0p1
  Max compute units                               12
  Global memory size                              11854016512 (11.04GiB)
  Local memory size                               32768 (32KiB)
```

### ⚠️ 坑：绝对不要设 `LD_LIBRARY_PATH`

这是最容易踩的坑。一旦设置，ocl-icd 加载厂商库会失败：

```
dlopen failed: cannot find "libbinder_ndk.so" from verneed[0]
  in DT_NEEDED list for ".../opt/vendor/lib/libOpenCL.so"
```

原因：MTK 版 `libOpenCL.so` 依赖 `/system/lib64/libbinder_ndk.so`，
`LD_LIBRARY_PATH` 会干扰 namespace 的**符号版本（verneed）解析**。

**不设这个变量，一切正常。**

### 步骤 3：写自己的 kernel

```bash
pkg install opencl-headers
clang my_kernel.c -o my_kernel -lOpenCL -O2
```

一个完整的向量加法示例：

```c
#include <CL/cl.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static const char *src =
"__kernel void vadd(__global const float *a, __global const float *b,"
"                   __global float *c, int n) {"
"  int i = get_global_id(0); if (i < n) c[i] = a[i] + b[i]; }";

#define CK(x) do{ cl_int _r=(x); if(_r!=CL_SUCCESS){ \
    printf("FAIL %s -> %d\n",#x,_r); return 1; } }while(0)

int main(void) {
    cl_platform_id p; cl_device_id d; cl_uint np=0, nd=0;
    CK(clGetPlatformIDs(1, &p, &np));
    CK(clGetDeviceIDs(p, CL_DEVICE_TYPE_GPU, 1, &d, &nd));

    char name[256]={0};
    clGetDeviceInfo(d, CL_DEVICE_NAME, sizeof(name), name, NULL);
    printf("Device: %s\n", name);

    const int N = 1 << 20;
    size_t bytes = N * sizeof(float);
    float *A = malloc(bytes), *B = malloc(bytes), *C = malloc(bytes);
    for (int i = 0; i < N; i++) { A[i] = i; B[i] = 2.0f * i; }

    cl_int err;
    cl_context ctx = clCreateContext(NULL, 1, &d, NULL, NULL, &err); CK(err);
    cl_command_queue q = clCreateCommandQueue(ctx, d, CL_QUEUE_PROFILING_ENABLE, &err); CK(err);
    cl_mem a = clCreateBuffer(ctx, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR, bytes, A, &err); CK(err);
    cl_mem b = clCreateBuffer(ctx, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR, bytes, B, &err); CK(err);
    cl_mem c = clCreateBuffer(ctx, CL_MEM_WRITE_ONLY, bytes, NULL, &err); CK(err);

    cl_program pr = clCreateProgramWithSource(ctx, 1, &src, NULL, &err); CK(err);
    err = clBuildProgram(pr, 1, &d, "", NULL, NULL);
    if (err != CL_SUCCESS) {
        char log[8192] = {0};
        clGetProgramBuildInfo(pr, d, CL_PROGRAM_BUILD_LOG, sizeof(log), log, NULL);
        printf("BUILD ERR:\n%s\n", log); return 1;
    }

    cl_kernel k = clCreateKernel(pr, "vadd", &err); CK(err);
    CK(clSetKernelArg(k, 0, sizeof(a), &a));
    CK(clSetKernelArg(k, 1, sizeof(b), &b));
    CK(clSetKernelArg(k, 2, sizeof(c), &c));
    CK(clSetKernelArg(k, 3, sizeof(int), &N));

    size_t gsz = N;
    cl_event ev;
    CK(clEnqueueNDRangeKernel(q, k, 1, NULL, &gsz, NULL, 0, NULL, &ev));
    CK(clFinish(q));

    cl_ulong t0 = 0, t1 = 0;
    clGetEventProfilingInfo(ev, CL_PROFILING_COMMAND_START, sizeof(t0), &t0, NULL);
    clGetEventProfilingInfo(ev, CL_PROFILING_COMMAND_END,   sizeof(t1), &t1, NULL);
    CK(clEnqueueReadBuffer(q, c, CL_TRUE, 0, bytes, C, 0, NULL, NULL));

    int ok = 1;
    for (int i = 0; i < N; i++)
        if (fabsf(C[i] - (A[i] + B[i])) > 1e-3f) { ok = 0; break; }

    double ms = (t1 - t0) / 1e6;
    printf("kernel: %.3f ms | %.2f GB/s | verify: %s\n",
           ms, 3.0 * bytes / ((t1 - t0) / 1e9) / 1e9, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
```

编译运行：

```bash
clang vadd.c -o vadd -lOpenCL -O2
./vadd
```

实测：

```
Device: Mali-G1-Ultra MC12 r0p1
kernel: 0.428 ms | 29.39 GB/s | verify: PASS
```

### GPU 支持的关键 OpenCL 扩展

| 扩展 | 用途 |
|---|---|
| `cl_khr_fp16` | 半精度，NPU/GPU 推理常用 |
| `cl_arm_matrix_multiply` | ARM 矩阵乘加速 |
| `cl_khr_integer_dot_product` (2.0.0) | int8 点积，量化推理关键 |
| `cl_khr_subgroups` | 子组通信 |
| `cl_khr_command_buffer` | 减少提交开销 |
| `cl_arm_import_memory_dma_buf` | 零拷贝接入 Android 图形缓冲 |

---

## 2.5 方法三：EGL / GLES

`libEGL.so`、`libGLESv2.so` 都在 `/system/lib64`，可直接 `dlopen`。
适合需要和 Android `Surface` / `ANativeWindow` 交互的场景
（例如把 GPU 结果直接画到屏幕）。

Termux 下典型用途是配合 `termux-x11` 或离屏 FBO 做渲染。
纯计算不用这条路——直接用 Vulkan compute 或 OpenCL 更高效。

---

## 2.6 三条路径对比

| | Vulkan | OpenCL | EGL/GLES |
|---|---|---|---|
| 需搬库 | ❌ 不用 | ✅ 需要 | ❌ 不用 |
| 需设环境变量 | ❌ | ⚠️ 绝不能设 `LD_LIBRARY_PATH` | ❌ |
| 计算能力 | 完整 compute | 完整 compute | 图形为主 |
| 生态工具 | ncnn（超分/补帧） | clblast、自写 kernel | 图形应用 |
| 本文实测 | ✅ 可用 | ✅ 可用 | 未测 |
| **推荐度** | ⭐⭐⭐ | ⭐⭐ | ⭐ |

---

## 2.7 实测性能参考

### Vulkan（realesrgan-ncnn-vulkan，720p 输入 ×4）

| 模型 | 耗时 | 场景 |
|---|---|---|
| `realesr-animevideov3` | **3.3 秒** | 动漫/截图/插画 |
| `realesrnet-x4plus` | ~15 秒 | 折中 |
| `realesrgan-x4plus` | ~72 秒 | 真实照片，最慢最好 |

### OpenCL（自己写的 kernel）

| 任务 | 耗时 | 有效带宽 |
|---|---|---|
| 1M 元素 float 向量加 | 0.428 ms | 29.39 GB/s |

### 与 NPU 路线对比

同样的超分任务，用 AOT 编译后在 NPU 上跑（见 [07 · 超分实战](07-super-resolution.md)）：

| 路径 | 2364×1773 整图 ×4 |
|---|---|
| GPU (Vulkan, x4plus) | ~72 秒 / 720p |
| GPU (Vulkan, animevideov3) | ~3.3 秒 / 720p |
| **NPU (Real-ESRGAN x4v3)** | **4.37 秒 / 整张 2364×1773** |

NPU 路线在这里快得多，而且**不占用 GPU、不影响系统流畅度**。
GPU 路线的优势是零配置、开箱可用。

---

## 2.8 一句话总结

> **无 root 调 GPU 的关键只有两条：**
> **① 优先挑 `/system` 下的库（Vulkan/EGL 全部可用）；**
> **② 必须用 `/vendor` 的库时，复制到 `/data` 并 patch RUNPATH，且千万别设 `LD_LIBRARY_PATH`。**
