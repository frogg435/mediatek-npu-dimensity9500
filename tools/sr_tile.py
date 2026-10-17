"""Real-ESRGAN 分块超分管线（配合 sr_run 使用）

用法:
  python sr_tile.py prep   <输入图> <patches.bin> <meta.json> [裁剪x,y,w,h]
  python sr_tile.py stitch <out.bin> <meta.json> <输出图> [放大倍数=4]
"""
import sys, json
import numpy as np
from PIL import Image

TILE = 128
SCALE = 4


def prep(src, patches_path, meta_path, crop=None):
    im = Image.open(src).convert("RGB")
    if crop:
        x, y, w, h = [int(v) for v in crop.split(",")]
        im = im.crop((x, y, x + w, y + h))
    W, H = im.size
    # 补边到 128 的整数倍（边缘复制，避免黑边干扰）
    pw = (-W) % TILE
    ph = (-H) % TILE
    if pw or ph:
        im = Image.fromarray(np.pad(np.array(im), ((0, ph), (0, pw), (0, 0)), mode="edge"))
    arr = np.asarray(im).astype(np.float32) / 255.0
    PH, PW = arr.shape[:2]
    ny, nx = PH // TILE, PW // TILE
    tiles = np.empty((ny * nx, TILE, TILE, 3), dtype=np.float32)
    k = 0
    for ty in range(ny):
        for tx in range(nx):
            tiles[k] = arr[ty * TILE:(ty + 1) * TILE, tx * TILE:(tx + 1) * TILE]
            k += 1
    tiles.tofile(patches_path)
    meta = {"orig_w": W, "orig_h": H, "padded_w": PW, "padded_h": PH,
            "nx": nx, "ny": ny, "count": nx * ny, "tile": TILE}
    json.dump(meta, open(meta_path, "w"))
    print(f"图像 {W}x{H} -> 补边 {PW}x{PH} -> {nx}x{ny}={nx*ny} 块 (每块 {TILE}x{TILE})")


def stitch(outbin, meta_path, dst, scale=SCALE):
    meta = json.load(open(meta_path))
    nx, ny, T = meta["nx"], meta["ny"], meta["tile"]
    OW, OH = meta["padded_w"] * scale, meta["padded_h"] * scale
    canvas = np.empty((OH, OW, 3), dtype=np.float32)
    osz = 3 * (T * scale) * (T * scale)
    with open(outbin, "rb") as f:
        for k in range(nx * ny):
            raw = f.read(osz * 4)
            if len(raw) != osz * 4:
                print(f"第 {k} 块数据不完整"); break
            t = np.frombuffer(raw, dtype=np.float32).reshape(3, T * scale, T * scale)  # NCHW
            t = np.transpose(t, (1, 2, 0))                                            # -> HWC
            ty, tx = divmod(k, nx)
            canvas[ty * T * scale:(ty + 1) * T * scale,
                   tx * T * scale:(tx + 1) * T * scale] = t
    # 裁掉补边
    final = canvas[:meta["orig_h"] * scale, :meta["orig_w"] * scale]
    final = np.clip(final * 255.0 + 0.5, 0, 255).astype(np.uint8)
    Image.fromarray(final).save(dst)
    print(f"输出 {final.shape[1]}x{final.shape[0]} -> {dst}")


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "prep":
        prep(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5] if len(sys.argv) > 5 else None)
    elif cmd == "stitch":
        stitch(sys.argv[2], sys.argv[3], sys.argv[4])
    else:
        print(__doc__)
