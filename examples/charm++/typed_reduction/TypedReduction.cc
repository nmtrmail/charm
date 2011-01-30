#include "TypedReduction.h"
#include <stdlib.h>

Driver::Driver(CkArgMsg* args) {
    int array_size = 10;
    if (args->argc > 1) array_size = strtol(args->argv[1], NULL, 10);
    w = CProxy_Worker::ckNew(array_size);
    CkCallback *cb = new CkCallback(CkIndex_Driver::untyped_done(NULL), thisProxy);
    w.ckSetReductionClient(cb);
    w.reduce();
}

void Driver::untyped_done(CkReductionMsg* m) {
    int* output = (int*)m->getData();
    CkPrintf("Untyped Sum: %d\n", output[0]);
    delete m;

    CkCallback *cb = new CkCallback(
            CkReductionTarget(Driver, typed_done), thisProxy);
    w.ckSetReductionClient(cb);
    w.reduce();
}

void Driver::typed_done(int result)
{
    CkPrintf("Typed Sum: %d\n", result);
    CkCallback *cb = new CkCallback(
            CkIndex_Driver::typed_array_done_redn_wrapper(NULL), thisProxy);
    w.ckSetReductionClient(cb);
    w.reduce_array();
}

void Driver::typed_array_done(int* results, int n)
{
    CkPrintf("Typed Sum: [ ");
    for (int i=0; i<n; ++i) CkPrintf("%d ", results[i]);
    CkPrintf("]\n");
    CkCallback *cb = new CkCallback(
            CkIndex_Driver::typed_array_done2_redn_wrapper(NULL), thisProxy);
    w.ckSetReductionClient(cb);
    w.reduce_array();
}

void Driver::typed_array_done2(int x, int y, int z)
{
    CkPrintf("Typed Sum: (x, y, z) = (%d, %d, %d)\n", x, y, z);
    CkExit();
}

Worker::Worker() { }

void Worker::reduce() {
    int contribution = 1;
    contribute(1*sizeof(int), &contribution, CkReduction::sum_int); 
}

void Worker::reduce_array() {
    int contribution[3] = { 1, 2, 3 };
    contribute(3*sizeof(int), &contribution, CkReduction::sum_int); 
}

#include "TypedReduction.def.h"
