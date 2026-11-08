// ui/hud.cpp -- see ui/hud.h.

#include "ui/hud.h"

#include "coop/comms/chat_feed.h"
#include "coop/dev/object_overlay.h"
#include "coop/dev/ragdoll_bone_overlay.h"
#include "coop/player/nameplate.h"
#include "coop/text/utf8_codec.h"
#include "coop/player/nick_color.h"
#include "coop/voice/voice_chat.h"
#include "ui/chat_view.h"
#include "ui/fonts.h"
#include "ui/link_format.h"
#include "ui/scale.h"
#include "ui/voice_icons.h"
#include "l10n/l10n.h"

#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ui::hud {
namespace {

using ui::scale::S;  // 1080p-authored px -> live-resolution px (ui/scale.h)

// The nameplate base text size in px, the size up close. The whole plate scales down with
// distance (about 1/distance, computed by the nameplate module), so it stays proportional to
// the peer's on-screen body instead of looming over a far, shrunken character; 16 px reads at
// a few metres without dominating when a peer stands next to you. Distance also fades the
// opacity, and the scale is capped at 1, so the plate is never bigger than this.
constexpr float kNickPx = 16.f;

// Draw `text` at `size` with a cheap outline, a 4-way offset of about 1 px at 1080p and
// proportional above, so it stays legible over any scene.
void TextOutlined(ImDrawList* dl, ImFont* font, float size, ImVec2 pos,
                  ImU32 col, ImU32 outline, const char* text) {
    const float o = std::max(1.f, S(1.f));
    dl->AddText(font, size, ImVec2(pos.x - o, pos.y), outline, text);
    dl->AddText(font, size, ImVec2(pos.x + o, pos.y), outline, text);
    dl->AddText(font, size, ImVec2(pos.x, pos.y - o), outline, text);
    dl->AddText(font, size, ImVec2(pos.x, pos.y + o), outline, text);
    dl->AddText(font, size, pos, col, text);
}

void DrawNameplate(ImDrawList* dl, const coop::nameplate::Plate& p) {
    // A plate has one parenthetical slot, so it cannot say "not applicable" the way the
    // scoreboard's cell can; it shows a number or nothing, and a negative ping renders as a bare
    // name, meaning not sampled yet. A client reads its own roster row for the host's ping, since
    // host and client are one link with one RTT; host-side the host's own row is local, and you
    // never see your own plate. The number is formatted by the shared renderer. The name and the
    // ping are drawn as two pieces (one colour each) but centred as one unit, see the anchor
    // below; the cost is that the nick's centre sits half the suffix's width left of the bar's
    // centre and slides sideways as the latency gains or loses a digit.
    char nickTxt[coop::text::kNickBufBytes];
    // A sleeper's name shows only near it (nameplate.cpp decides); far, the plate is its "z Z Z" alone.
    std::snprintf(nickTxt, sizeof(nickTxt), "%s", (p.sleeper && !p.showNick) ? "" : p.nick);
    char pingTxt[20] = {};
    if (!p.sleeper && p.linkKind != coop::net::LinkKind::Local && p.ping >= 0) {
        char pb[16];
        ui::link_format::FormatPing(p.ping, p.linkKind, pb, sizeof(pb));
        std::snprintf(pingTxt, sizeof(pingTxt), " (%s)", pb);
    }

    // Occlusion, the minecraft nametag shape: a peer behind world geometry keeps a readable plate,
    // but the whole unit (the nick and the health bar; the badge and the outline ride the same
    // alpha) goes grey and half-transparent, so behind-something reads at a glance and nothing
    // vanishes. The hurt flash keeps priority.
    const float a = std::clamp(p.alpha, 0.f, 1.f) * (p.occluded ? 0.5f : 1.f);
    const ImU32 white   = IM_COL32(255, 255, 255, static_cast<int>(a * 245.f));
    const ImU32 gray    = IM_COL32(158, 158, 164, static_cast<int>(a * 245.f));
    const ImU32 red     = IM_COL32(255, 48, 48, static_cast<int>(a * 255.f));
    const ImU32 outline = IM_COL32(0, 0, 0, static_cast<int>(a * 215.f));
    // The peer's custom nick colour replaces the white base; the flash and occluded colours keep
    // priority, since a hurt or hidden peer must read as such regardless of the cosmetic pick.
    const ImU32 base = coop::nick_color::IsCustom(p.colorRGB)
        ? IM_COL32(coop::nick_color::R(p.colorRGB), coop::nick_color::G(p.colorRGB),
                   coop::nick_color::B(p.colorRGB), static_cast<int>(a * 245.f))
        : white;
    const ImU32 textCol = p.flash ? red : (p.occluded ? gray : base);

    // The distance size scale: the whole plate (text, bar, box, gaps) scales as one unit, so a far
    // peer's label stays proportional to their shrunken body. The scale is 1 up close, capped, and
    // shrinks about 1/distance to a legible floor.
    const float s = std::clamp(p.scale, 0.20f, 1.f);
    const float px = S(kNickPx) * s;
    const float barW = S(44.f) * s, barH = S(5.f) * s;
    const float gap = S(10.f) * s;    // nick sits sz.y + gap above the head anchor
    const float barGap = S(4.f) * s;  // bar sits barGap below the anchor

    ImFont* font = ui::fonts::FontFor(ui::fonts::Role::Nameplate);  // per-role font (F1 > Interface)
    if (!font) font = ImGui::GetFont();
    // Measured separately because they carry one colour each and are drawn as two calls, not
    // because the anchor centres the nick alone; it does not.
    const ImVec2 sz     = font->CalcTextSizeA(px, FLT_MAX, 0.f, nickTxt);
    const ImVec2 pingSz = pingTxt[0] ? font->CalcTextSizeA(px, FLT_MAX, 0.f, pingTxt)
                                     : ImVec2(0.f, 0.f);

    // The overhead chat bubble rows, split before the on-screen clamp so the clamp can reserve the
    // bubble's height; an unclamped stack rendered off the top when looking up at a nearby peer.
    const float bpx = px * 0.88f;
    const float rowH = bpx + S(1.f) * s;
    constexpr int kMaxRows = 5;
    struct BubbleRow { const char* b; const char* e; };
    BubbleRow rows[kMaxRows];
    int nRows = 0;
    if (p.bubbleAlpha > 0.f && p.bubble[0]) {
        const float wrapW = S(240.f) * s;
        const char* sPtr = p.bubble;
        const char* bEnd = p.bubble + std::strlen(p.bubble);
        while (sPtr < bEnd && nRows < kMaxRows) {
            const char* rowEnd = font->CalcWordWrapPositionA(bpx / font->LegacySize, sPtr, bEnd, wrapW);
            if (rowEnd == sPtr) rowEnd = sPtr + 1;  // never stall on a single overlong glyph
            rows[nRows++] = {sPtr, rowEnd};
            sPtr = rowEnd;
            while (sPtr < bEnd && *sPtr == ' ') ++sPtr;  // swallow the wrap-point space
        }
    }
    const float bubbleH = nRows > 0 ? rowH * static_cast<float>(nRows) + S(4.f) * s : 0.f;

    // Keep the whole nameplate on-screen: a peer in front of you whose head sits past a screen
    // edge still shows the label at the edge instead of vanishing.
    const ImGuiIO& io = ImGui::GetIO();
    const float m = S(6.f);
    // The whole label is centred on the bar, the nick and the ping as one unit, so the plate is
    // symmetric about the anchor. The known cost is accepted: the nick shifts a few pixels when
    // the ping gains or loses a digit; do not change it back to nick-only centring. MTA's
    // nametags do the same, drawing the whole string centred on the health bar's X.
    const float labelW = sz.x + pingSz.x;
    const float ext = std::max(labelW, barW) * 0.5f;
    const float loX = m + ext, hiX = io.DisplaySize.x - m - ext;
    const float ax = (loX <= hiX) ? std::clamp(p.x, loX, hiX) : p.x;
    const float ay = std::clamp(p.y, m + sz.y + gap + bubbleH, io.DisplaySize.y - m - barH - barGap);
    // Both on whole pixels: ImGui truncates a text origin to the pixel grid, and a bar left at a
    // sub-pixel position would slide up to a pixel against the nick as the plate moves.
    const ImVec2 textPos(std::round(ax - labelW * 0.5f), std::round(ay - sz.y - gap));
    const ImVec2 bp(std::round(ax - barW * 0.5f), std::round(ay - barGap));

    // The plate extents (the nick and bar union), kept for the voice-badge anchor below. No
    // backing box is drawn: readability rides the text outline and the health bar's own outline.
    const float padX = S(6.f) * s, padY = S(3.f) * s;
    const ImVec2 boxMin(std::min(textPos.x, bp.x) - padX, textPos.y - padY);
    const ImVec2 boxMax(std::max(textPos.x + sz.x + pingSz.x, bp.x + barW) + padX,
                        bp.y + barH + padY);

    TextOutlined(dl, font, px, textPos, textCol, outline, nickTxt);
    // The annotation hangs off the nick's right edge, but the pair is centred as one unit (the
    // anchor above), so this is a layout continuation, not an exemption from the centring.
    if (pingTxt[0]) {
        TextOutlined(dl, font, px, ImVec2(textPos.x + sz.x, textPos.y),
                     textCol, outline, pingTxt);
    }

    // The overhead chat bubble, the MTA and SAMP shape: the peer's last chat message, word-wrapped
    // and centred above the nick, its own hold and fade multiplied into the plate's distance and
    // occlusion alpha. Outlined text only, no backing box, like the plate. The rows were split
    // above, before the clamp, which reserved their height; capped, so a max-length line cannot
    // tower over the scene.
    if (nRows > 0) {
        const float ba = a * std::clamp(p.bubbleAlpha, 0.f, 1.f);
        const ImU32 bCol = IM_COL32(255, 255, 255, static_cast<int>(ba * 235.f));
        const ImU32 bOut = IM_COL32(0, 0, 0, static_cast<int>(ba * 205.f));
        float y = textPos.y - S(4.f) * s - rowH * static_cast<float>(nRows);
        for (int r = 0; r < nRows; ++r) {
            char buf[224];
            const size_t len = std::min(static_cast<size_t>(rows[r].e - rows[r].b),
                                        sizeof(buf) - 1);
            std::memcpy(buf, rows[r].b, len);
            buf[len] = '\0';
            const ImVec2 rsz = font->CalcTextSizeA(bpx, FLT_MAX, 0.f, buf);
            TextOutlined(dl, font, bpx, ImVec2(ax - rsz.x * 0.5f, y), bCol, bOut, buf);
            y += rowH;
        }
    }

    // The health bar. While occluded the fill stays red, a darker and more transparent red than
    // the normal fill; `a` already carries the occlusion halving, so the lower constant stacks on
    // it. The nick takes the grey treatment; the hurt flash wins.
    const float frac = std::clamp(p.healthPct / 100.f, 0.f, 1.f);
    if (p.sleeper) {
        // No health bar on a sleeper; the loot hint sits where it would be, while the viewer can loot it. A joining
        // body is never lootable; it says the player is on the way in.
        if (p.lootHint || p.joining) {
            const auto step = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count() / 450 % 3;
            const char* hint = p.joining
                ? (step == 0 ? l10n::T("(joining.)") : step == 1 ? l10n::T("(joining..)")
                                                               : l10n::T("(joining...)"))
                : l10n::T("[E] Loot");
            const float hpx = px * 0.8f;
            const ImVec2 hsz = font->CalcTextSizeA(hpx, FLT_MAX, 0.f, hint);
            TextOutlined(dl, font, hpx, ImVec2(ax - hsz.x * 0.5f, bp.y), IM_COL32(230, 230, 200, static_cast<int>(a * 245.f)),
                         outline, hint);
        } else if (p.wakeHint != 0) {
            // An online sleeper the viewer may push awake: the press, or the cooldown the host named (dimmer).
            char cool[48];
            const bool cooling = p.wakeHint == 2;
            const char* hint = l10n::T("[RMB x2] Wake");
            if (cooling && l10n::Fmt(cool, sizeof(cool), l10n::T("Cooldown: %1$d:%2$02d"), p.wakeSecs / 60, p.wakeSecs % 60) >= 0)
                hint = cool;
            const float hpx = px * 0.8f;
            const ImVec2 hsz = font->CalcTextSizeA(hpx, FLT_MAX, 0.f, hint);
            TextOutlined(dl, font, hpx, ImVec2(ax - hsz.x * 0.5f, bp.y),
                         cooling ? IM_COL32(170, 170, 178, static_cast<int>(a * 235.f))
                                 : IM_COL32(230, 230, 200, static_cast<int>(a * 245.f)),
                         outline, hint);
        }
    } else {
    dl->AddRectFilled(bp, ImVec2(bp.x + barW, bp.y + barH), IM_COL32(0, 0, 0, static_cast<int>(a * 160.f)));
    const ImU32 fillCol = p.flash    ? red
                        : p.occluded ? IM_COL32(120, 18, 18, static_cast<int>(a * 220.f))
                                     : IM_COL32(190, 30, 30, static_cast<int>(a * 235.f));
    dl->AddRectFilled(bp, ImVec2(bp.x + barW * frac, bp.y + barH), fillCol);
    dl->AddRect(bp, ImVec2(bp.x + barW, bp.y + barH), IM_COL32(0, 0, 0, static_cast<int>(a * 200.f)));
    }

    // Sleep Zs: a sleeping peer's plate sheds "z Z Z" from its upper-right corner. Three letters
    // share one looping life, a third of a life apart, so a new one leaves every ~0.8 s; each drifts
    // up and to the right with a small sway, grows, and fades in then out. Pure function of the
    // clock and the sleeping flag, so it starts and stops with the flag, with no state kept.
    if (p.sleeping) {
        constexpr int   kZs      = 3;
        constexpr float kLifeSec = 2.4f;
        static const char* const kGlyph[kZs] = {"z", "Z", "Z"};
        const double now = ImGui::GetTime();
        const ImU32 zCol = IM_COL32(190, 215, 255, 255);
        for (int i = 0; i < kZs; ++i) {
            const float u = std::fmod(static_cast<float>(now) + kLifeSec * i / kZs, kLifeSec) / kLifeSec;
            const float fade = std::min(u / 0.15f, 1.f) * (1.f - u);
            const float za = a * std::clamp(fade, 0.f, 1.f);
            if (za <= 0.01f) continue;
            const float zpx = px * (0.55f + 0.65f * u);
            const ImVec2 zp(boxMax.x - S(10.f) * s + u * S(26.f) * s + std::sin(u * 9.f) * S(3.f) * s,
                            textPos.y - u * S(38.f) * s - zpx * 0.4f);
            TextOutlined(dl, font, zpx, zp,
                         (zCol & 0x00FFFFFFu) | (static_cast<ImU32>(za * 245.f) << 24),
                         IM_COL32(0, 0, 0, static_cast<int>(za * 215.f)), kGlyph[i]);
        }
    }

    // Tipsy: a few small bubbles drift up off the plate's left end, wobbling and swelling as they rise, wine-pink with a
    // bright rim. A pure function of the clock and the flag, like the Zs, so it needs no state; the Zs and the voice
    // badge sit on the other side. The bubbles ride the plate's alpha (distance fade, occlusion) and its scale.
    if (p.tipsy) {
        constexpr int kBubbles = 5;
        const double now = ImGui::GetTime();
        const float rise = S(36.f) * s;
        for (int i = 0; i < kBubbles; ++i) {
            const float life = 2.3f + 0.37f * static_cast<float>(i);
            const float u = std::fmod(static_cast<float>(now) + 0.61f * static_cast<float>(i), life) / life;
            const float fade = std::min(u / 0.2f, 1.f) * (1.f - u);
            const float ba = a * std::clamp(fade, 0.f, 1.f);
            if (ba <= 0.01f) continue;
            const float lane = (static_cast<float>(i % 3) - 1.f) * S(4.f) * s;
            const float wobble = std::sin(static_cast<float>(now) * 2.7f + static_cast<float>(i) * 1.9f) * S(3.5f) * s * (0.4f + u);
            const float r = S(2.1f + 0.9f * static_cast<float>((i * 7) % 4)) * s * (0.7f + 0.5f * u);
            ImVec2 c(boxMin.x - S(7.f) * s + lane + wobble, textPos.y + sz.y * 0.55f - u * rise);
            c.y = std::max(c.y, r + 1.f);   // never off the top of the screen
            dl->AddCircleFilled(c, r, IM_COL32(240, 120, 190, static_cast<int>(ba * 70.f)), 14);
            dl->AddCircle(c, r, IM_COL32(255, 175, 225, static_cast<int>(ba * 225.f)), 14, std::max(1.f, S(1.1f) * s));
            dl->AddCircleFilled(ImVec2(c.x - r * 0.35f, c.y - r * 0.35f), std::max(0.7f, r * 0.22f),
                                IM_COL32(255, 255, 255, static_cast<int>(ba * 200.f)), 6);
        }
    }

    // The voice badge: right of the plate extents, scaled and faded with it.
    if (p.voiceIcon != 0) {
        const float ih = S(13.f) * s;
        ui::voice_icons::Draw(dl, ImVec2(boxMax.x + S(4.f) * s + ih * 0.5f,
                                         (boxMin.y + boxMax.y) * 0.5f),
                              ih, static_cast<coop::voice_chat::VoiceIcon>(p.voiceIcon), a);
    }
}

void DrawNameplates() {
    coop::nameplate::Snapshot ns;
    coop::nameplate::GetSnapshot(ns);
    if (ns.count <= 0) return;
    // The background draw list: over the game scene but under every ImGui window, since nameplates
    // are the lowest-priority layer and the scoreboard, the voice panel and the browser must never
    // be overdrawn by a plate. It renders in the same draw data as everything else, so the
    // screenshot grab captures it; it never hit-tests input.
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    for (int i = 0; i < ns.count; ++i) {
        const auto& p = ns.plates[i];
        if (!p.onScreen || p.alpha <= 0.02f) continue;
        DrawNameplate(dl, p);
    }
}

// The dev object-overlay labels: a small anchor dot at each projected object and up to three
// stacked text lines (names, net identity, physics layers; empty lines skipped). Line 1 is
// tinted by tracking kind, so the problem class jumps out: cyan is a wire mirror, green a
// tracked local, orange an untracked local-only object, the kind that cannot mirror-sync.
void DrawObjectOverlay() {
    namespace OO = coop::dev::object_overlay;
    if (!OO::IsEnabled()) return;
    OO::Snapshot os;
    OO::GetSnapshot(os);

    ImDrawList* dl = ImGui::GetBackgroundDrawList();  // under windows, like the nameplates
    ImFont* font = ImGui::GetFont();
    const ImGuiIO& io = ImGui::GetIO();

    // The always-on status line, top right: proves the overlay is live and shows the in-range
    // tracked and untracked counts even when no label is on screen.
    if (os.status[0]) {
        const float statusPx = S(13.f);
        const ImVec2 sz = font->CalcTextSizeA(statusPx, FLT_MAX, 0.f, os.status);
        TextOutlined(dl, font, statusPx,
                     ImVec2(io.DisplaySize.x - sz.x - S(10.f), S(8.f)),
                     IM_COL32(255, 235, 130, 235), IM_COL32(0, 0, 0, 200), os.status);
    }

    for (int i = 0; i < os.count; ++i) {
        const auto& L = os.labels[i];
        if (L.alpha <= 0.02f) continue;
        // Cull labels projected outside the viewport; a small margin keeps a label attached to an
        // object sliding off the edge from popping.
        const float cm = S(60.f);
        if (L.x < -cm || L.y < -cm ||
            L.x > io.DisplaySize.x + cm || L.y > io.DisplaySize.y + cm) continue;

        // The same billboard shape as the nameplates: base size up close, about 1/distance shrink
        // to a legible floor.
        const float s = (L.dist <= 600.f) ? 1.f : std::max(600.f / L.dist, 0.45f);
        const float a = std::clamp(L.alpha, 0.f, 1.f);

        ImU32 kindCol;
        switch (L.kind) {
            case 0:  kindCol = IM_COL32(110, 205, 255, static_cast<int>(a * 245.f)); break;
            case 1:  kindCol = IM_COL32(150, 255, 150, static_cast<int>(a * 245.f)); break;
            default: kindCol = IM_COL32(255, 170, 64,  static_cast<int>(a * 255.f)); break;
        }
        const ImU32 grey    = IM_COL32(225, 225, 225, static_cast<int>(a * 225.f));
        const ImU32 outline = IM_COL32(0, 0, 0, static_cast<int>(a * 210.f));

        dl->AddCircleFilled(ImVec2(L.x, L.y), S(2.5f) * s, kindCol);

        const float px1 = S(13.f) * s, px2 = S(11.f) * s;
        float tx = L.x + S(7.f) * s;
        float ty = L.y - S(6.f) * s;
        if (L.line1[0]) { TextOutlined(dl, font, px1, ImVec2(tx, ty), kindCol, outline, L.line1); ty += px1 + S(1.f); }
        if (L.line2[0]) { TextOutlined(dl, font, px2, ImVec2(tx, ty), grey, outline, L.line2);   ty += px2 + S(1.f); }
        if (L.line3[0]) { TextOutlined(dl, font, px2, ImVec2(tx, ty), grey, outline, L.line3);   ty += px2 + S(1.f); }
        if (L.line4[0]) {
            // The health or process ramp: green when full, through yellow, red when empty; grey
            // when no maximum is known, since a bare pool has no meaningful fraction.
            ImU32 hcol = grey;
            if (L.healthFrac >= 0.f) {
                const float f = L.healthFrac;
                const int rr = static_cast<int>(255.f * std::min(1.f, 2.f - 2.f * f));
                const int gg = static_cast<int>(255.f * std::min(1.f, 2.f * f));
                hcol = IM_COL32(rr, gg, 40, static_cast<int>(a * 245.f));
            }
            TextOutlined(dl, font, px2, ImVec2(tx, ty), hcol, outline, L.line4);
        }
    }
}

}  // namespace

bool IsActive() {
    // The live-lines check covers only live lines; the reveal check is the other half: while the
    // chat history is on screen, including the fade-out after a close, the frame must keep being
    // built even if every live line expired, or the fade draws zero frames and the block vanishes
    // instead of dimming.
    return coop::nameplate::HasAny() || coop::chat_feed::HasAny() ||
           coop::chat_feed::RevealActive() ||
           coop::dev::object_overlay::IsEnabled() ||
           coop::dev::ragdoll_bone_overlay::IsEnabled() ||
           coop::voice_chat::Enabled();  // the local mic indicator works before a join too
}

// The local voice indicator: a compact bottom-left icon (talking, whispering, muted,
// disconnected; push-to-talk idle shows nothing). Reads the published snapshot, since this
// runs on the render thread and the live chain walks game-thread state.
void DrawLocalVoiceIcon() {
    coop::voice_chat::UiSnapshot vs;
    coop::voice_chat::GetUiSnapshot(vs);
    if (!vs.enabled || !vs.started) return;
    const auto icon = static_cast<coop::voice_chat::VoiceIcon>(vs.localIcon);
    if (icon == coop::voice_chat::VoiceIcon::None) return;
    // Hide the disconnected badge when solo: a lone host or client has not failed at anything,
    // there is nobody to talk to yet, and the no-signal glyph was noise that vanished the instant
    // a peer joined. With a peer present the icon means a real transport-down state.
    if (icon == coop::voice_chat::VoiceIcon::Disconnected && !coop::nameplate::HasAny()) return;
    const ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();  // under windows, like the nameplates
    // Offset right of the left edge so it clears the game's native bottom-left vitals column (the
    // food and stamina icons and their numbers); a compact badge near the bottom edge.
    ui::voice_icons::Draw(dl, ImVec2(S(170.f), io.DisplaySize.y - S(36.f)), S(28.f), icon, 0.9f);
}

// The dev ragdoll skeleton overlay: bone-to-parent lines and joint dots for every active
// ragdoll body. Orange is the local player's own native ragdoll; cyan a remote peer's mirror
// body.
void DrawRagdollBones() {
    namespace RB = coop::dev::ragdoll_bone_overlay;
    if (!RB::IsEnabled()) return;
    RB::Snapshot rs;
    RB::GetSnapshot(rs);

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImFont* font = ImGui::GetFont();
    const ImGuiIO& io = ImGui::GetIO();

    if (rs.status[0]) {
        const float statusPx = S(13.f);
        const ImVec2 sz = font->CalcTextSizeA(statusPx, FLT_MAX, 0.f, rs.status);
        TextOutlined(dl, font, statusPx,
                     ImVec2(io.DisplaySize.x - sz.x - S(10.f), S(26.f)),
                     IM_COL32(255, 190, 120, 235), IM_COL32(0, 0, 0, 200), rs.status);
    }

    for (int i = 0; i < rs.count; ++i) {
        const auto& L = rs.lines[i];
        // Cull segments fully outside the viewport; the margin keeps partially visible limbs.
        const float m = S(80.f);
        if ((L.x1 < -m && L.x2 < -m) || (L.y1 < -m && L.y2 < -m) ||
            (L.x1 > io.DisplaySize.x + m && L.x2 > io.DisplaySize.x + m) ||
            (L.y1 > io.DisplaySize.y + m && L.y2 > io.DisplaySize.y + m)) continue;
        const ImU32 col = (L.kind == 0) ? IM_COL32(255, 170, 64, 235)    // local native ragdoll
                                        : IM_COL32(110, 205, 255, 235);  // remote mirror body
        dl->AddLine(ImVec2(L.x1, L.y1), ImVec2(L.x2, L.y2), col, S(1.6f));
        dl->AddCircleFilled(ImVec2(L.x1, L.y1), S(2.2f), col);
        dl->AddCircleFilled(ImVec2(L.x2, L.y2), S(2.2f), col);
    }
}

void Render() {
    DrawObjectOverlay();   // first: debug labels sit UNDER the player nameplates
    DrawRagdollBones();    // skeleton lines under the nameplates too
    DrawNameplates();
    ui::chat_view::Draw();  // the feed + the T-activated history reveal (ui/chat_view.h)
    DrawLocalVoiceIcon();
}

}  // namespace ui::hud
