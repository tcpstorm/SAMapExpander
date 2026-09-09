#pragma once

// Asking San Andreas' streamer for a model of ours WITHOUT taking it out of the game's own
// rotation for the rest of the session. StreamedModels.cpp says why that is not what
// CStreaming::RequestModel on its own does.
namespace StreamedModels {

// Ask for `model` if it is not in memory yet. Safe to call every frame with the same id: the
// request is only made once while it is outstanding.
void Want(int model);

// Once a frame. Any model that has finished loading is handed back to the streamer's own
// least-recently-used list, so it can be thrown out again when the game needs the room.
void Tick();

// Hand every outstanding model back at once - when the player leaves every region, or between
// games. Nothing of ours is being made out here, so nothing of ours should be held.
void ReleaseAll();

// How many models we are still holding out of the game's rotation, for the F8 read-out.
int Held();

}   // namespace StreamedModels
