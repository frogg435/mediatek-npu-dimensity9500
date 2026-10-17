// Neuron SDK (MediaTek NeuroPilot) 最小推理测试
// 目标：在 NPU 上编译并执行一个 FLOAT32 向量加法
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>

#define LIB "/system_ext/lib64/libneuronservice_adapter.mtk.so"

// NNAPI 兼容的枚举值（MTK Neuron 沿用 NNAPI 编号）
#define NEURON_FLOAT32      0
#define NEURON_TENSOR_FLOAT32 3
#define NEURON_ADD          0
#define NEURON_INT32        1
#define NEURON_NO_ACTIVATION 0

typedef struct {
    int32_t   type;
    uint32_t  dimensionCount;
    const uint32_t *dimensions;
    float     scale;
    int32_t   zeroPoint;
} NeuronOperandType;

typedef void NeuronModel;
typedef void NeuronCompilation;
typedef void NeuronExecution;
typedef void NeuronDevice;

#define SYM(h, name) \
    __typeof__(name##_t) name = (__typeof__(name##_t)) dlsym(h, #name); \
    if (!name) { printf("缺少符号 %s\n", #name); return 2; }

/* 先把函数指针类型声明出来 */
typedef int  (*fn_model_create)(NeuronModel **);
typedef int  (*fn_model_addOperand)(NeuronModel *, const NeuronOperandType *);
typedef int  (*fn_model_setOperandValue)(NeuronModel *, int32_t, const void *, size_t);
typedef int  (*fn_model_addOperation)(NeuronModel *, int32_t, uint32_t,
                                      const uint32_t *, uint32_t, const uint32_t *);  /* (model, type, inputCount, inputs, outputCount, outputs) */
typedef int  (*fn_model_identify)(NeuronModel *, uint32_t, const uint32_t *,
                                  uint32_t, const uint32_t *);
typedef int  (*fn_model_finish)(NeuronModel *);
typedef int  (*fn_comp_create)(NeuronModel *, NeuronCompilation **);
typedef int  (*fn_comp_finish)(NeuronCompilation *);
typedef int  (*fn_exec_create)(NeuronCompilation *, NeuronExecution **);
typedef int  (*fn_exec_setInput)(NeuronExecution *, int32_t, const NeuronOperandType *,
                                 const void *, size_t);
typedef int  (*fn_exec_setOutput)(NeuronExecution *, int32_t, const NeuronOperandType *,
                                  void *, size_t);
typedef int  (*fn_exec_compute)(NeuronExecution *);
typedef int  (*fn_getDevCount)(uint32_t *);
typedef int  (*fn_getDevice)(uint32_t, NeuronDevice **);
typedef int  (*fn_dev_getName)(const NeuronDevice *, uint32_t *, char *);

#define CK(expr) do { int _r = (expr); \
    printf("  %-46s -> %d%s\n", #expr, _r, _r == 0 ? "" : "  <-- 失败"); \
    if (_r != 0) { fail = _r; goto done; } } while (0)

int main(void) {
    void *h = dlopen(LIB, RTLD_NOW);
    if (!h) { printf("dlopen 失败: %s\n", dlerror()); return 1; }

    fn_getDevCount  getDevCount  = (fn_getDevCount)  dlsym(h, "Neuron_getDeviceCount");
    fn_getDevice    getDevice    = (fn_getDevice)    dlsym(h, "Neuron_getDevice");
    if (!getDevCount || !getDevice) { printf("缺少设备枚举符号\n"); return 2; }

    uint32_t ndev = 0;
    printf("设备枚举:\n");
    if (getDevCount(&ndev) != 0 || ndev == 0) { printf("  无 NPU 设备\n"); return 3; }
    NeuronDevice *dev = NULL;
    for (uint32_t i = 0; i < ndev; i++) {
        NeuronDevice *d = NULL;
        if (getDevice(i, &d) == 0 && d) {
            char name[256] = {0}; uint32_t ln = sizeof(name);
            fn_dev_getName gn = (fn_dev_getName) dlsym(h, "NeuronDevice_getName");
            int rn = gn ? gn(d, &ln, name) : -1;
            printf("  [%u] name ret=%d -> '%s'\n", i, rn, name);
            if (!dev) dev = d;
        }
    }

    /* 动态取符号 */
    fn_model_create        Model_create     = (fn_model_create)        dlsym(h, "NeuronModel_create");
    fn_model_addOperand    Model_addOperand = (fn_model_addOperand)    dlsym(h, "NeuronModel_addOperand");
    fn_model_setOperandValue Model_setVal   = (fn_model_setOperandValue) dlsym(h, "NeuronModel_setOperandValue");
    fn_model_addOperation  Model_addOp      = (fn_model_addOperation)  dlsym(h, "NeuronModel_addOperation");
    fn_model_identify      Model_identify   = (fn_model_identify)      dlsym(h, "NeuronModel_identifyInputsAndOutputs");
    fn_model_finish        Model_finish     = (fn_model_finish)        dlsym(h, "NeuronModel_finish");
    fn_comp_create         Comp_create      = (fn_comp_create)         dlsym(h, "NeuronCompilation_create");
    fn_comp_finish         Comp_finish      = (fn_comp_finish)         dlsym(h, "NeuronCompilation_finish");
    fn_exec_create         Exec_create      = (fn_exec_create)         dlsym(h, "NeuronExecution_create");
    fn_exec_setInput       Exec_setInput    = (fn_exec_setInput)       dlsym(h, "NeuronExecution_setInput");
    fn_exec_setOutput      Exec_setOutput   = (fn_exec_setOutput)      dlsym(h, "NeuronExecution_setOutput");
    fn_exec_compute        Exec_compute     = (fn_exec_compute)        dlsym(h, "NeuronExecution_compute");

    const char *need[] = {"NeuronModel_create (NeuronModel**, )"};
    (void)need;
    if (!Model_create || !Model_addOperand || !Model_setVal || !Model_addOp ||
        !Model_identify || !Model_finish || !Comp_create || !Comp_finish ||
        !Exec_create || !Exec_setInput || !Exec_setOutput || !Exec_compute) {
        printf("缺少必要符号\n"); return 2;
    }

    int fail = 0;
    NeuronModel *model = NULL;
    NeuronCompilation *comp = NULL;
    NeuronExecution *exec = NULL;
    uint32_t dims[2] = {1, 4};
    NeuronOperandType t = { NEURON_TENSOR_FLOAT32, 2, dims, 0.0f, 0 };

    float A[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    float B[4] = {10.0f, 20.0f, 30.0f, 40.0f};
    float C[4] = {0};

    printf("\n构建模型:\n");
    CK(Model_create(&model));
    CK(Model_addOperand(model, &t));            // 0: 输入 A  [1,4] f32
    CK(Model_addOperand(model, &t));            // 1: 输入 B  [1,4] f32
    NeuronOperandType tAct = { NEURON_INT32, 0, NULL, 0.0f, 0 };   // 标量
    CK(Model_addOperand(model, &tAct));         // 2: act (NO_ACTIVATION)
    CK(Model_addOperand(model, &t));            // 3: 输出 C  [1,4] f32
    int32_t act = NEURON_NO_ACTIVATION;
    CK(Model_setVal(model, 2, &act, sizeof(act)));
    uint32_t ins[3] = {0, 1, 2}, outs[1] = {3};
    CK(Model_addOp(model, NEURON_ADD, 3, ins, 1, outs));   // inputCount=3
    CK(Model_identify(model, 2, ins, 1, outs));
    CK(Model_finish(model));

    printf("\n编译 (交给 NPU):\n");
    CK(Comp_create(model, &comp));
    CK(Comp_finish(comp));

    printf("\n执行:\n");
    CK(Exec_create(comp, &exec));
    CK(Exec_setInput(exec, 0, &t, A, sizeof(A)));
    CK(Exec_setInput(exec, 1, &t, B, sizeof(B)));
    CK(Exec_setOutput(exec, 0, &t, C, sizeof(C)));
    CK(Exec_compute(exec));

    printf("\n结果: ");
    int ok = 1;
    for (int i = 0; i < 4; i++) {
        printf("%.1f ", C[i]);
        if (C[i] != A[i] + B[i]) ok = 0;
    }
    printf("\n验证: %s   (期望 11 22 33 44)\n", ok ? "PASS ✅ NPU 真的在算" : "FAIL ❌");

done:
    if (fail) printf("\n失败码 = %d\n", fail);
    return fail ? 4 : (0);
}
