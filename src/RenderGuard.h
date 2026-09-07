#pragma once

namespace RenderGuard {
    // Debug only: skip rendering an entity whose memory is not a valid entity, so one corrupted or
    // freed entity cannot take the whole frame down, and log what it was. See RenderGuard.cpp.
    void Install();

    // The model id of the entity RenderEntity was last handed, for the crash log: when the fault is
    // inside the model's own render (a stale RwObject), this names the model that did it.
    int LastRenderedModel();
}
