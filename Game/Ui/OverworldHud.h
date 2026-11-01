#pragma once

#include "Campaign/Narrative/IntelLedger.h" // IntelVerdict
#include "Campaign/World/Encounter.h"       // EncounterTrigger
#include "Campaign/World/March.h"           // MarchPlan/MarchRules (default arg)
#include "Game/Ui/OrderMenu.h"              // MarchOption
#include "Gameplay/Result.h"                // Confirm's Result<MarchPlan>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Potato::Campaign {
class BencaoCodex;
class BencaoLibrary;
class CampaignState;
class EncounterLibrary;
class IntelLedger;
class Ledger;
class WorldMap;
class WorldState;
} // namespace Potato::Campaign

namespace Potato::Game {

// Overworld HUD (Story 13.4 — GAME_UI_UX_DESIGN.md S2): the
// campaign map's chrome. Same convention as HudView/TitleScreen —
// pure view-models the shell paints; nothing here mutates
// campaign state except MarchIntent::Confirm, which is the
// two-step commit seam into Campaign::IssueMarch.
//
//   ┌─ top bar ──────────────────────────────────────────┐
//   │ Day 12 · 開封  →  洛陽 (Day 15)   物資 民心 秩序 ☰ │
//   ├────────────────────────────── map ─┬─ drawer (Tab)─┤
//   │                                    │ 行軍令 遭遇    │
//   │                                    │ 情報 名冊      │
//   │                                    │ 帳冊 本草      │
//   └────────────────────────────────────┴───────────────┘

// --- top bar ---------------------------------------------------

struct OverworldTopBar {
    std::int64_t day = 0;
    std::string nodeId;     // warband anchor (empty = no world)
    std::string nodeName;   // display name; falls back to the id
    // In-flight march — the bar shows "→ dest (Day N)" instead of
    // a static anchor while Marching().
    bool marching = false;
    std::string marchDestId;
    std::string marchDestName;
    std::int64_t marchEta = 0;
    // Right-cluster numbers: spendable 物資 is an account balance;
    // 民心/秩序 are folds — the bar reads the same truth the
    // ledger holds, never a stored stat.
    std::int64_t materiel = 0;
    std::int64_t popularSupport = 0;
    std::int64_t order = 0;
};

// Reads world position + ledger folds. `map` may be a default
// WorldMap when the campaign carries no world — the bar then
// shows day/accounts with an empty anchor.
OverworldTopBar BuildTopBar(const Campaign::WorldMap& map,
                            const Campaign::WorldState& ws,
                            const Campaign::Ledger& ledger);

// --- drawer ----------------------------------------------------

enum class OverworldTab : std::uint8_t {
    March = 0,     // 行軍令 — MarchOptions (OrderMenu.h)
    Encounters,    // 遭遇   — PendingEncounters
    Intel,         // 情報   — IntelLedger feed
    Roster,        // 名冊   — campaign roster
    Ledger,        // 帳冊   — recent postings
    Codex,         // 本草   — unlocked/pending pages
    Auction,       // 拍賣   — recruit contract proxy auction
};
constexpr std::size_t kOverworldTabCount = 7;
// CJK section title ("行軍令", …) — presentation vocabulary.
const char* OverworldTabTitle(OverworldTab t);

// 遭遇 — encounters firing NOW (PendingEncounters order is
// canonical: library id sort).
struct EncRow {
    std::string id;
    std::string nodeId;
    std::string nodeName; // falls back to nodeId
    Campaign::EncounterTrigger trigger =
        Campaign::EncounterTrigger::Arrival;
};
// CJK trigger label (抵達/鄰近/要地) — the drawer renders it,
// the wire id stays in the doc.
const char* EncTriggerLabel(Campaign::EncounterTrigger t);
std::vector<EncRow> EncounterRows(
    const Campaign::WorldState& ws, const Campaign::WorldMap& map,
    const Campaign::EncounterLibrary& lib);

// 情報 — the claims feed. One row per IntelEntry in ledger order
// (seq ascending); the verdict IS the row's truth — Unresolved
// rows are claims, not facts.
struct IntelRow {
    std::uint64_t seq = 0;
    std::string subject;
    std::string claim;
    Campaign::IntelVerdict verdict =
        Campaign::IntelVerdict::Unresolved;
    std::int64_t chapter = -1;
};
std::vector<IntelRow> IntelRows(const Campaign::IntelLedger& l);

// 名冊 — roster spine; dead entries keep their rows (memorials
// stay visible — the drawer doesn't hide the cost).
struct SquadRow {
    std::string name;
    std::int64_t veterancy = 0;
    std::int64_t casualties = 0;
    bool dead = false;
};
std::vector<SquadRow> SquadRows(const Campaign::CampaignState& s);

// 帳冊 — the last `tail` postings, oldest-first within the tail
// (chronological read: the newest entry is the bottom row).
struct LedgerRow {
    std::uint64_t seq = 0;
    std::string credit;  // account wire id
    std::int64_t creditAmt = 0;
    std::string debit;
    std::int64_t debitAmt = 0;
    std::string memo;
    bool forged = false;
    bool suspect = false;
};
std::vector<LedgerRow> LedgerRows(const Campaign::Ledger& l,
                                  std::size_t tail = 12);

// 本草 — the codex's paper. Unlocked pages carry their library
// name/category; pending pages (awaiting 補鈔) list with
// pending=true — the Scribe knows them but can't cite them yet.
struct CodexRow {
    std::string id;
    std::string name;     // entry 正名; falls back to id
    std::string category; // section wire id ("shancao", …)
    bool pending = false;
};
std::vector<CodexRow> CodexRows(const Campaign::BencaoLibrary& lib,
                                const Campaign::BencaoCodex& codex);

// --- two-step march --------------------------------------------
//
// The design's "點節點下軍令": a node click stages a candidate,
// the confirm card commits. Nothing posts to the ledger until
// Confirm — a click is intent, not an order.

class MarchIntent {
public:
    bool Pending() const { return pending_; }
    // The staged candidate — valid only while Pending().
    const MarchOption& Option() const { return opt_; }

    // Stage `nodeId` if it is a legal march target right now
    // (adjacent, not already marching — MarchOptions is the same
    // rulebook the order menu shows). Any other click clears the
    // intent and returns false.
    bool Click(const Campaign::WorldMap& map,
               const Campaign::WorldState& ws,
               std::int64_t supplyAvailable,
               std::string_view nodeId);
    // Same, with explicit rules.
    bool Click(const Campaign::WorldMap& map,
               const Campaign::WorldState& ws,
               std::int64_t supplyAvailable,
               std::string_view nodeId,
               const Campaign::MarchRules& rules);
    void Cancel() { pending_ = false; }

    // Commit: IssueMarch with the state's real ledger. The intent
    // clears on ANY result — a rejected order doesn't linger.
    Gameplay::Result<Campaign::MarchPlan> Confirm(
        Campaign::WorldState& ws, const Campaign::WorldMap& map,
        Campaign::Ledger& ledger,
        const Campaign::MarchRules& rules = {});

private:
    bool pending_ = false;
    MarchOption opt_;
};

} // namespace Potato::Game
