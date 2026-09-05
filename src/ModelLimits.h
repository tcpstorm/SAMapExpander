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
}
