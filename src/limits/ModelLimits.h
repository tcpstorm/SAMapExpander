#pragma once

// The raised DFF id limit and the resource id bases that move with it. Every getter answers the
// vanilla value when the limit is not raised, so callers never need to ask whether it is active.
namespace ModelLimits {
    void Install();

    bool Active();
    int ModelCount();   // DFF ids are 0 .. ModelCount() - 1
    int TxdBase();
    int ColBase();
    int IplBase();
    int DatBase();
    int IfpBase();
    int RrrBase();
    int ScmBase();
    int TotalCount();   // CStreamingInfo entries: every resource id plus the 4 list-head sentinels

    // Is this model in memory? There is no CStreaming::HasModelLoaded to call in 1.0 US, and the
    // flag has to be read out of whichever ms_aInfoForModel is the live one - which is the whole
    // reason this lives here and not in the caller.
    bool ModelIsLoaded(int model);

    // Where the arrays live now: the vanilla globals, or the heap blocks they were moved to.
    uintptr_t StreamingInfo();   // CStreaming::ms_aInfoForModel
    uintptr_t ModelInfoPtrs();   // CModelInfo::ms_modelInfoPtrs
    uintptr_t AtomicStore();     // CModelInfo::ms_atomicModelInfoStore (int32 count, then objects)
    int AtomicCapacity();        // how many CAtomicModelInfo that store can hold
    uintptr_t TimeStore();       // CModelInfo::ms_timeModelInfoStore (int32 count, then objects)
    int TimeCapacity();          // how many CTimeModelInfo it can hold: what tobj lines become
}
