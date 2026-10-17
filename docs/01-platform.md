# 01 · 平台探测

## 1.1 设备识别

在 Termux 里探测到的基本信息：

```bash
$ uname -a
Linux localhost 6.12.58-android16-6-g0a092cc0037a-abogki521987229-4k #1 SMP PREEMPT aarch64

$ getprop ro.board.platform      # mt6993
$ getprop ro.soc.model           # MT6993
$ getprop ro.product.model       # V2548A
$ getprop ro.build.version.release   # 16
```

`MT6993` 对应 **天玑 9500**（注意：天玑 9400 是 MT6991，容易混淆）。

| 项 | 值 |
|---|---|
| SoC | MT6993 = Dimensity 9500（台积电 N3P） |
| CPU | 1× C1-Ultra + 3× C1-Premium + 4× C1-Pro，全大核，`nproc=8` |
| CPU 频率 | 最高 4,210,000 kHz (4.21 GHz)，用户态**不可调** |
| GPU | **Mali-G1-Ultra MC12**（ARM 2025 年把 Immortalis 系列改名后的新旗舰） |
| NPU | MediaTek NPU 990（APU，含 MDLA 卷积核心与 VP6 DSP） |
| 内存 | ~11 GiB 可寻址（OpenCL 报 11854016512 字节） |

## 1.2 关键属性

```bash
# GPU 相关
[ro.hardware.egl]:      [mali]
[ro.hardware.vulkan]:   [mali]
[ro.hwui.use_vulkan]:   [true]

# NPU 相关
[init.svc.neuralnetworks_hal_mtk_neuron_service]: [running]
[debug.mtk_tflite.target_nnapi]: [29]
[persist.device_config.nnapi_native.current_feature_level]: [7]
```

`neuralnetworks_hal_mtk_neuron_service` 正在运行，说明 NeuroPilot HAL 服务是活的。

## 1.3 设备节点可达性

```
/dev/mali0            crw-rw-rw-  system graphics   →  可打开（无 SELinux 拦截）
/dev/dri/card0        crw-rw----  system system     →  拒绝（DAC，属组不匹配）
/dev/dri/renderD128                                   不存在
/dev/apusys  /dev/mdla  /dev/vpu                      均不存在
```

**关键结论：GPU 有设备节点可直接访问；NPU 没有任何用户态设备节点，只能走 HAL 服务。**

想直接打开 `/dev/apusys` 需要 root + SELinux 策略模块
（可参考 `Peterc3-dev/phone2a-apu-unlock` 那类 KernelSU 方案），本文不涉及 root。

## 1.4 进程 SELinux 域

```
$ cat /proc/self/attr/current
u:r:untrusted_app_27:s0:c42,c257,c512,c768
```

Termux 运行在 `untrusted_app_27` 域。这个域对 `/vendor/lib64` 的 linker 访问、
对 vendor HAL 的 binder 调用都有额外限制——这正是后面几条路线的分水岭。

## 1.5 一个容易踩的坑：文件沙箱假象

如果在受限沙箱里测试设备节点，可能得到与真实终端不同的结果。例如在受限环境下：

```
/dev/mali0  FAIL [Errno 13] Permission denied
```

但模式是 `crw-rw-rw-`，DAC 不可能拒绝。在真实 Termux shell 中复测：

```
/dev/mali0  OPEN OK
```

**结论：排查权限问题必须在真实 shell 中验证，不要相信沙箱内的 EACCES。**

## 1.6 探测用到的命令

```bash
# SoC / GPU
getprop | grep -iE 'vulkan|gpu|egl|apu|npu|neuron'
ls -l /dev/mali0 /dev/dri/

# NPU 服务
getprop init.svc.neuralnetworks_hal_mtk_neuron_service
cmd -l | grep -iE 'neuron|apu'

# 厂商 NPU 库
ls /system_ext/lib64/ | grep -iE 'neuron|apu'
ls /vendor/lib64/ | grep -iE 'neuron|mdla|mvpu'
```
