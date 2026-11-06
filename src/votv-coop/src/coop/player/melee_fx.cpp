// coop/player/melee_fx.cpp -- see coop/player/melee_fx.h. The wire half: pack and send, the host's judge, and the
// receiver's play. The swing and contact seams that make the effects are melee_fx_swing.cpp and melee_fx_contact.cpp.

#include "coop/player/melee_fx.h"
#include "melee_fx_internal.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/creatures/hit_target_creature.h"
#include "coop/element/element.h"
#include "coop/element/player.h"
#include "coop/element/registry.h"
#include "coop/net/intent_bucket.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/player/hit_judge.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"
#include "coop/props/prop_sound.h"
#include "coop/props/remote_prop.h"

#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_audio.h"
#include "ue_wrap/engine/engine_emitter.h"
#include "ue_wrap/world/weapon_catalog.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_set>

namespace coop::melee_fx {

namespace {

namespace N  = coop::net;
namespace E  = ue_wrap::engine;
namespace EL = coop::element;
using ue_wrap::FVector;

constexpr int kSaid = 8;   // the lines of each kind a session says before it says only the counts

std::atomic<N::Session*> g_session{nullptr};

// ---- the maker's side -----------------------------------------------------------------------------------------

// A maker's own pace: a swing and its hit make a few effects, a flurry of knocks on one held item many. Bounded so a
// physics storm on a held prop cannot flood the host's lane.
constexpr N::IntentBudget kMakeBudget{10.0f, 6.0f};
N::IntentBucket g_makeBucket;
uint32_t        g_nextEventId = 1;
uint32_t        g_sent = 0;
uint32_t        g_paced = 0;
int             g_sentSaid = 0;
bool            g_wireSaid = false;

bool Ascii(const std::wstring& s) {
    for (wchar_t c : s)
        if (c < 0x20 || c > 0x7E) return false;
    return true;
}

const char* KindName(uint8_t kind) {
    switch (kind) {
    case N::kMeleeFxSwing:   return "swing";
    case N::kMeleeFxHit:     return "hit";
    case N::kMeleeFxContact: return "contact";
    case N::kMeleeFxBreak:   return "break";
    default:                 return "?";
    }
}

// ---- the host's judge ------------------------------------------------------------------------------------------

constexpr N::IntentBudget kJudgeBudget{12.0f, 6.0f};
constexpr float kSwingSoundExtraCm = 150.f;   // a sound at the swinger's own place lies behind the swing's reach
constexpr float kHeldReachCm = 700.f;         // a held prop knocks within the holder's arm and what it carries
constexpr int   kIdRing = 16;

struct Reason {
    const char* why = nullptr;
    uint32_t count = 0;
};
constexpr int kMaxReasons = 16;

struct Slot {
    N::IntentBucket bucket;
    uint32_t        ids[kIdRing] = {};
    int             nextId = 0;
    Reason          reasons[kMaxReasons];
};
coop::roster_ledger::PerSlotState<Slot> g_slots;

uint32_t g_judged = 0;
int      g_judgedSaid = 0;

// Refuse: counted, and said on the first of each reason per sender.
bool Refuse(int slot, const char* why) {
    if (slot < 1 || slot >= coop::players::kMaxPeers) return false;
    for (Reason& r : g_slots[slot].reasons) {
        if (r.why && std::strcmp(r.why, why) != 0) continue;
        if (!r.why) {
            r.why = why;
            UE_LOGI("[MELEE-FX] host: slot %d's effect refused: %s", slot, why);
        } else if (detail::LogAll()) {
            UE_LOGI("[MELEE-FX] host: slot %d's effect refused: %s (#%u)", slot, why, r.count + 1);
        }
        ++r.count;
        return false;
    }
    return false;
}

float Dist(const FVector& a, const FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool SeenId(Slot& sl, uint32_t id) {
    for (uint32_t v : sl.ids)
        if (v == id) return true;
    sl.ids[sl.nextId] = id;
    sl.nextId = (sl.nextId + 1) % kIdRing;
    return false;
}

// The held prop the sender names is the one it drives, or, for a hand item (no element of its own), the sender's hand
// holds something.
const char* HolderRefusal(int slot, uint32_t sourceEid) {
    if (sourceEid != 0) {
        void* actor = EL::LivePropActor(static_cast<EL::ElementId>(sourceEid));
        if (!actor) return "the held prop has no actor here";
        if (ue_wrap::container_inventory::IsContainer(actor)) return "a container's cues are the host's";
        if (coop::remote_prop::DrivingSlotOf(actor) != slot) return "the sender does not hold that prop";
        return nullptr;
    }
    return coop::hand_item::HoldsItem(static_cast<uint8_t>(slot)) ? nullptr : "the sender's hand holds nothing";
}

// The plausibility of one effect against the sender's pose. Null when it passes, else the reason.
const char* PlausibilityRefusal(int slot, const N::MeleeFxPayload& p) {
    coop::RemotePlayer& rp = coop::puppet_drive::Puppet(slot);
    FVector eye{};
    if (!(rp.valid() && rp.HasPose() && rp.TryGetEyePosition(eye))) return "no eye";
    const FVector at{p.loc[0], p.loc[1], p.loc[2]};
    if (p.kind == N::kMeleeFxSwing || p.kind == N::kMeleeFxHit) {
        ue_wrap::weapon_catalog::Swing swing;
        if (!ue_wrap::weapon_catalog::Lookup(coop::hand_item::HeldItem(static_cast<uint8_t>(slot)), swing))
            return "weapon table unreadable";
        if (!swing.canSwing) return "no swinging item";
        if (!(swing.length > 0.f)) return "no reach data";
        const float bound = swing.length + coop::hit_judge::kReachSlackCm +
                            (p.kind == N::kMeleeFxSwing ? kSwingSoundExtraCm : 0.f);
        if (!(Dist(eye, at) <= bound)) return "out of reach";
        if (p.kind == N::kMeleeFxHit) {
            const FVector aim = rp.GetLatestAimDirection();
            if (!(coop::hit_judge::FacingCosine(aim, eye, at) >= coop::hit_judge::kFacingCosine)) return "not facing";
        }
        return nullptr;
    }
    if (!(Dist(eye, at) <= kHeldReachCm)) return "out of reach";
    return HolderRefusal(slot, p.sourceEid);
}

// ---- the receiver's play ---------------------------------------------------------------------------------------

constexpr N::IntentBudget kPlayBudget{20.0f, 12.0f};
N::IntentBucket g_playBucket;
uint32_t        g_played = 0;
uint32_t        g_playPaced = 0;
int             g_playedSaid = 0;
bool            g_noWorldSaid = false;
bool            g_noSoundSaid = false;
bool            g_noParticleSaid = false;
bool            g_spawnFailedSaid = false;

// A name this machine could not resolve once is not asked again: the lookup walks every instance of the class.
std::unordered_set<std::wstring> g_unresolved;
constexpr size_t kMaxUnresolved = 64;

struct Seen {
    uint32_t origin = 0;
    uint32_t id = 0;
};
constexpr int kSeenRing = 32;
Seen g_seen[kSeenRing];
int  g_seenNext = 0;

bool AlreadySeen(uint32_t origin, uint32_t id) {
    for (const Seen& s : g_seen)
        if (s.origin == origin && s.id == id) return true;
    g_seen[g_seenNext] = Seen{origin, id};
    g_seenNext = (g_seenNext + 1) % kSeenRing;
    return false;
}

void* Resolve(const std::wstring& name) {
    if (g_unresolved.count(name)) return nullptr;
    void* asset = E::FindSound(name);   // the lookup is by class, package and name: any asset class
    if (!asset && g_unresolved.size() < kMaxUnresolved) g_unresolved.insert(name);
    return asset;
}

// The rotator whose up axis is `n`, so a surface's particles spray away from it; none for a zero normal.
ue_wrap::FRotator RotationFromNormal(const FVector& n) {
    const float len = std::sqrt(n.X * n.X + n.Y * n.Y + n.Z * n.Z);
    if (!(len > 0.01f)) return ue_wrap::FRotator{0.f, 0.f, 0.f};
    constexpr float kRadToDeg = 57.29578f;
    const float nz = n.Z / len;
    ue_wrap::FRotator r{};
    r.Pitch = std::asin(nz > 1.f ? 1.f : nz < -1.f ? -1.f : nz) * kRadToDeg - 90.f;
    r.Yaw = std::atan2(n.Y, n.X) * kRadToDeg;
    r.Roll = 0.f;
    return r;
}

std::wstring TextOf(const N::MeleeFxPayload& p, size_t from, size_t n) {
    return std::wstring(p.text + from, p.text + from + n);
}

int g_ownSaid = 0;

// Play `p` here. `skipSound` leaves the sound out (the host's own zombie feedback already played one). `own` is this
// machine's player's effect, played as it is made (PlayOwn): it has its own log line.
void Play(const N::MeleeFxPayload& p, bool skipSound, bool own = false) {
    if (!detail::Enabled()) return;
    if (!g_playBucket.Take(kPlayBudget, N::NowMs())) {
        ++g_playPaced;
        return;
    }
    void* world = coop::players::Registry::Get().Local();
    if (!world) {
        if (!g_noWorldSaid) {
            g_noWorldSaid = true;
            UE_LOGI("[MELEE-FX] no local player to play an effect through yet -- skipped (said once)");
        }
        return;
    }
    const FVector at{p.loc[0], p.loc[1], p.loc[2]};
    bool soundPlayed = false, particlePlayed = false;
    if ((p.effects & N::kMeleeFxSound) && !skipSound) {
        void* sound = Resolve(TextOf(p, 0, p.soundLen));
        if (sound) {
            coop::prop_sound::PlaySoundAt(world, sound, at, p.volume, p.pitch);
            soundPlayed = true;
        } else if (!g_noSoundSaid) {
            g_noSoundSaid = true;
            UE_LOGI("[MELEE-FX] sound '%.*s' is not loaded here -- skipped (said once)", static_cast<int>(p.soundLen),
                    p.text);
        }
    }
    if (p.effects & N::kMeleeFxParticle) {
        void* particle = Resolve(TextOf(p, p.soundLen, p.particleLen));
        if (particle) {
            const FVector scale{p.scale, p.scale, p.scale};
            if (E::SpawnEmitterAtLocation(world, particle, at, RotationFromNormal({p.normal[0], p.normal[1], p.normal[2]}),
                                          scale)) {
                particlePlayed = true;
            } else if (!g_spawnFailedSaid) {
                g_spawnFailedSaid = true;
                UE_LOGW("[MELEE-FX] the emitter call failed -- a hit's particles are not shown (said once)");
            }
        } else if (!g_noParticleSaid) {
            g_noParticleSaid = true;
            UE_LOGI("[MELEE-FX] particle system '%.*s' is not loaded here -- skipped (said once)",
                    static_cast<int>(p.particleLen), p.text + p.soundLen);
        }
    }
    if (!soundPlayed && !particlePlayed) return;
    ++g_played;
    if (own) {
        if (detail::LogAll() || g_ownSaid < kSaid) {
            ++g_ownSaid;
            UE_LOGI("[MELEE-FX] local play own %s event %u at (%.0f, %.0f, %.0f): sound=%d particle=%d (#%u)",
                    KindName(p.kind), p.eventId, static_cast<double>(at.X), static_cast<double>(at.Y),
                    static_cast<double>(at.Z), soundPlayed ? 1 : 0, particlePlayed ? 1 : 0, g_played);
        }
        return;
    }
    if (detail::LogAll() || g_playedSaid < kSaid) {
        ++g_playedSaid;
        UE_LOGI("[MELEE-FX] play %s from player %u event %u at (%.0f, %.0f, %.0f): sound=%d particle=%d (#%u)",
                KindName(p.kind), p.originEid, p.eventId, static_cast<double>(at.X), static_cast<double>(at.Y),
                static_cast<double>(at.Z), soundPlayed ? 1 : 0, particlePlayed ? 1 : 0, g_played);
    }
}

bool Bounded(float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; }

bool TextShape(const N::MeleeFxPayload& p) {
    for (int i = 0; i < p.soundLen + p.particleLen; ++i)
        if (p.text[i] < 0x20 || p.text[i] > 0x7E) return false;
    const auto starts = [](const char* at, int n, const char* prefix) {
        const size_t len = std::strlen(prefix);
        return static_cast<size_t>(n) > len && std::strncmp(at, prefix, len) == 0;
    };
    if (p.soundLen > 0 && !starts(p.text, p.soundLen, "SoundCue ") && !starts(p.text, p.soundLen, "SoundWave "))
        return false;
    return p.particleLen == 0 || starts(p.text + p.soundLen, p.particleLen, "ParticleSystem ");
}

}  // namespace

namespace detail {

bool Enabled() {
    static const bool on = coop::config::ResolveFlag(::coop::config_registry::rows::melee_fx);
    return on;
}

bool LogAll() {
    static const bool on = coop::config::ResolveFlag(::coop::config_registry::rows::melee_fx_log);
    return on;
}

N::Session* LiveSession() {
    N::Session* s = g_session.load(std::memory_order_acquire);
    return s && s->connected() ? s : nullptr;
}

uint32_t KeyHash(const std::wstring& text) {
    uint32_t h = 2166136261u;
    for (wchar_t c : text) {
        h ^= static_cast<uint32_t>(c) & 0xFFu;
        h *= 16777619u;
        h ^= (static_cast<uint32_t>(c) >> 8) & 0xFFu;
        h *= 16777619u;
    }
    return h;
}

void PlayOwn(const Effect& e) {
    if (!Enabled() || e.particle.empty() || !Ascii(e.particle) ||
        e.particle.size() > static_cast<size_t>(N::kMeleeFxTextCap))
        return;
    N::MeleeFxPayload p{};
    p.eventId = g_nextEventId;   // the id Publish gives this effect, so the two log lines name one event
    p.kind = e.kind;
    p.loc[0] = e.loc.X;
    p.loc[1] = e.loc.Y;
    p.loc[2] = e.loc.Z;
    p.normal[0] = e.normal.X;
    p.normal[1] = e.normal.Y;
    p.normal[2] = e.normal.Z;
    p.scale = e.scale;
    p.volume = e.volume;
    p.pitch = e.pitch;
    p.effects = N::kMeleeFxParticle;
    p.particleLen = static_cast<uint8_t>(e.particle.size());
    for (size_t i = 0; i < e.particle.size(); ++i) p.text[i] = static_cast<char>(e.particle[i]);
    Play(p, false, true);
}

bool Publish(const Effect& e) {
    N::Session* s = LiveSession();
    if (!Enabled() || !s || (e.sound.empty() && e.particle.empty())) return false;
    if (e.kind >= N::kMeleeFxKindCount) return false;
    // The two asset paths share the text; one that does not fit, or is not plain ASCII, is left out, the sound's
    // place going to the particle when the sound alone is too long.
    std::wstring sound = e.sound, particle = e.particle;
    if (!Ascii(sound) || sound.size() > static_cast<size_t>(N::kMeleeFxTextCap)) sound.clear();
    if (!Ascii(particle) || sound.size() + particle.size() > static_cast<size_t>(N::kMeleeFxTextCap)) particle.clear();
    if (sound.empty() && particle.empty()) {
        if (!g_wireSaid) {
            g_wireSaid = true;
            UE_LOGW("[MELEE-FX] an asset name does not fit the wire ('%ls' / '%ls') -- the effect is not sent (said once)",
                    e.sound.c_str(), e.particle.c_str());
        }
        return false;
    }
    if (!g_makeBucket.Take(kMakeBudget, N::NowMs())) {
        ++g_paced;
        return false;
    }

    N::MeleeFxPayload p{};
    const bool host = s->role() == N::Role::Host;
    p.originEid = host ? static_cast<uint32_t>(coop::players::Registry::Get().LocalPlayerElementId()) : 0u;
    p.eventId = g_nextEventId++;
    if (g_nextEventId == 0) g_nextEventId = 1;
    p.sourceEid = e.sourceEid;
    p.targetEid = e.targetEid;
    p.weaponKey = host ? KeyHash(coop::hand_item::HeldItem(0)) : 0u;
    p.kind = e.kind;
    p.loc[0] = e.loc.X;
    p.loc[1] = e.loc.Y;
    p.loc[2] = e.loc.Z;
    p.normal[0] = e.normal.X;
    p.normal[1] = e.normal.Y;
    p.normal[2] = e.normal.Z;
    p.scale = e.scale;
    p.volume = e.volume;
    p.pitch = e.pitch;
    if (!sound.empty()) {
        p.effects |= N::kMeleeFxSound;
        p.soundLen = static_cast<uint8_t>(sound.size());
        for (size_t i = 0; i < sound.size(); ++i) p.text[i] = static_cast<char>(sound[i]);
    }
    if (!particle.empty()) {
        p.effects |= N::kMeleeFxParticle;
        p.particleLen = static_cast<uint8_t>(particle.size());
        for (size_t i = 0; i < particle.size(); ++i) p.text[sound.size() + i] = static_cast<char>(particle[i]);
    }
    const int bytes = N::kMeleeFxHeaderBytes + p.soundLen + p.particleLen;
    const bool ok = s->SendReliable(host ? N::ReliableKind::MeleeFx : N::ReliableKind::MeleeFxRequest, &p, bytes);
    if (!ok) return false;
    ++g_sent;
    if (LogAll() || g_sentSaid < kSaid) {
        ++g_sentSaid;
        UE_LOGI("[MELEE-FX] %s sent %s event %u at (%.0f, %.0f, %.0f): sound='%ls' particle='%ls' (#%u)",
                host ? "host" : "client", KindName(e.kind), p.eventId, static_cast<double>(e.loc.X),
                static_cast<double>(e.loc.Y), static_cast<double>(e.loc.Z), sound.c_str(), particle.c_str(), g_sent);
    }
    return true;
}

}  // namespace detail

void Install(N::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!session || !detail::Enabled()) return;
    detail::InstallSwing();
    detail::InstallContact();
}

void Tick() {
    if (!detail::Enabled()) return;
    detail::TickSwing();
    detail::TickContact();
    N::Session* s = g_session.load(std::memory_order_acquire);
    // The weapon table the host's judge reads is built once the session runs, not on a client's first effect: its build
    // finds the table by walking every object.
    static bool sWeaponsReady = false;
    if (!sWeaponsReady && s && s->running() && s->role() == N::Role::Host)
        sWeaponsReady = ue_wrap::weapon_catalog::Ready();
}

bool WellFormed(const N::MeleeFxPayload& p, int len) {
    if (len < N::kMeleeFxHeaderBytes || p.kind >= N::kMeleeFxKindCount || p.eventId == 0) return false;
    if (p.effects == 0 || (p.effects & ~(N::kMeleeFxSound | N::kMeleeFxParticle))) return false;
    if (((p.effects & N::kMeleeFxSound) != 0) != (p.soundLen > 0)) return false;
    if (((p.effects & N::kMeleeFxParticle) != 0) != (p.particleLen > 0)) return false;
    const int text = p.soundLen + p.particleLen;
    if (text > N::kMeleeFxTextCap || len < N::kMeleeFxHeaderBytes + text) return false;
    for (int i = 0; i < 3; ++i)
        if (!Bounded(p.loc[i], -1e7f, 1e7f) || !Bounded(p.normal[i], -1.5f, 1.5f)) return false;
    if (!Bounded(p.scale, 0.05f, 20.f) || !Bounded(p.volume, 0.f, 4.f) || !Bounded(p.pitch, 0.25f, 4.f)) return false;
    return TextShape(p);
}

void OnRequest(int senderSlot, const N::MeleeFxPayload& req) {
    N::Session* s = detail::LiveSession();
    if (!s || s->role() != N::Role::Host || !detail::Enabled()) return;
    if (senderSlot < 1 || senderSlot >= coop::players::kMaxPeers) return;
    Slot& sl = g_slots[senderSlot];
    if (!s->IsSlotWorldReady(senderSlot)) {
        Refuse(senderSlot, "slot not world-ready");
        return;
    }
    if (!sl.bucket.Take(kJudgeBudget, N::NowMs())) {
        Refuse(senderSlot, "over its pace");
        return;
    }
    if (SeenId(sl, req.eventId)) {
        Refuse(senderSlot, "a repeated id");
        return;
    }
    if (const char* why = PlausibilityRefusal(senderSlot, req)) {
        Refuse(senderSlot, why);
        return;
    }

    // The host stamps who made it and with what; a client's own claim of either is not read.
    N::MeleeFxPayload out = req;
    const coop::element::Player* maker = coop::players::Registry::Get().GetPlayerElement(static_cast<uint8_t>(senderSlot));
    out.originEid = maker ? static_cast<uint32_t>(maker->GetId()) : 0u;
    out.weaponKey = detail::KeyHash(coop::hand_item::HeldItem(static_cast<uint8_t>(senderSlot)));
    const int bytes = N::kMeleeFxHeaderBytes + out.soundLen + out.particleLen;
    // Every client but the maker, which played it as it made it.
    s->SendReliable(N::ReliableKind::MeleeFx, &out, bytes, senderSlot);
    ++g_judged;
    if (detail::LogAll() || g_judgedSaid < kSaid) {
        ++g_judgedSaid;
        UE_LOGI("[MELEE-FX] host: slot %d's %s event %u accepted and published (#%u)", senderSlot, KindName(out.kind),
                out.eventId, g_judged);
    }
    // A creature the host parks answers a hit with its own zombie feedback, played on the host as the host applies the
    // hit; the effect here is what the other clients lack.
    const bool creature = out.targetEid != 0 && coop::hit_target_creature::HostTarget(out.targetEid) != nullptr;
    Play(out, creature && out.kind == N::kMeleeFxHit);
}

void OnFx(const N::MeleeFxPayload& p) {
    N::Session* s = detail::LiveSession();
    if (!s || s->role() != N::Role::Client) return;
    if (p.originEid != 0 &&
        p.originEid == static_cast<uint32_t>(coop::players::Registry::Get().LocalPlayerElementId()))
        return;   // this machine's own effect, which it played as it made it
    if (AlreadySeen(p.originEid, p.eventId)) return;
    Play(p, false);
}

void OnDisconnect() {
    if (g_sent != 0 || g_paced != 0 || g_judged != 0 || g_played != 0 || g_playPaced != 0)
        UE_LOGI("[MELEE-FX] this session: %u sent (%u held back by the pace), %u judged and published, %u played (%u "
                "held back by the pace)", g_sent, g_paced, g_judged, g_played, g_playPaced);
    g_makeBucket.Reset();
    g_playBucket.Reset();
    g_nextEventId = 1;
    g_sent = g_paced = g_judged = g_played = g_playPaced = 0;
    g_sentSaid = g_judgedSaid = g_playedSaid = g_ownSaid = 0;
    g_wireSaid = g_noWorldSaid = g_noSoundSaid = g_noParticleSaid = g_spawnFailedSaid = false;
    for (Seen& v : g_seen) v = Seen{};
    g_seenNext = 0;
    g_unresolved.clear();
    detail::ResetSwing();
    detail::ResetContact();
}

}  // namespace coop::melee_fx
