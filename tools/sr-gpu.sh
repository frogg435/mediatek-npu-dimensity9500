#!/data/data/com.termux/files/usr/bin/bash
# sr.sh —— Termux 下的 AI 超分工具（基于 realesrgan-ncnn-vulkan / Mali GPU）
#
# 设计目标：
#   1. 默认"低干扰"模式，nice/ionice/taskset 三重限流，跑超分时不卡系统
#   2. 模型自动选：轻量模型 4 秒/张，重量模型 70 秒/张，按需求切换
#   3. 批量目录处理、跳过已完成、单文件失败不影响整体

set -uo pipefail

# ---------- 默认配置 ----------
BIN="${PREFIX:-/data/data/com.termux/files/usr}/bin/realesrgan-ncnn-vulkan"
MODEL_DIR="${PREFIX:-/data/data/com.termux/files/usr}/opt/realesrgan-ncnn-vulkan/models"

PRESET="fast"
SCALE=4
TILE=512
FORMAT=""
THREADS="1:1:1"
LOW_IMPACT=1
FORCE=0
OUTDIR=""
VERBOSE=0
CPU_LIST="0-5"        # 留给系统 6-7 号核，UI 更流畅

# 模型预设
model_for_preset() {
    case "$1" in
        fast)    echo "realesr-animevideov3"  ;;   # 1.2MB，最快，动漫/插画/截图极佳
        general) echo "realesrnet-x4plus"     ;;   # 折中
        anime)   echo "realesrgan-x4plus-anime" ;; # 动漫专用高质量
        quality) echo "realesrgan-x4plus"     ;;   # 真实照片，最慢最好
        *) echo "" ;;
    esac
}

usage() {
    cat <<'EOF'
用法: sr.sh [选项] <输入文件或目录> [输出]

  -m <预设>   模型预设: fast(默认) | general | anime | quality
              fast    = realesr-animevideov3     ~4 秒/张   (动漫/截图/插画)
              general = realesrnet-x4plus        ~15 秒/张
              anime   = realesrgan-x4plus-anime  ~70 秒/张  (动漫高质量)
              quality = realesrgan-x4plus        ~70 秒/张  (真实照片)
  -s <2|3|4>  放大倍率 (默认 4)
  -t <尺寸>   分块大小 (默认 512，实测比自动快 18%)
  -f <格式>   输出格式: png | jpg | webp (默认沿用源格式)
  -j <L:P:S>  线程 加载:处理:保存 (默认 1:1:1，最省资源)
  -F          强制覆盖已存在的输出
  -v          显示 realesrgan 详细输出
  --full      全速模式：不加 nice/ionice/taskset（会明显卡系统）
  -h          显示帮助

示例:
  sr.sh photo.jpg                     # 单张，输出 photo_x4.png
  sr.sh -m quality -s 2 photo.jpg     # 照片质量模式，2 倍
  sr.sh -F ~/DCIM/album               # 整个目录
  sr.sh -m fast -f webp -s 2 in.png out.webp

说明:
  默认会限制到 0-5 号 CPU 核 + 最低优先级，避免抢系统资源。
  大图处理时请保持 Termux 在前台，锁屏可能导致进程被冻结。
EOF
}

# ---------- 参数解析 ----------
while [ $# -gt 0 ]; do
    case "$1" in
        -m) PRESET="${2:?}"; shift 2 ;;
        -s) SCALE="${2:?}"; shift 2 ;;
        -t) TILE="${2:?}"; shift 2 ;;
        -f) FORMAT="${2:?}"; shift 2 ;;
        -j) THREADS="${2:?}"; shift 2 ;;
        -F) FORCE=1; shift ;;
        -v) VERBOSE=1; shift ;;
        --full) LOW_IMPACT=0; shift ;;
        -h|--help) usage; exit 0 ;;
        -*) echo "未知选项: $1" >&2; usage >&2; exit 2 ;;
        *) break ;;
    esac
done

[ $# -ge 1 ] || { usage >&2; exit 2; }
INPUT="$1"
OUTPUT="${2:-}"

MODEL="$(model_for_preset "$PRESET")"
[ -n "$MODEL" ] || { echo "未知模型预设: $PRESET" >&2; exit 2; }
case "$SCALE" in 2|3|4) ;; *) echo "倍率必须是 2/3/4" >&2; exit 2 ;; esac

[ -x "$BIN" ] || { echo "找不到 realesrgan-ncnn-vulkan，请先: pkg install realesrgan-ncnn-vulkan" >&2; exit 3; }
[ -e "$INPUT" ] || { echo "输入不存在: $INPUT" >&2; exit 3; }

# ---------- 组装执行前缀（降低系统干扰）----------
PREFIX_CMD=()
if [ "$LOW_IMPACT" = 1 ]; then
    command -v nice    >/dev/null 2>&1 && PREFIX_CMD+=(nice -n 19)
    command -v ionice  >/dev/null 2>&1 && PREFIX_CMD+=(ionice -c 3)   # idle IO
    if command -v taskset >/dev/null 2>&1; then
        PREFIX_CMD+=(taskset -c "$CPU_LIST")
        # 关键：ncnn 用 OpenMP，限制 CPU 亲和性会触发 libomp 断言崩溃，
        # 必须同时关闭它的自动亲和性绑定。
        export KMP_AFFINITY=disabled
    fi
fi

run_one() {
    local src="$1" dst="$2"
    local t0 t1 ms
    local -a args=(-i "$src" -o "$dst" -n "$MODEL" -s "$SCALE" -t "$TILE" -j "$THREADS" -m "$MODEL_DIR")
    [ -n "$FORMAT" ] && args+=(-f "$FORMAT")
    [ "$VERBOSE" = 1 ] && args+=(-v)

    printf '  → %s ... ' "$(basename "$dst")"
    t0=$(date +%s%N)
    if [ ${#PREFIX_CMD[@]} -gt 0 ]; then
        "${PREFIX_CMD[@]}" "$BIN" "${args[@]}" >/dev/null 2>&1
    else
        "$BIN" "${args[@]}" >/dev/null 2>&1
    fi
    local rc=$?
    t1=$(date +%s%N)
    ms=$(( (t1 - t0) / 1000000 ))
    if [ $rc -eq 0 ] && [ -f "$dst" ]; then
        printf '完成 %d.%ds\n' $((ms/1000)) $(( (ms%1000)/100 ))
        return 0
    else
        printf '失败 (rc=%d)\n' $rc
        return 1
    fi
}


# 推导输出扩展名：优先 -f，否则沿用源格式（bmp 等不支持的回退 png）
out_ext_for() {
    local src="$1"
    if [ -n "$FORMAT" ]; then echo "$FORMAT"; return; fi
    local e="${src##*.}"
    e="$(printf '%s' "$e" | tr 'A-Z' 'a-z')"
    case "$e" in
        png|jpg|jpeg|webp) echo "$e" ;;
        *) echo "png" ;;
    esac
}

# ---------- 单文件模式 ----------
if [ -f "$INPUT" ]; then
    src="$INPUT"
    if [ -n "$OUTPUT" ]; then
        dst="$OUTPUT"
    else
        base="$(basename "$src")"
        stem="${base%.*}"
        ext="$(out_ext_for "$src")"
        dst="$(dirname "$src")/${stem}_x${SCALE}.${ext}"
    fi
    mkdir -p "$(dirname "$dst")" 2>/dev/null

    if [ -f "$dst" ] && [ "$FORCE" = 0 ]; then
        echo "输出已存在，跳过: $dst  (用 -F 强制覆盖)"
        exit 0
    fi

    echo "模型: $MODEL   倍率: x$SCALE   分块: $TILE"
    echo "低干扰模式: $([ "$LOW_IMPACT" = 1 ] && echo "开 (CPU $CPU_LIST + nice19 + idle IO)" || echo "关 (全速)")"
    echo "输入: $src"
    run_one "$src" "$dst" || exit 4
    echo
    if command -v ls >/dev/null; then ls -lh "$dst" | awk '{print "  输出大小: "$5}'; fi
    exit 0
fi

# ---------- 目录批量模式 ----------
srcdir="$INPUT"
if [ -n "$OUTPUT" ]; then
    dstdir="$OUTPUT"
elif [ -n "$OUTDIR" ]; then
    dstdir="$OUTDIR"
else
    dstdir="${srcdir%/}_x${SCALE}"
fi
mkdir -p "$dstdir" 2>/dev/null || { echo "无法创建输出目录: $dstdir" >&2; exit 3; }

echo "模型: $MODEL   倍率: x$SCALE   分块: $TILE"
echo "低干扰模式: $([ "$LOW_IMPACT" = 1 ] && echo "开 (CPU $CPU_LIST + nice19 + idle IO)" || echo "关 (全速)")"
echo "输入目录: $srcdir"
echo "输出目录: $dstdir"
echo

ext_re="\.(png|jpe?g|webp|bmp)$"
total=0; ok=0; skip=0; fail=0
t_start=$(date +%s)

while IFS= read -r -d '' src; do
    base="$(basename "$src")"
    stem="${base%.*}"
    outext="$(out_ext_for "$src")"
    dst="$dstdir/${stem}_x${SCALE}.${outext}"
    total=$((total+1))
    if [ -f "$dst" ] && [ "$FORCE" = 0 ]; then
        printf '  · %s 已存在，跳过\n' "$base"
        skip=$((skip+1)); continue
    fi
    if run_one "$src" "$dst"; then ok=$((ok+1)); else fail=$((fail+1)); fi
done < <(find "$srcdir" -maxdepth 1 -type f -regextype posix-extended -iregex ".*${ext_re}" -print0 2>/dev/null)

t_end=$(date +%s)
echo
echo "================ 汇总 ================"
echo "  总计: $total   成功: $ok   跳过: $skip   失败: $fail"
echo "  耗时: $((t_end - t_start)) 秒"
echo "  输出: $dstdir"
