#include "Game/Ui/OverworldHud.h"

#include "Campaign/Governance/Accumulators.h"
#include "Campaign/Ledger/Ledger.h"
#include "Campaign/Narrative/Bencao.h"
#include "Campaign/Narrative/BencaoCodex.h"
#include "Campaign/State/CampaignState.h"
#include "Campaign/World/March.h"
#include "Campaign/World/WorldMap.h"
#include "Campaign/World/WorldState.h"

namespace Potato::Game {

namespace {

// Node display name — authored name wins; bare ids show as
// themselves (unlabeled ground is still nameable ground).
std::string NodeLabel(const Campaign::WorldMap& map,
                      const std::string& id) {
    if (id.empty()) return {};
    const Campaign::WorldNode* n = map.FindNode(id);
    if (n != nullptr && !n->name.empty()) return n->name;
    return id;
}

} // namespace

OverworldTopBar BuildTopBar(const Campaign::WorldMap& map,
                            const Campaign::WorldState& ws,
                            const Campaign::Ledger& ledger) {
    OverworldTopBar bar;
    bar.day = ws.Day();
    bar.nodeId = ws.WarbandAt();
    bar.nodeName = NodeLabel(map, bar.nodeId);
    if (ws.Marching()) {
        bar.marching = true;
        bar.marchDestId = ws.MarchDest();
        bar.marchDestName = NodeLabel(map, bar.marchDestId);
        bar.marchEta = ws.MarchEta();
    }
    bar.materiel = ledger.Balance(Campaign::Account::Materiel);
    const auto gov = Campaign::FoldGovernance(ledger);
    bar.popularSupport = gov.popularSupport;
    bar.order = gov.order;
    return bar;
}

const char* OverworldTabTitle(OverworldTab t) {
    switch (t) {
        case OverworldTab::March:      return "行軍令";
        case OverworldTab::Encounters: return "遭遇";
        case OverworldTab::Intel:      return "情報";
        case OverworldTab::Roster:     return "名冊";
        case OverworldTab::Ledger:     return "帳冊";
        case OverworldTab::Codex:      return "本草";
        case OverworldTab::Auction:    return "拍賣";
    }
    return "?";
}

const char* EncTriggerLabel(Campaign::EncounterTrigger t) {
    switch (t) {
        case Campaign::EncounterTrigger::Arrival:
            return "抵達";
        case Campaign::EncounterTrigger::Proximity:
            return "鄰近";
        case Campaign::EncounterTrigger::Poi:
            return "要地";
    }
    return "?";
}

std::vector<EncRow> EncounterRows(
    const Campaign::WorldState& ws, const Campaign::WorldMap& map,
    const Campaign::EncounterLibrary& lib) {
    std::vector<EncRow> rows;
    for (const Campaign::EncounterDef* enc :
         Campaign::PendingEncounters(ws, map, lib)) {
        EncRow r;
        r.id = enc->id;
        r.nodeId = enc->node;
        r.nodeName = NodeLabel(map, enc->node);
        r.trigger = enc->trigger;
        rows.push_back(std::move(r));
    }
    return rows;
}

std::vector<IntelRow> IntelRows(const Campaign::IntelLedger& l) {
    std::vector<IntelRow> rows;
    rows.reserve(l.Size());
    for (const auto& e : l.Entries()) {
        IntelRow r;
        r.seq = e.seq;
        r.subject = e.subject;
        r.claim = e.claim;
        r.verdict = e.verdict;
        r.chapter = e.chapter;
        rows.push_back(std::move(r));
    }
    return rows;
}

std::vector<SquadRow> SquadRows(
    const Campaign::CampaignState& s) {
    std::vector<SquadRow> rows;
    rows.reserve(s.GetRoster().size());
    for (const auto& e : s.GetRoster()) {
        SquadRow r;
        r.name = e.name;
        r.veterancy = e.veterancy;
        r.casualties = e.casualties;
        r.dead = e.dead;
        rows.push_back(std::move(r));
    }
    return rows;
}

std::vector<LedgerRow> LedgerRows(const Campaign::Ledger& l,
                                  std::size_t tail) {
    std::vector<LedgerRow> rows;
    const auto& entries = l.Entries();
    const std::size_t start =
        entries.size() > tail ? entries.size() - tail : 0;
    rows.reserve(entries.size() - start);
    for (std::size_t i = start; i < entries.size(); ++i) {
        const auto& e = entries[i];
        LedgerRow r;
        r.seq = e.seq;
        r.credit = Campaign::AccountName(e.credit.account);
        r.creditAmt = e.credit.amount;
        r.debit = Campaign::AccountName(e.debit.account);
        r.debitAmt = e.debit.amount;
        r.memo = e.memo;
        r.forged = e.provenance == Campaign::Provenance::Forged;
        r.suspect = e.suspect;
        rows.push_back(std::move(r));
    }
    return rows;
}

std::vector<CodexRow> CodexRows(
    const Campaign::BencaoLibrary& lib,
    const Campaign::BencaoCodex& codex) {
    std::vector<CodexRow> rows;
    auto add = [&lib, &rows](const std::string& id, bool pending) {
        CodexRow r;
        r.id = id;
        if (const Campaign::BencaoEntry* e = lib.Find(id)) {
            r.name = e->name;
            r.category =
                Campaign::BencaoCategoryName(e->category);
        } else {
            r.name = id; // page without a doc — cite the id
        }
        r.pending = pending;
        rows.push_back(std::move(r));
    };
    // Delivery order for the written pages (the book's own
    // history), then the queue order for pages awaiting 補鈔.
    for (const std::string& id : codex.Unlocked()) {
        add(id, false);
    }
    for (const Campaign::PendingPage& p : codex.Pending()) {
        add(p.id, true);
    }
    return rows;
}

bool MarchIntent::Click(const Campaign::WorldMap& map,
                        const Campaign::WorldState& ws,
                        std::int64_t supplyAvailable,
                        std::string_view nodeId) {
    for (const MarchOption& o : MarchOptions(map, ws,
                                             supplyAvailable)) {
        if (o.nodeId == nodeId) {
            opt_ = o;
            pending_ = true;
            return true;
        }
    }
    pending_ = false;
    return false;
}

bool MarchIntent::Click(const Campaign::WorldMap& map,
                        const Campaign::WorldState& ws,
                        std::int64_t supplyAvailable,
                        std::string_view nodeId,
                        const Campaign::MarchRules& rules) {
    for (const MarchOption& o :
         MarchOptions(map, ws, supplyAvailable, rules)) {
        if (o.nodeId == nodeId) {
            opt_ = o;
            pending_ = true;
            return true;
        }
    }
    pending_ = false;
    return false;
}

Gameplay::Result<Campaign::MarchPlan> MarchIntent::Confirm(
    Campaign::WorldState& ws, const Campaign::WorldMap& map,
    Campaign::Ledger& ledger,
    const Campaign::MarchRules& rules) {
    if (!pending_) {
        return Gameplay::Fail<Campaign::MarchPlan>(
            "intent", "no march staged");
    }
    const std::string dest = opt_.nodeId;
    pending_ = false;
    return Campaign::IssueMarch(ws, map, ledger, dest, rules);
}

} // namespace Potato::Game
