// ue_wrap/engine/save_restore_watch.h -- did the game restore the save into the level it loaded?
//
// A level's BeginPlay restores the registered save through mainGamemode_C::loadObjects: the
// player's transform, every saved object, the player's own data (mainPlayer_C::betterLoadData:
// inventory, equipment) and the triggers. The body skips all of it while the game instance's
// currentLevel is None, so a pawn standing in the level says nothing about whether the save
// arrived. This module watches the two bodies at the script gate for the length of one load
// campaign and answers, for the world the game is in now, whether the restore ran, ran with no
// level named (and so restored nothing), or has not been seen.
//
// Engine-wrapper layer (principle 7): no gameplay or co-op state. Game thread only.

#pragma once

#include <string>

namespace ue_wrap::save_restore_watch {

// Begin (or continue) a load campaign: the two watches registered once per process and their names
// resolved, and the script gate held so the bodies are seen before any session holds it. Polled
// with the boot; idempotent. `who` names the caller on the edge's log line.
void Arm(const char* who);

// End the campaign: the gate hold released. Idempotent.
void Disarm();

enum class Restore {
    NotSeen,  // no loadObjects has returned in the current world
    Ran,      // loadObjects ran with a level named: the save's contents were restored
    Skipped,  // loadObjects ran with currentLevel None: it restored nothing
};

struct View {
    bool watchLive = false;     // both watches registered and resolved; false = cannot tell
    Restore objects = Restore::NotSeen;
    bool playerLoaded = false;  // mainPlayer_C::betterLoadData returned in the current world
    std::wstring currentLevel;  // the game instance's currentLevel as loadObjects began ("None" if so)
};

// The answer for the world the game is in now; a record from any earlier world reads NotSeen.
View ForCurrentWorld();

}  // namespace ue_wrap::save_restore_watch
