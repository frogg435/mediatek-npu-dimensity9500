// 干净的算子支持度探测：每种算子按其真实签名构建模型
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>

#define LIB "/system_ext/lib64/libneuronservice_adapter.mtk.so"
#define TF32 3
#define I32  1

typedef struct { int32_t type; uint32_t dims; const uint32_t *d; float s; int32_t zp; } OT;
typedef void M; typedef void D;

static int (*mcreate)(M**); static int (*madd)(M*, const OT*);
static int (*mset)(M*, int32_t, const void*, size_t);
static int (*mop)(M*, int32_t, uint32_t, const uint32_t*, uint32_t, const uint32_t*);
static int (*mident)(M*, uint32_t, const uint32_t*, uint32_t, const uint32_t*);
static int (*mfin)(M*); static int (*mfree)(M*);
static int (*gsup)(const M*, D* const*, uint32_t, uint8_t*);
static D *devs[4]; static uint32_t ndev;

static const uint32_t DIMS[2] = {1, 4};
static const OT T_IN  = {TF32, 2, DIMS, 0, 0};
static const OT T_OUT = {TF32, 2, DIMS, 0, 0};
static const OT T_ACT = {I32, 0, NULL, 0, 0};
static const OT T_BETA= {TF32, 0, NULL, 0, 0};
static const uint32_t DIMS2[1] = {2};
static const OT T_PERM = {I32, 1, DIMS2, 0, 0};
static const OT T_AXIS = {I32, 0, NULL, 0, 0};

typedef enum { F_TWO_IN_ACT, F_ONE_IN_ACT, F_ONE_IN_BETA, F_ONE_IN_PERM, F_MEAN } Family;

static M *build(int opcode, Family fam) {
    M *m = NULL;
    if (mcreate(&m)) return NULL;
    int32_t act = 0; float beta = 1.0f;
    int32_t perm[2] = {1, 0};   // 秩 2 张量的合法置换
    int32_t axis[1] = {0}, keep = 0;
    uint32_t ins[3], nIn, outs[1];
    float A[4] = {1, 2, 3, 4}, B[4] = {1, 1, 1, 1};

    switch (fam) {
    case F_TWO_IN_ACT:   // 0=in1 1=in2 2=act 3=out
        madd(m,&T_IN); madd(m,&T_IN); madd(m,&T_ACT); madd(m,&T_OUT);
        mset(m,0,A,16); mset(m,1,B,16); mset(m,2,&act,4);
        ins[0]=0; ins[1]=1; ins[2]=2; nIn=3; outs[0]=3;
        break;
    case F_ONE_IN_ACT:   // 0=in 1=act 2=out
        madd(m,&T_IN); madd(m,&T_ACT); madd(m,&T_OUT);
        mset(m,0,A,16); mset(m,1,&act,4);
        ins[0]=0; ins[1]=1; nIn=2; outs[0]=2;
        break;
    case F_ONE_IN_BETA:  // 0=in 1=beta 2=out
        madd(m,&T_IN); madd(m,&T_BETA); madd(m,&T_OUT);
        mset(m,0,A,16); mset(m,1,&beta,4);
        ins[0]=0; ins[1]=1; nIn=2; outs[0]=2;
        break;
    case F_ONE_IN_PERM:  // 0=in 1=perm 2=out
        madd(m,&T_IN); madd(m,&T_PERM); madd(m,&T_OUT);
        mset(m,0,A,16); mset(m,1,perm,sizeof(perm));
        ins[0]=0; ins[1]=1; nIn=2; outs[0]=2;
        break;
    case F_MEAN:         // 0=in 1=axis 2=keep 3=out
        madd(m,&T_IN); madd(m,&T_AXIS); madd(m,&T_ACT); madd(m,&T_OUT);
        mset(m,0,A,16); mset(m,1,axis,4); mset(m,2,&keep,4);
        ins[0]=0; ins[1]=1; ins[2]=2; nIn=3; outs[0]=3;
        break;
    default: return NULL;
    }
    if (mop(m, opcode, nIn, ins, 1, outs) != 0) { if (mfree) mfree(m); return NULL; }
    if (mident(m, nIn - 1, ins, 1, outs) != 0) { if (mfree) mfree(m); return NULL; }
    if (mfin(m) != 0) { if (mfree) mfree(m); return NULL; }
    return m;
}

static const struct { int op; const char *name; Family fam; } CASES[] = {
    { 0, "ADD",              F_TWO_IN_ACT},
    {18, "MUL",              F_TWO_IN_ACT},
    {36, "SUB",              F_TWO_IN_ACT},
    {30, "DIV",              F_TWO_IN_ACT},
    {39, "MAXIMUM",          F_TWO_IN_ACT},
    {40, "MINIMUM",          F_TWO_IN_ACT},
    {19, "RELU",             F_ONE_IN_ACT},
    {20, "RELU1",            F_ONE_IN_ACT},
    {21, "RELU6",            F_ONE_IN_ACT},
    {28, "TANH",             F_ONE_IN_ACT},
    {14, "LOGISTIC",         F_ONE_IN_ACT},
    {25, "SOFTMAX",          F_ONE_IN_BETA},
};

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    void *h = dlopen(LIB, RTLD_NOW);
    if (!h) { printf("dlopen 失败\n"); return 1; }
    #define S(v,n) v=(__typeof__(v))dlsym(h,n)
    S(mcreate,"NeuronModel_create"); S(madd,"NeuronModel_addOperand");
    S(mset,"NeuronModel_setOperandValue"); S(mop,"NeuronModel_addOperation");
    S(mident,"NeuronModel_identifyInputsAndOutputs"); S(mfin,"NeuronModel_finish");
    S(mfree,"NeuronModel_free"); S(gsup,"NeuronModel_getSupportedOperationsForDevices");
    int (*gdc)(uint32_t*) = (void*)dlsym(h,"Neuron_getDeviceCount");
    int (*gd)(uint32_t, D**) = (void*)dlsym(h,"Neuron_getDevice");
    if (!mcreate||!madd||!mset||!mop||!mident||!mfin||!gdc||!gd) { printf("缺符号\n"); return 2; }
    gdc(&ndev); if (ndev > 4) ndev = 4;
    for (uint32_t i = 0; i < ndev; i++) gd(i, &devs[i]);

    printf("%-10s", "算子"); for (uint32_t i=0;i<ndev;i++) printf("  dev[%u]", i); printf("\n");
    printf("------------------------------------\n");
    for (unsigned k = 0; k < sizeof(CASES)/sizeof(CASES[0]); k++) {
        M *m = build(CASES[k].op, CASES[k].fam);
        printf("%-10s", CASES[k].name);
        if (!m) { printf("  模型构建失败\n"); continue; }
        for (uint32_t i = 0; i < ndev; i++) {
            uint8_t sup = 0;
            if (gsup) gsup(m, (D* const*)&devs[i], 1, &sup);
            printf("  %s", sup ? "OK  " : "  -- ");
        }
        printf("\n");
        if (mfree) mfree(m);
    }
    return 0;
}
