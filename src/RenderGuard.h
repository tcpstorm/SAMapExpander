#pragma once

namespace RenderGuard {
    // Debug only: skip rendering an entity whose memory is not a valid entity, so one corrupted or
    // freed entity cannot take the whole frame down, and log what it was. See RenderGuard.cpp.
    void Install();
}
