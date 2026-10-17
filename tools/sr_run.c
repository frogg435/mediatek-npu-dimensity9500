// 分块执行 Real-ESRGAN：从 in.bin 读入 N 个 128x128x3 float32 块，
// 逐块跑 NPU，把 3x512x512 结果顺序写入 out.bin（流式，不占大内存）
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

typedef int  LiteRtStatus;
typedef void *LiteRtEnvironment, *LiteRtModel, *LiteRtOptions;
typedef void *LiteRtCompiledModel, *LiteRtTensorBuffer, *LiteRtTensorBufferRequirements;
typedef uint32_t LiteRtParamIndex;
#define OK 0

typedef struct { uint32_t rank_bits; int32_t dimensions[8]; uint32_t strides[8]; } LiteRtLayout;
typedef struct { int element_type; LiteRtLayout layout; } LiteRtRankedTensorType;
typedef struct { int tag; int _pad0; int type; int _pad1;
                 union { long long int_value; const char *str_value; const void *ptr_value; } value; } EnvOption;

extern LiteRtStatus LiteRtCreateEnvironment(int, const void*, LiteRtEnvironment*);
extern LiteRtStatus LiteRtCreateModelFromFile(LiteRtEnvironment, const char*, LiteRtModel*);
extern LiteRtStatus LiteRtCreateOptions(LiteRtOptions*);
extern LiteRtStatus LiteRtSetOptionsHardwareAccelerators(LiteRtOptions, int);
extern LiteRtStatus LiteRtCreateCompiledModel(LiteRtEnvironment, LiteRtModel, LiteRtOptions, LiteRtCompiledModel*);
extern LiteRtStatus LiteRtGetCompiledModelInputTensorLayout(LiteRtCompiledModel, LiteRtParamIndex, LiteRtParamIndex, LiteRtLayout*);
extern LiteRtStatus LiteRtGetCompiledModelOutputTensorLayouts(LiteRtCompiledModel, LiteRtParamIndex, size_t, LiteRtLayout*, int);
extern LiteRtStatus LiteRtCreateManagedTensorBuffer(LiteRtEnvironment, int, const LiteRtRankedTensorType*, size_t, LiteRtTensorBuffer*);
extern LiteRtStatus LiteRtLockTensorBuffer(LiteRtTensorBuffer, void**, int);
extern LiteRtStatus LiteRtUnlockTensorBuffer(LiteRtTensorBuffer);
extern LiteRtStatus LiteRtRunCompiledModel(LiteRtCompiledModel, LiteRtParamIndex, size_t, LiteRtTensorBuffer*, size_t, LiteRtTensorBuffer*);

static double now_ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return t.tv_sec*1e3 + t.tv_nsec/1e6; }

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 5) { printf("用法: sr_run <model> <in.bin> <out.bin> <块数>\n"); return 1; }
    const char *mp = argv[1], *inp = argv[2], *outp = argv[3];
    int count = atoi(argv[4]);

    const char *ddir = getenv("DISPATCH_DIR");
    if (!ddir) ddir = "/data/data/com.termux/files/home/x86env/aar/libs";
    EnvOption eo[2];
    memset(eo, 0, sizeof(eo));
    eo[0].tag = 0; eo[0].type = 8; eo[0].value.str_value = ddir;
    eo[1].tag = 1; eo[1].type = 8; eo[1].value.str_value = ddir;

    LiteRtEnvironment env; if (LiteRtCreateEnvironment(2, eo, &env)) return 2;
    LiteRtModel model;     if (LiteRtCreateModelFromFile(env, mp, &model)) return 3;
    LiteRtOptions opts;    LiteRtCreateOptions(&opts);
    LiteRtSetOptionsHardwareAccelerators(opts, 1);          // CPU 位，NPU 由 dispatch 接管
    LiteRtCompiledModel cm;
    LiteRtStatus st = LiteRtCreateCompiledModel(env, model, opts, &cm);
    if (st) { printf("CreateCompiledModel 失败 %d\n", st); return 4; }

    LiteRtLayout il, ol;
    memset(&il, 0, sizeof(il)); memset(&ol, 0, sizeof(ol));
    LiteRtGetCompiledModelInputTensorLayout(cm, 0, 0, &il);
    LiteRtGetCompiledModelOutputTensorLayouts(cm, 0, 1, &ol, 0);
    size_t isz = 4, osz = 4;
    for (unsigned i = 0; i < (il.rank_bits & 0x7f); i++) isz *= (size_t)il.dimensions[i];
    for (unsigned i = 0; i < (ol.rank_bits & 0x7f); i++) osz *= (size_t)ol.dimensions[i];
    printf("块输入 %zu 字节 / 块输出 %zu 字节 / 共 %d 块\n", isz, osz, count);

    LiteRtRankedTensorType it = {1, il}, ot = {1, ol};
    LiteRtTensorBuffer ib = NULL, ob = NULL;
    if (LiteRtCreateManagedTensorBuffer(env, 1, &it, isz, &ib)) return 5;
    if (LiteRtCreateManagedTensorBuffer(env, 1, &ot, osz, &ob)) return 5;

    FILE *fi = fopen(inp, "rb"), *fo = fopen(outp, "wb");
    if (!fi || !fo) { printf("打不开输入/输出文件\n"); return 6; }

    unsigned char *inbuf = malloc(isz), *outbuf = malloc(osz);
    double t0 = now_ms();
    for (int k = 0; k < count; k++) {
        if (fread(inbuf, 1, isz, fi) != isz) { printf("第 %d 块读不满\n", k); break; }
        void *addr = NULL;
        LiteRtLockTensorBuffer(ib, &addr, 1); memcpy(addr, inbuf, isz); LiteRtUnlockTensorBuffer(ib);
        st = LiteRtRunCompiledModel(cm, 0, 1, &ib, 1, &ob);
        if (st) { printf("第 %d 块执行失败 %d\n", k, st); return 7; }
        LiteRtLockTensorBuffer(ob, &addr, 0); memcpy(outbuf, addr, osz); LiteRtUnlockTensorBuffer(ob);
        fwrite(outbuf, 1, osz, fo);
        if ((k+1) % 20 == 0 || k+1 == count) printf("  %d/%d  %.1f ms/块\n", k+1, count, (now_ms()-t0)/(k+1));
    }
    double total = now_ms() - t0;
    printf("总计 %.1f ms  (%.2f ms/块)\n", total, total/count);
    fclose(fi); fclose(fo);
    return 0;
}
