# sr.sh — Termux AI 超分工具

基于 `realesrgan-ncnn-vulkan` + Mali GPU（Vulkan 计算），在本机实测可用。

设备：vivo V2548A / 天玑 9500 (MT6993) / Mali-G1-Ultra MC12 / Android 16

## 安装

```bash
pkg install realesrgan-ncnn-vulkan     # 已装
ln -sf ~/sr-tool/sr.sh $PREFIX/bin/sr  # 已装，可直接用 sr 命令
```

## 快速开始

```bash
sr photo.jpg                  # 单张，输出 photo_x4.jpg
sr -s 2 photo.jpg             # 2 倍（更快）
sr ~/DCIM/Camera              # 整个目录
sr -m quality -s 2 photo.jpg  # 真实照片质量模式
sr -f webp in.png out.webp    # 指定输出格式
```

## 模型预设（本机实测，720p 输入 ×4）

| 预设 | 实际模型 | 耗时 | 适用场景 |
|---|---|---|---|
| `fast`（默认） | realesr-animevideov3 | **3.3 秒** | 动漫、截图、插画、UI 图 |
| `general` | realesrnet-x4plus | ~15 秒 | 折中 |
| `anime` | realesrgan-x4plus-anime | ~70 秒 | 动漫高质量 |
| `quality` | realesrgan-x4plus | ~70 秒 | 真实照片，最慢最好 |

`fast` 比 `quality` **快约 20 倍**。日常优先用 `fast`。

## 低干扰模式（默认开启）

跑超分时系统不卡的关键，三重限流：

| 手段 | 作用 |
|---|---|
| `taskset -c 0-5` | 只占 6 个核，留 2 个给系统 UI |
| `nice -n 19` | 最低 CPU 优先级 |
| `ionice -c 3` | 空闲级磁盘 IO |

**注意**：`taskset` 会和 ncnn 的 OpenMP 冲突（`kmp_affinity.cpp` 断言 → SIGABRT）。
脚本里已通过 `export KMP_AFFINITY=disabled` 解决。自己手写命令时务必带上这个变量。

要全速跑（会明显卡系统）：`sr --full ...`

## 内部优化

| 参数 | 值 | 原因 |
|---|---|---|
| `-t` 分块 | 512 | 实测比自动（`-t 0`）快 18%：3957ms vs 4843ms |
| `-j` 线程 | 1:1:1 | 默认的 1:2:2 会多抢 CPU，限制后体感更顺 |
| `-m` 模型目录 | 绝对路径 | 避免依赖当前工作目录 |

## 全部选项

```
-m <fast|general|anime|quality>   模型预设
-s <2|3|4>                        放大倍率
-t <尺寸>                         分块大小（默认 512）
-f <png|jpg|webp>                 输出格式（默认沿用源格式）
-j <L:P:S>                        线程 加载:处理:保存
-F                                强制覆盖已存在输出
-v                                realesrgan 详细输出
--full                            全速模式，不加限流
-h                                帮助
```

## 行为说明

- **单文件**：不指定输出时，输出到源文件同目录，命名 `<原名>_x<倍率>.<扩展名>`
- **目录**：递归深度 1，输出到 `<源目录>_x<倍率>/`
- **断点续跑**：输出已存在则跳过（用 `-F` 覆盖），中断后重跑不会重复劳动
- **容错**：单张失败不影响其余文件，最后汇总成功/跳过/失败数量

## 已知限制

- 需要保持 Termux 在**前台**，锁屏可能被系统冻结
- 1080p ×4 输出约 7680×4320，`quality` 模型需要 ~2.5 分钟，建议 `fast` 或 `-s 2`
- 输出 PNG 体积较大（30MB+），需要小体积用 `-f jpg` 或 `-f webp`

## NPU 为什么没用上

详见 [../npu-test/NPU-NOTES.md](../npu-test/NPU-NOTES.md)。简要结论：

- GPU（Vulkan）路径完全可用 ✅
- NPU 能连上、能算，但 vivo 只对第三方开放了 `ADD`/`MUL` 两个算子，
  **卷积不支持**，所以跑不了超分
