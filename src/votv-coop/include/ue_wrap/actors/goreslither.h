// ue_wrap/actors/goreslither.h -- engine access for the gore slither family (npc_goreSlither_C and the classes below
// it, the blue brain cheese's npc_goreslither_cheese_C among them). What a client does for its mirror of one, whose
// own Blueprint graph is refused: the part of BeginPlay that is presentation. Engine-wrapper layer (principle 7):
// reflected reads, writes and calls, with no gameplay, network or coop state. Game thread. Implementation:
// src/ue_wrap/actors/goreslither.cpp.

#pragma once

namespace ue_wrap::goreslither {

// BeginPlay's presentation, reproduced in its order. The class's BeginPlay does four things: domove (the AI move to a
// point of this machine's navmesh, a decision), a random `timeAnim` (the slither's animation phase), the mesh
// re-attached to `springArm` (KeepRelative x3, welded; the construction parents it to the capsule), and the 0.5 s
// looping `sense` timer (the jump at the nearest pawn, a decision). The two presentation steps run here. True when
// both ran; false, and said once per class, when a step did not resolve or its call faulted (the other still runs).
// Game thread.
bool ApplyBeginPlayPresentation(void* slither);

}  // namespace ue_wrap::goreslither
