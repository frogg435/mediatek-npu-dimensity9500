// 自包含版本：不使用 LiteRT 头文件，直接声明所需 C API
// 所有句柄都是不透明指针，LiteRtStatus/LiteRtParamIndex 均为整型
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

typedef int            LiteRtStatus;
typedef void          *LiteRtEnvironment;
typedef void          *LiteRtModel;
typedef void          *LiteRtOptions;
typedef void          *LiteRtCompiledModel;
typedef void          *LiteRtTensorBuffer;
typedef void          *LiteRtTensorBufferRequirements;
typedef uint32_t       LiteRtParamIndex;

#define OK 0

// LiteRtEnvOption 的 ABI：{ int tag; (pad) LiteRtAny { int type; (pad) union{...} } }  = 24 字节
typedef struct {
    int tag;
    int _pad0;
    int type;
    int _pad1;
    union { long long int_value; const char *str_value; const void *ptr_value; } value;
} EnvOption;
#define TAG_DISPATCH_LIBRARY_DIR 1
#define ANY_STRING 8

static EnvOption make_str_opt(int tag, const char *str) {
    EnvOption o;
    memset(&o, 0, sizeof(o));
    o.tag = tag; o.type = ANY_STRING; o.value.str_value = str;
    return o;
}

extern LiteRtStatus LiteRtCreateEnvironment(int num_options, const void *options,
                                            LiteRtEnvironment *environment);
extern LiteRtStatus LiteRtCreateModelFromFile(LiteRtEnvironment environment,
                                              const char *model_path,
                                              LiteRtModel *model);
extern LiteRtStatus LiteRtCreateOptions(LiteRtOptions *options);
extern LiteRtStatus LiteRtSetOptionsHardwareAccelerators(LiteRtOptions options, int accels);
extern LiteRtStatus LiteRtGetOptionsHardwareAccelerators(LiteRtOptions options, int *accels);
extern LiteRtStatus LiteRtCreateCompiledModel(LiteRtEnvironment environment,
                                              LiteRtModel model,
                                              LiteRtOptions compilation_options,
                                              LiteRtCompiledModel *compiled_model);
extern LiteRtStatus LiteRtGetCompiledModelInputBufferRequirements(
    LiteRtCompiledModel cm, LiteRtParamIndex signature_index, LiteRtParamIndex input_index,
    LiteRtTensorBufferRequirements *req);
extern LiteRtStatus LiteRtGetCompiledModelOutputBufferRequirements(
    LiteRtCompiledModel cm, LiteRtParamIndex signature_index, LiteRtParamIndex output_index,
    LiteRtTensorBufferRequirements *req);
// LiteRtLayout: { rank:7 | has_strides:1 } + dims[8] + strides[8] = 68 字节
typedef struct {
    uint32_t rank_bits;
    int32_t  dimensions[8];
    uint32_t strides[8];
} LiteRtLayout;

typedef struct {
    int          element_type;
    LiteRtLayout layout;
} LiteRtRankedTensorType;

extern LiteRtStatus LiteRtGetCompiledModelInputTensorLayout(
    LiteRtCompiledModel cm, LiteRtParamIndex sig, LiteRtParamIndex idx, LiteRtLayout *layout);
extern LiteRtStatus LiteRtGetCompiledModelOutputTensorLayouts(
    LiteRtCompiledModel cm, LiteRtParamIndex sig, size_t n, LiteRtLayout *layouts, int update);
extern LiteRtStatus LiteRtCreateManagedTensorBuffer(
    LiteRtEnvironment env, int buffer_type, const LiteRtRankedTensorType *tensor_type,
    size_t buffer_size, LiteRtTensorBuffer *buffer);

extern LiteRtStatus LiteRtCreateManagedTensorBufferFromRequirements(
    LiteRtEnvironment env, LiteRtTensorBufferRequirements req, LiteRtTensorBuffer *buffer);
extern LiteRtStatus LiteRtGetTensorBufferSize(LiteRtTensorBuffer buf, size_t *size);
extern LiteRtStatus LiteRtLockTensorBuffer(LiteRtTensorBuffer buf, void **addr, int lock_mode);
extern LiteRtStatus LiteRtUnlockTensorBuffer(LiteRtTensorBuffer buf);
extern LiteRtStatus LiteRtRunCompiledModel(LiteRtCompiledModel cm, LiteRtParamIndex signature_index,
                                           size_t num_input_buffers, LiteRtTensorBuffer *inputs,
                                           size_t num_output_buffers, LiteRtTensorBuffer *outputs);

#define CHECK(expr) do { LiteRtStatus _s = (expr); \
    printf("  %-56s -> %d%s\n", #expr, _s, _s == OK ? "" : "  <-- 失败"); \
    if (_s != OK) return 1; } while (0)

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *path = (argc > 1) ? argv[1]
        : "/data/data/com.termux/files/home/x86env/aot_out/mobilenet_MediaTek_MT6993_apply_plugin.tflite";
    printf("模型: %s\n\n", path);

    const char *dispatch_dir = getenv("DISPATCH_DIR");
    if (!dispatch_dir) dispatch_dir = "/data/data/com.termux/files/home/x86env/aar/libs";
    printf("Dispatch 插件目录: %s\n", dispatch_dir);
    EnvOption opts_arr[2];
    opts_arr[0] = make_str_opt(0, dispatch_dir);   // CompilerPluginLibraryDir
    opts_arr[1] = make_str_opt(TAG_DISPATCH_LIBRARY_DIR, dispatch_dir);
    printf("sizeof(EnvOption)=%zu (期望 24)\n", sizeof(EnvOption));

    LiteRtEnvironment env;
    CHECK(LiteRtCreateEnvironment(2, opts_arr, &env));

    LiteRtModel model;
    CHECK(LiteRtCreateModelFromFile(env, path, &model));

    LiteRtOptions opts;
    CHECK(LiteRtCreateOptions(&opts));
    {
        const char *ax = getenv("ACCEL");
        int bits = ax ? atoi(ax) : (1 << 2);
        if (bits >= 0) CHECK(LiteRtSetOptionsHardwareAccelerators(opts, bits));
        printf("  请求加速器位 = 0x%x\n", bits);
    }

    int acc = 0;
    if (LiteRtGetOptionsHardwareAccelerators(opts, &acc) == OK)
        printf("  加速器设置位 = 0x%x (4=NPU)\n", acc);

    printf("\n=== 创建 CompiledModel（会加载 MediaTek NPU 插件）===\n");
    LiteRtCompiledModel cm;
    LiteRtStatus st = LiteRtCreateCompiledModel(env, model, opts, &cm);
    printf("  LiteRtCreateCompiledModel -> %d%s\n", st,
           st == OK ? "  ✅ NPU 构建成功" : "  <-- 失败");
    if (st != OK) return 2;

    printf("\n=== 分配 IO ===\n");
    LiteRtTensorBufferRequirements in_req = NULL, out_req = NULL;
    CHECK(LiteRtGetCompiledModelInputBufferRequirements(cm, 0, 0, &in_req));
    CHECK(LiteRtGetCompiledModelOutputBufferRequirements(cm, 0, 0, &out_req));

    (void)in_req; (void)out_req;

    LiteRtRankedTensorType in_type, out_type;
    memset(&in_type, 0, sizeof(in_type));
    memset(&out_type, 0, sizeof(out_type));
    CHECK(LiteRtGetCompiledModelInputTensorLayout(cm, 0, 0, &in_type.layout));
    CHECK(LiteRtGetCompiledModelOutputTensorLayouts(cm, 0, 1, &out_type.layout, 0));
    in_type.element_type = 1;    // kLiteRtElementTypeFloat32
    out_type.element_type = 1;
    printf("  输入 rank=%u dims=%d,%d,%d,%d\n",
           in_type.layout.rank_bits & 0x7f, in_type.layout.dimensions[0],
           in_type.layout.dimensions[1], in_type.layout.dimensions[2], in_type.layout.dimensions[3]);

    size_t in_size = 1, out_size = 1;
    for (unsigned i = 0; i < (in_type.layout.rank_bits & 0x7f); i++)
        in_size *= (size_t)in_type.layout.dimensions[i];
    for (unsigned i = 0; i < (out_type.layout.rank_bits & 0x7f); i++)
        out_size *= (size_t)out_type.layout.dimensions[i];
    in_size *= 4; out_size *= 4;

    LiteRtTensorBuffer in_buf = NULL, out_buf = NULL;
    CHECK(LiteRtCreateManagedTensorBuffer(env, 1 /*HostMemory*/, &in_type, in_size, &in_buf));
    CHECK(LiteRtCreateManagedTensorBuffer(env, 1 /*HostMemory*/, &out_type, out_size, &out_buf));
    printf("  输入 %zu 字节 / 输出 %zu 字节\n", in_size, out_size);

    void *addr = NULL;
    CHECK(LiteRtLockTensorBuffer(in_buf, &addr, 1));      // Write
    memset(addr, 0, in_size);
    for (size_t i = 0; i + 4 <= in_size; i += 4) *(float *)((char *)addr + i) = 0.5f;
    CHECK(LiteRtUnlockTensorBuffer(in_buf));

    printf("\n=== 执行 ===\n");
    int iter = getenv("ITER") ? atoi(getenv("ITER")) : 1;
    LiteRtRunCompiledModel(cm, 0, 1, &in_buf, 1, &out_buf);   // 预热
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int k = 0; k < iter; k++) {
        LiteRtStatus rs = LiteRtRunCompiledModel(cm, 0, 1, &in_buf, 1, &out_buf);
        if (rs != OK) { printf("  第 %d 次执行失败 %d\n", k, rs); return 3; }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = ((t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6) / iter;
    printf("  %d 次平均: %.3f ms/次\n", iter, ms);

    CHECK(LiteRtLockTensorBuffer(out_buf, &addr, 0));     // Read
    int n = (int)(out_size / 4);
    printf("  输出前 8 值: ");
    for (int i = 0; i < n && i < 8; i++) printf("%.4f ", ((float *)addr)[i]);
    float best = -1e30f; int bi = -1;
    for (int i = 0; i < n; i++) { float v = ((float *)addr)[i]; if (v > best) { best = v; bi = i; } }
    printf("\n  argmax=%d 值=%.4f (共 %d 类)\n", bi, best, n);
    {
        const char *dump = getenv("DUMP");
        if (dump) {
            FILE *fp = fopen(dump, "wb");
            if (fp) { fwrite(addr, 1, out_size, fp); fclose(fp);
                      printf("  已写出输出到 %s (%zu 字节)\n", dump, out_size); }
        }
    }
    CHECK(LiteRtUnlockTensorBuffer(out_buf));

    printf("\n🎉 执行完成\n");
    return 0;
}
