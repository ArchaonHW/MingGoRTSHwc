// potato_game — the windowed shell (Story 13.9): the full
// campaign flow on real hardware. ScreenFlow drives navigation;
// every screen paints the headless view-models built in
// Game/Ui/* — this file owns only ImGui input plumbing and the
// calls a scripted caller would make.
//
//   S0 Title → S1 Creation / load → S2 Overworld → S3 Battle
//   (Planning → Execution pump → Aftermath report) → S3d
//   Settlement → S4 Ending folio → S0. Esc overlay on top.
//
// Determinism note: the sim is unchanged — the shell only reads
// state and issues the same canonical calls (IssueMarch,
// ResolveBeats, Tick) a scripted caller would.

#include "Game/Shell/GlShell.h"
#include "Game/Hud/HudView.h"
#include "Game/Render/BattleView.h"
#include "Game/Render/MapProjection.h"
#include "Game/Render/OverworldView.h"
#include "Game/Render/ReportView.h"
#include "Game/Render/SpriteAtlas.h"
#include "Game/Ui/AftermathScreen.h"
#include "Game/Ui/ActionNotice.h"
#include "Game/Ui/BattleCommands.h"
#include "Game/Ui/BattleSession.h"
#include "Game/Ui/CreationForm.h"
#include "Game/Ui/EndingScreen.h"
#include "Game/Ui/EncounterLaunch.h"
#include "Game/Ui/OverworldHud.h"
#include "Game/Ui/PauseOverlay.h"
#include "Game/Ui/ScreenFlow.h"
#include "Game/Ui/SettingsForm.h"
#include "Game/Ui/TitleScreen.h"
#include "Campaign/Characters/Character.h"
#include "Campaign/Characters/CommanderBind.h"
#include "Campaign/Ledger/Ledger.h"
#include "Campaign/Myth/MythLog.h"
#include "Campaign/Narrative/BattleReport.h"
#include "Campaign/Narrative/Bencao.h"
#include "Campaign/Narrative/BencaoCodex.h"
#include "Campaign/Narrative/IntelLedger.h"
#include "Campaign/Save/SaveSystem.h"
#include "Campaign/State/CampaignState.h"
#include "Campaign/World/Encounter.h"
#include "Campaign/World/EncounterSettle.h"
#include "Campaign/World/March.h"
#include "Campaign/World/WorldMap.h"
#include "Campaign/World/WorldState.h"
#include "Gameplay/AI/BattleAI.h"
#include "Gameplay/Doctrine/Doctrine.h"
#include "Gameplay/Map/BattleMap.h"
#include "Gameplay/Squad/Squad.h"

#include <imgui.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace Potato;

// First loadable atlas in the dir — atlases are per-context
// content; the overworld ships one (assets/atlases/overworld).
Game::SpriteAtlas LoadAtlas(const std::filesystem::path& dir) {
    if (std::filesystem::exists(dir)) {
        for (const auto& e :
             std::filesystem::directory_iterator(dir)) {
            if (e.path().extension() != ".json") continue;
            auto a = Game::SpriteAtlas::Load(e.path().string());
            if (a.ok()) return std::move(a.value);
        }
    }
    Game::SpriteAtlas a;
    return a;
}

// Battle maps are content files too — one potato.map/1 per
// encounter field, keyed by doc id. std::map keeps the nodes
// address-stable: BattleController borrows a const ref for the
// battle's whole lifetime.
Game::BattleMapCatalog LoadBattleMaps(
    const std::filesystem::path& dir) {
    Game::BattleMapCatalog maps;
    if (!std::filesystem::exists(dir)) return maps;
    for (const auto& e :
         std::filesystem::directory_iterator(dir)) {
        if (e.path().extension() != ".json") continue;
        auto m = Gameplay::BattleMap::Load(e.path().string());
        if (m.ok()) maps.emplace(m.value.Id(), std::move(m.value));
    }
    return maps;
}

// One campaign's live state — the stores the screens fold.
// intel/myth/codex live inside CampaignState (resident stores)
// so the save envelope carries them; `deck` stays run-local
// (the character doc already persists it).
struct Run {
    Campaign::CampaignState state;
    std::vector<std::string> deck; // the commander's sheet
};

} // namespace

int main() {
    namespace fs = std::filesystem;

    // ---------- content registries (boot-time, immutable) ------
    Campaign::WorldLibrary worlds;
    const auto wl =
        Campaign::WorldLibrary::Load("assets/worlds", worlds);
    if (!wl.ok || worlds.Size() == 0) {
        std::fprintf(stderr, "no world in assets/worlds\n");
        return 1;
    }
    const Campaign::WorldMap& map = worlds.Worlds().front();

    Campaign::CharacterLibrary chars;
    Campaign::CharacterLibrary::Load("assets/characters", chars);
    Campaign::EncounterLibrary encs;
    Campaign::EncounterLibrary::Load("assets/encounters", encs);
    Campaign::BencaoLibrary bencaoLib;
    auto auctionLotResult =
        Campaign::AuctionLot::Load("assets/auctions/militia_contract.json");
    if (!auctionLotResult.ok()) {
        std::fprintf(stderr, "auction lot unavailable: %s\n",
                     auctionLotResult.reason.c_str());
        return 1;
    }
    const Campaign::AuctionLot auctionLot =
        std::move(auctionLotResult.value);
    Campaign::BencaoLibrary::Load("assets/bencao", bencaoLib);

    auto cardsR = Gameplay::DoctrineLibrary::Load(
        "assets/doctrine/core.json");
    if (!cardsR.ok()) {
        std::fprintf(stderr, "doctrine library failed\n");
        return 1;
    }
    Gameplay::DoctrineLibrary cards = std::move(cardsR.value);
    auto squadsR = Gameplay::SquadTemplateLibrary::Load(
        "assets/squads/core.json");
    if (!squadsR.ok()) {
        std::fprintf(stderr, "squad library failed\n");
        return 1;
    }
    Gameplay::SquadTemplateLibrary squads =
        std::move(squadsR.value);
    auto battleMaps = LoadBattleMaps("assets/maps");
    Game::SpriteAtlas atlas = LoadAtlas("assets/atlases");

    Campaign::SaveSystem saves("saves");
    fs::create_directories(saves.Dir());

    // The title's ambient backdrop — a fresh world standing in
    // for "no campaign loaded yet" (the design's "first
    // impression is the world").
    Campaign::WorldState idleWs;
    {
        auto r = Campaign::WorldState::Init(map, 0x9d2c5680ull);
        if (r.ok()) idleWs = std::move(r.value);
    }

    Game::GlShell shell;
    if (!shell.Init(960, 600, "ChinaWarlords — 民國史詩")) {
        std::fprintf(stderr, "shell init failed\n");
        return 1;
    }

    // ---------- settings (potato.settings/1) ----------
    // Missing file = first run defaults; a present-but-broken
    // file also falls back (stderr note) — the alternative is
    // refusing to launch over a preference file.
    Game::GameSettings settings;
    {
        auto s0 = Game::GameSettings::Load("settings.json");
        if (s0.ok()) {
            settings = s0.value;
        } else {
            std::fprintf(stderr,
                         "settings.json unreadable (%s) — "
                         "defaults\n",
                         s0.reason.c_str());
        }
    }
    Game::GameSettings draft = settings;
    bool settingsOpen = false;
    // baseStyle is captured post-Init so apply can re-scale
    // from stock every time (ScaleAllSizes multiplies).
    const ImGuiStyle baseStyle = ImGui::GetStyle();
    auto applySettings = [&] {
        ImGui::GetStyle() = baseStyle;
        ImGui::GetStyle().ScaleAllSizes(
            Game::DensityScale(settings.density));
        shell.ResizeWindow(settings.Res().w,
                           settings.Res().h);
    };
    applySettings();

    // ---------- campaign state ----------
    Game::ScreenFlow flow;
    std::unique_ptr<Run> run;

    auto newCampaign = [&](const Campaign::Character& c) {
        run = std::make_unique<Run>();
        auto wsr = Campaign::WorldState::Init(map, 0x9d2c5680ull);
        if (wsr.ok()) {
            run->state.SetWorld(std::move(wsr.value));
        }
        run->state.SetCharacter(c);
        // War chest — the marches' supply budget.
        Campaign::Posting seed;
        seed.credit = {Campaign::Account::Materiel, 200};
        seed.debit = {Campaign::Account::ArmyPrestige, 200};
        seed.memo = "起兵糧秣";
        run->state.GetLedger().Post(seed);
        Campaign::Enlist(run->state, "先鋒營");
        Campaign::Enlist(run->state, "輕騎隊");
        run->state.GetAuction().Start(auctionLot,
                                      run->state.GetWorld().Day());
        run->deck = c.deck.empty()
                        ? Game::RollDeck(c.deckSeed, cards)
                        : c.deck;
        Campaign::RecordCommander(c, run->state.GetIntel());
    };

    // ---------- battle staging ----------
    std::unique_ptr<Game::BattleSession> battle;
    std::unique_ptr<Gameplay::BattleAI> foe;
    const Gameplay::BattleMap* battleMap = nullptr;
    std::optional<Game::MapProjection> battleProj;
    Game::BattleIntent intent;
    std::string cmdError;
    Campaign::EncounterAssembly asmb;
    const Campaign::EncounterDef* pendingEnc = nullptr;
    std::string report;                       // 13.7 chronicle
    Campaign::EncounterSettlement lastSettle;
    bool settleReady = false, haveSettle = false;
    auto endBattle = [&] {
        battle.reset();
        foe.reset();
        battleMap = nullptr;
        battleProj.reset();
        intent.Clear();
        cmdError.clear();
        report.clear();
    };

    // ---------- creation form ----------
    int rosterSel = -1;
    char nameBuf[128] = "";
    char originBuf[128] = "";
    int priorSel = 0;
    long long seedIn = 0;
    std::string formError;
    bool confirmDeploy = false;
    bool drawerOpen = true;
    std::int64_t auctionBid = auctionLot.openingBid;

    bool running = true;
    Game::ActionNotice actionNotice;
    while (running && !shell.ShouldClose()) {
        shell.NewFrame();

        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            flow.SetPaused(!flow.Paused());
        }
        // S2 hotkeys (design §S2): Tab 開闔卷宗, Space 推進一日 —
        // overworld-scoped; the pause overlay and text input
        // swallow keys first.
        if (flow.Screen() == Game::ScreenId::Overworld && run &&
            !flow.Paused() && !ImGui::GetIO().WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
                drawerOpen = !drawerOpen;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
                run->state.GetWorld().ResolveBeats(map, 1);
            }
        }
        const float dtMs =
            ImGui::GetIO().DeltaTime * 1000.0f;
        Game::AdvanceActionNotice(
            actionNotice,
            dtMs > 0.0f ? static_cast<std::uint32_t>(dtMs) : 0);

        // ---------- per-screen ----------
        switch (flow.Screen()) {
        case Game::ScreenId::Title: {
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize({960, 600});
            ImGui::Begin("##title", nullptr,
                         ImGuiWindowFlags_NoDecoration);
            ImGui::SetCursorPos({410, 112});
            ImGui::TextDisabled("ChinaWarlords");
            ImGui::SetCursorPos({330, 140});
            ImGui::Text("民 國 史 詩");
            ImGui::SetCursorPos({360, 170});
            ImGui::TextDisabled("至聖者無戰 · 治平者無勝");

            const auto slots = Game::TitleSlots(saves);
            static int sel = -1;
            const auto lay = Game::BuildTitleLayout(
                {.w = 960, .h = 600,
                 .slots = std::span<const Game::TitleSlotInfo>(
                     slots),
                 .selectedSlot = sel});
            // slot rows — aux = slot index
            for (const auto& w : lay.widgets) {
                if (w.kind != Game::TitleCell::SlotRow) continue;
                const Game::TitleSlotInfo* s = nullptr;
                for (const auto& cand : slots) {
                    if (cand.slot == w.aux) s = &cand;
                }
                if (s == nullptr) continue;
                ImGui::SetCursorPos(
                    {static_cast<float>(w.x),
                     static_cast<float>(w.y)});
                if (ImGui::Selectable(
                        ("  " + s->label +
                         "##slot" + std::to_string(s->slot))
                            .c_str(),
                        sel == s->slot, 0,
                        {static_cast<float>(w.w), 20})) {
                    sel = s->slot;
                }
            }
            // menu items — aux = TitleAction ordinal
            int my = 400;
            static const char* kActions[] = {
                "繼續戰役", "新戰役", "設定", "離開"};
            for (const auto& w : lay.widgets) {
                if (w.kind != Game::TitleCell::MenuItem) continue;
                ImGui::SetCursorPos({360, static_cast<float>(my)});
                my += 40;
                if (!w.enabled) ImGui::BeginDisabled();
                const bool hit = ImGui::Button(
                    kActions[w.aux], {240, 30});
                if (!w.enabled) ImGui::EndDisabled();
                if (!hit) continue;
                switch (static_cast<Game::TitleAction>(w.aux)) {
                case Game::TitleAction::Continue: {
                    auto s = saves.Load(sel);
                    if (s.ok()) {
                        run = std::make_unique<Run>();
                        run->state = std::move(s.value);
                        if (run->state.GetAuction().LotId().empty()) {
                            run->state.GetAuction().Start(
                                auctionLot, run->state.GetWorld().Day());
                        }
                        flow.Request(Game::ScreenId::Overworld);
                    }
                    Game::ShowActionResult(actionNotice, s, "續玩");
                    break;
                }
                case Game::TitleAction::NewGame:
                    flow.Request(Game::ScreenId::Creation);
                    break;
                case Game::TitleAction::Settings:
                    draft = settings;
                    settingsOpen = true;
                    break;
                case Game::TitleAction::Quit:
                    running = false;
                    break;
                }
            }
            ImGui::End();
            break;
        }

        case Game::ScreenId::Creation: {
            ImGui::SetNextWindowPos({120, 60});
            ImGui::SetNextWindowSize({720, 480});
            ImGui::Begin("任命", nullptr,
                         ImGuiWindowFlags_NoCollapse);
            const auto roster = Game::RosterList(chars);
            ImGui::Text("名將譜");
            for (int i = 0;
                 i < static_cast<int>(roster.size()); ++i) {
                if (ImGui::Selectable(
                        roster[i].name.c_str(),
                        rosterSel == i)) {
                    rosterSel = i;
                }
            }
            if (rosterSel >= 0 && ImGui::Button("受命")) {
                const Campaign::Character* c =
                    chars.Find(roster[rosterSel].id);
                if (c != nullptr) {
                    newCampaign(*c);
                    flow.Request(Game::ScreenId::Overworld);
                }
            }
            ImGui::Separator();
            ImGui::Text("自立門戶");
            ImGui::InputText("名", nameBuf, sizeof(nameBuf));
            ImGui::InputText("籍", originBuf,
                             sizeof(originBuf));
            static const char* kPriors[] = {
                "aggressive", "defensive", "cunning"};
            ImGui::Combo("性向", &priorSel, kPriors, 3);
            ImGui::InputScalar("牌種",
                               ImGuiDataType_S64, &seedIn);

            Game::CreationForm form;
            form.name = nameBuf;
            form.origin = originBuf;
            form.prior =
                static_cast<Campaign::RivalPrior>(priorSel);
            form.deckSeed =
                static_cast<std::uint64_t>(seedIn);
            // field-level echo — ValidateForm underlines
            // before the gate even runs.
            for (const auto& fe :
                 Game::ValidateForm(form)) {
                ImGui::TextDisabled("! %s", fe.message.c_str());
            }
            // 起始牌組預覽
            ImGui::Text("起手牌組：");
            for (const auto& row :
                 Game::PreviewDeck(form, cards)) {
                if (!row.resolved) ImGui::BeginDisabled();
                ImGui::BulletText("%s", row.name.c_str());
                if (!row.resolved) ImGui::EndDisabled();
            }
            if (!formError.empty()) {
                ImGui::TextColored({1, .4f, .4f, 1}, "%s",
                                   formError.c_str());
            }
            if (ImGui::Button("建檔")) {
                auto c = Game::BuildCharacter(form);
                if (c.ok()) {
                    formError.clear();
                    newCampaign(c.value);
                    flow.Request(Game::ScreenId::Overworld);
                } else {
                    formError = c.reason;
                    (void)Game::FieldForReason(c.reason);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("回標題")) {
                flow.AbandonToTitle();
            }
            ImGui::End();
            break;
        }

        case Game::ScreenId::Overworld: {
            if (!run) {
                flow.AbandonToTitle();
                break;
            }
            Campaign::WorldState& ws = run->state.GetWorld();
            const auto bar = Game::BuildTopBar(
                map, ws, run->state.GetLedger());
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize({960, 28});
            ImGui::Begin("##topbar", nullptr,
                         ImGuiWindowFlags_NoDecoration);
            if (bar.marching) {
                ImGui::Text("Day %lld · %s → %s (Day %lld)"
                            "   物資 %lld  民心 %lld  秩序 %lld",
                            (long long)bar.day,
                            bar.nodeName.c_str(),
                            bar.marchDestName.c_str(),
                            (long long)bar.marchEta,
                            (long long)bar.materiel,
                            (long long)bar.popularSupport,
                            (long long)bar.order);
            } else {
                ImGui::Text("Day %lld · %s"
                            "   物資 %lld  民心 %lld  秩序 %lld",
                            (long long)bar.day,
                            bar.nodeName.c_str(),
                            (long long)bar.materiel,
                            (long long)bar.popularSupport,
                            (long long)bar.order);
            }
            ImGui::End();

            if (drawerOpen) {
            ImGui::SetNextWindowPos({700, 28});
            ImGui::SetNextWindowSize({260, 500});
            ImGui::Begin("卷宗");
            if (ImGui::BeginTabBar("##drawer")) {
                if (ImGui::BeginTabItem("行軍令")) {
                    static Game::MarchIntent march;
                    for (const auto& o : Game::MarchOptions(
                             map, ws, bar.materiel)) {
                        if (!o.affordable) ImGui::BeginDisabled();
                        if (ImGui::Selectable(
                                o.name.c_str(),
                                march.Pending() &&
                                    march.Option().nodeId ==
                                        o.nodeId)) {
                            march.Click(map, ws, bar.materiel,
                                        o.nodeId);
                        }
                        ImGui::SameLine();
                        ImGui::TextDisabled("%lldd %lldc",
                                            (long long)o.days,
                                            (long long)o.supply);
                        if (!o.affordable) ImGui::EndDisabled();
                    }
                    if (march.Pending()) {
                        ImGui::Separator();
                        ImGui::Text("令下 %s：%lld 日，耗糧 %lld",
                                    march.Option().name.c_str(),
                                    (long long)march.Option()
                                        .days,
                                    (long long)march.Option()
                                        .supply);
                        if (ImGui::Button("確認")) {
                            const auto result = march.Confirm(
                                ws, map, run->state.GetLedger());
                            Game::ShowActionResult(
                                actionNotice, result, "行軍令");
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("作罷")) {
                            march.Cancel();
                        }
                    }
                    if (ImGui::Button("歇息一日")) {
                        ws.ResolveBeats(map, 1);
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("遭遇")) {
                    for (const auto& e : Game::EncounterRows(
                             ws, map, encs)) {
                        ImGui::BulletText("%s @ %s · %s",
                                          e.id.c_str(),
                                          e.nodeName.c_str(),
                                          Game::EncTriggerLabel(
                                              e.trigger));
                        if (ImGui::SmallButton(
                                ("應戰##" + e.id).c_str())) {
                            const auto* ed =
                                encs.Find(e.id);
                            if (ed == nullptr) {
                                Game::ShowActionResult(
                                    actionNotice,
                                    Gameplay::Fail<bool>(
                                        "encounter",
                                        "遭遇已不在待應戰清單"),
                                    "應戰");
                            } else {
                                Campaign::PlayerDeploy p0{
                                    squads.Find("vanguard"),
                                    0, run->deck, "先鋒營"};
                                Campaign::PlayerDeploy p1{
                                    squads.Find("riders"),
                                    3, run->deck, "輕騎隊"};
                                const Campaign::PlayerDeploy
                                    pds[] = {p0, p1};
                                const Campaign::Character* cmdr =
                                    run->state.HasCharacter()
                                        ? &run->state.GetCharacter()
                                        : nullptr;
                                auto launch =
                                    Game::PrepareEncounterLaunch(
                                        *ed, map, battleMaps, squads,
                                        cards, pds, cmdr,
                                        ws.Day() + 7);
                                Game::ShowActionResult(
                                    actionNotice, launch, "應戰");
                                if (launch.ok()) {
                                    asmb = std::move(launch.value);
                                    pendingEnc = ed;
                                    const auto bm =
                                        battleMaps.find(asmb.mapId);
                                    battleMap = &bm->second;
                                    battle =
                                        std::make_unique<
                                            Game::BattleSession>(
                                            asmb.seed,
                                            *battleMap,
                                            cards,
                                            asmb.fog);
                                    battle->Deploy(asmb);
                                    // Symmetric opponent —
                                    // the encounter's prior,
                                    // canonical sim-time
                                    // cadence (2 Hz).
                                    const auto gp =
                                        static_cast<
                                            Gameplay::Prior>(
                                            static_cast<int>(
                                                ed->hasPrior
                                                    ? ed->prior
                                                    : Campaign::
                                                          RivalPrior::
                                                              Defensive));
                                    foe = std::make_unique<
                                        Gameplay::BattleAI>(
                                        asmb.seed, 1, gp,
                                        *battleMap, cards);
                                    battle->AttachOpponent(
                                        foe.get(), 10);
                                    // The encounter's own
                                    // planning — arrows,
                                    // Cunning's feints. The
                                    // objective is the player
                                    // warband's deploy region
                                    // (marshal output, host-
                                    // supplied — the AI never
                                    // derives it from truth).
                                    for (const auto& row :
                                         asmb.rows) {
                                        if (row.side == 0) {
                                            foe->PlanDeployed(
                                                battle->
                                                    Controller(),
                                                row.region);
                                            break;
                                        }
                                    }
                                    auto pr =
                                        Game::MapProjection::
                                            Build(
                                                *battleMap,
                                                960, 600,
                                                8);
                                    battleProj =
                                        pr.ok()
                                            ? std::optional{
                                                  std::move(
                                                      pr.value)}
                                            : std::nullopt;
                                    intent.Clear();
                                    cmdError.clear();
                                    confirmDeploy = false;
                                    flow.Request(
                                        Game::ScreenId::
                                            Battle);
                                }
                                }
                            }
                        }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("情報")) {
                    for (const auto& e :
                         Game::IntelRows(
                             run->state.GetIntel())) {
                        const char* v =
                            e.verdict ==
                                    Campaign::IntelVerdict::True
                                ? "真"
                                : e.verdict ==
                                          Campaign::IntelVerdict::
                                              False
                                      ? "偽"
                                      : "懸";
                        ImGui::BulletText("[%s] %s",
                                          v,
                                          e.claim.c_str());
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("名冊")) {
                    for (const auto& e :
                         Game::SquadRows(run->state)) {
                        if (e.dead) {
                            ImGui::TextDisabled(
                                "%s 歿 · 殤 %lld",
                                e.name.c_str(),
                                (long long)e.casualties);
                        } else {
                            ImGui::BulletText(
                                "%s · 歷戰 %lld · 傷亡 %lld",
                                e.name.c_str(),
                                (long long)e.veterancy,
                                (long long)e.casualties);
                        }
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("帳冊")) {
                    for (const auto& e : Game::LedgerRows(
                             run->state.GetLedger(), 12)) {
                        ImGui::BulletText(
                            "%s%s +%lld→%s -%lld→%s",
                            e.forged ? "偽 " : "",
                            e.suspect ? "疑 " : "",
                            (long long)e.creditAmt,
                            e.credit.c_str(),
                            (long long)e.debitAmt,
                            e.debit.c_str());
                        ImGui::SameLine();
                        ImGui::TextDisabled("%s",
                                            e.memo.c_str());
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("拍賣")) {
                    auto& auction = run->state.GetAuction();
                    const auto& ledger = run->state.GetLedger();
                    if (auction.Settled()) {
                        if (auction.ClearingPrice() > 0) {
                            ImGui::Text("已成交：%s · 成交價 %lld 物資",
                                        auctionLot.rewardSquad.c_str(),
                                        (long long)auction.ClearingPrice());
                        } else {
                            ImGui::Text("本次競標未得標，托管物資已退回。");
                        }
                    } else if (auction.Open()) {
                        ImGui::Text("%s", auctionLot.title.c_str());
                        ImGui::Text("截止：第 %lld 日 · 可用物資：%lld",
                                    (long long)auction.ClosesOnDay(),
                                    (long long)ledger.Balance(
                                        Campaign::Account::Materiel));
                        ImGui::Text("起標 %lld · 最低加價 %lld · 已托管 %lld",
                                    (long long)auctionLot.openingBid,
                                    (long long)auctionLot.increment,
                                    (long long)ledger.Balance(
                                        Campaign::Account::AuctionEscrow));
                        if (auction.PlayerMaxBid() > 0) {
                            ImGui::Text("你的最高出價：%lld",
                                        (long long)auction.PlayerMaxBid());
                        }
                        if (ws.Day() < auction.ClosesOnDay()) {
                            ImGui::InputScalar("最高出價", ImGuiDataType_S64,
                                               &auctionBid);
                            if (ImGui::Button("提交出價")) {
                                const auto bid = auction.Bid(
                                    run->state.GetLedger(), auctionLot,
                                    auctionBid, ws.Day());
                                if (!bid.ok()) {
                                    ImGui::SameLine();
                                    ImGui::TextColored(
                                        {1, .35f, .3f, 1}, "%s",
                                        bid.reason.c_str());
                                } else if (auctionBid <=
                                           std::numeric_limits<std::int64_t>::max() -
                                               auctionLot.increment) {
                                    auctionBid += auctionLot.increment;
                                }
                            }
                        } else {
                            if (ImGui::Button("結算拍賣")) {
                                const auto result = auction.Settle(
                                    run->state, auctionLot, ws.Day());
                                if (!result.ok()) {
                                    ImGui::SameLine();
                                    ImGui::TextColored({1, .35f, .3f, 1}, "%s",
                                                       result.reason.c_str());
                                }
                            }
                        }
                        if (ws.Day() < auction.ClosesOnDay()) {
                            ImGui::TextDisabled("期限前可調高出價；歇息或推進戰役日期以等待結標。");
                        }
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("本草")) {
                    for (const auto& e :
                         Game::CodexRows(
                             bencaoLib, run->state.GetCodex())) {
                        if (e.pending) {
                            ImGui::TextDisabled("%s（待補鈔）",
                                                e.name.c_str());
                        } else {
                            ImGui::BulletText("%s",
                                              e.name.c_str());
                        }
                    }
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            ImGui::End();
            }
            break;
        }

        case Game::ScreenId::Battle: {
            if (!battle) {
                flow.Request(Game::ScreenId::Overworld);
                break;
            }
            flow.NoteBattleBeat(battle->Beat());
            Gameplay::BattleController& bc =
                battle->Controller();
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize({960, 600});
            // Zero padding: window content coords ARE fb coords
            // — the map hit-test and the raster share a space.
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                {0, 0});
            ImGui::Begin("##battle", nullptr,
                         ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_NoMove);

            switch (battle->Beat()) {
            case Gameplay::BattleBeat::Planning: {
                ImGui::SetCursorPos({8, 8});
                ImGui::Text("規劃 — %s",
                            pendingEnc
                                ? pendingEnc->id.c_str()
                                : "");
                ImGui::SetCursorPos({8, 30});
                ImGui::TextDisabled(
                    "點小隊後連點相鄰格標箭；開戰不可逆。");
                // Own squads — click arms a path build.
                for (int i = 0;
                     i < static_cast<int>(bc.Squads().size());
                     ++i) {
                    const auto& sq = bc.Squads()[i];
                    if (sq.side != 0) continue;
                    ImGui::SetCursorPos(
                        {8, 60.0f + i * 24.0f});
                    if (ImGui::Selectable(
                            ("  " + sq.name + "##sq" +
                             std::to_string(i))
                                .c_str(),
                            intent.Squad() == i, 0,
                            {170, 20})) {
                        intent.SelectSquad(i);
                        if (sq.regionIndex !=
                            Gameplay::Squad::NO_REGION) {
                            intent.BeginPath(sq.regionIndex);
                        }
                    }
                }
                if (!intent.Path().empty()) {
                    ImGui::SetCursorPos({8, 460});
                    ImGui::TextDisabled(
                        "箭線 %d 格",
                        static_cast<int>(
                            intent.Path().size()) - 1);
                }
                if (intent.Path().size() >= 2 &&
                    intent.Squad() >= 0) {
                    ImGui::SetCursorPos({8, 484});
                    if (ImGui::Button("標箭", {80, 24})) {
                        bc.DrawArrow(0, intent.Squad(),
                                     intent.Path());
                        intent.Clear();
                    }
                }
                ImGui::SetCursorPos({760, 520});
                if (!confirmDeploy) {
                    if (ImGui::Button("開戰", {160, 32})) {
                        confirmDeploy = true;
                    }
                } else {
                    ImGui::Text("確認開戰？");
                    ImGui::SetCursorPos({760, 556});
                    if (ImGui::Button("是", {80, 32})) {
                        battle->RequestExecution();
                        intent.Clear();
                        confirmDeploy = false;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("否", {80, 32})) {
                        confirmDeploy = false;
                    }
                }
                break;
            }
            case Gameplay::BattleBeat::Execution: {
                if (!flow.Paused()) {
                    battle->PumpMs(
                        static_cast<std::int64_t>(dtMs));
                }
                ImGui::SetCursorPos({8, 6});
                ImGui::Text("交戰 %lld   CP %d/%d   探 %d",
                            (long long)bc.GetSim().TickCount(),
                            bc.CpPool(0), Gameplay::CP_CAP,
                            bc.ProbesLeft(0));
                // Own squad dock.
                for (int i = 0;
                     i < static_cast<int>(bc.Squads().size());
                     ++i) {
                    const auto& sq = bc.Squads()[i];
                    if (sq.side != 0 || !sq.IsEffective()) {
                        continue;
                    }
                    ImGui::SetCursorPos(
                        {8, 40.0f + i * 24.0f});
                    const bool hit = ImGui::Selectable(
                        ("  " + sq.name + "##sq" +
                         std::to_string(i))
                            .c_str(),
                        intent.Squad() == i, 0, {170, 20});
                    if (!hit) continue;
                    if (intent.Armed() &&
                        Game::TargetOf(intent.VerbKind()) ==
                            Game::VerbTarget::OwnSquad &&
                        intent.Squad() != i) {
                        intent.FeedOwnSquad(i);
                    } else {
                        intent.SelectSquad(i);
                    }
                }
                // Verb bar — 下令 is the commit, every verb
                // passes through the controller's own gate.
                static const Gameplay::InterventionKind
                    kVerbs[] = {
                        Gameplay::InterventionKind::Redirect,
                        Gameplay::InterventionKind::Override,
                        Gameplay::InterventionKind::Retreat,
                        Gameplay::InterventionKind::Probe,
                        Gameplay::InterventionKind::Entangle,
                        Gameplay::InterventionKind::Replan,
                        Gameplay::InterventionKind::Execute,
                        Gameplay::InterventionKind::
                            MythPacify,
                        Gameplay::InterventionKind::
                            MythPossession,
                        Gameplay::InterventionKind::
                            MythGhostArmy,
                        Gameplay::InterventionKind::Subvert};
                float vx = 8;
                for (const auto k : kVerbs) {
                    ImGui::SetCursorPos({vx, 552});
                    const bool armed =
                        intent.Verb() ==
                        static_cast<int>(k);
                    if (armed) {
                        ImGui::PushStyleColor(
                            ImGuiCol_Button,
                            {0.35f, 0.45f, 0.6f, 1});
                    }
                    char lbl[64];
                    std::snprintf(
                        lbl, sizeof(lbl), "%s(%d)",
                        Game::HudVerbName(k),
                        Gameplay::CostOf(k));
                    if (ImGui::Button(lbl, {76, 24})) {
                        intent.Arm(k);
                        if (intent.VerbKind() ==
                                Gameplay::InterventionKind::
                                    Replan &&
                            intent.Squad() >= 0) {
                            const auto& sq = bc.Squads()
                                [static_cast<std::size_t>(
                                    intent.Squad())];
                            if (sq.regionIndex !=
                                Gameplay::Squad::NO_REGION) {
                                intent.BeginPath(
                                    sq.regionIndex);
                            }
                        }
                    }
                    if (armed) ImGui::PopStyleColor();
                    vx += 82;
                }
                // Override's slot pick — the squad's own sheet.
                if (intent.Armed() &&
                    intent.VerbKind() ==
                        Gameplay::InterventionKind::Override &&
                    intent.Squad() >= 0) {
                    const auto& sh = bc.SheetOf(
                        static_cast<std::size_t>(
                            intent.Squad()));
                    for (int s = 0;
                         s < static_cast<int>(
                                 sh.slots.size());
                         ++s) {
                        const auto& slot = sh.slots[s];
                        const char* cid =
                            slot.cardIndex < cards.Count()
                                ? cards.At(slot.cardIndex)
                                      .id.c_str()
                                : "?";
                        ImGui::SetCursorPos(
                            {8.0f + s * 84.0f, 500});
                        char sl[64];
                        std::snprintf(sl, sizeof(sl),
                                      "%s##slot%d", cid, s);
                        if (ImGui::SmallButton(sl)) {
                            intent.FeedSlot(s);
                        }
                    }
                }
                // Commit strip — 確認/作罷 for a Ready intent.
                if (intent.Ready()) {
                    ImGui::SetCursorPos({360, 520});
                    ImGui::Text("下令 %s？",
                                Game::HudVerbName(
                                    intent.VerbKind()));
                    ImGui::SetCursorPos({460, 516});
                    if (ImGui::Button("確認", {70, 26})) {
                        auto r = intent.Fire(bc, 0);
                        cmdError = r.ok() ? "" : r.reason;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("作罷", {70, 26})) {
                        intent.Clear();
                        cmdError.clear();
                    }
                } else if (intent.Armed()) {
                    ImGui::SetCursorPos({360, 520});
                    ImGui::TextDisabled("點選目標…");
                }
                if (!cmdError.empty()) {
                    ImGui::SetCursorPos({190, 6});
                    ImGui::TextColored({1, .4f, .4f, 1},
                                       "%s",
                                       cmdError.c_str());
                }
                break;
            }
            case Gameplay::BattleBeat::Aftermath: {
                if (report.empty()) {
                    report = Campaign::RenderBattleReport(
                        battle->Events(), 0);
                }
                // The frame handed to BuildReportPage is the
                // page's usable bounds (ReportView's sheet
                // minus margins), so the 結算 commit lands
                // on the paper, not the desk.
                const auto page = Game::BuildReportPage(
                    report, true, 850, 580);
                // Aligned to the rendered sheet's text inset —
                // the ruled lines sit under the glyphs.
                ImGui::SetCursorPos({170, 54});
                ImGui::BeginChild("##report", {640, 450});
                ImGui::TextWrapped("%s", page.text.c_str());
                ImGui::EndChild();
                ImGui::SetCursorPos(
                    {static_cast<float>(page.settleX),
                     static_cast<float>(page.settleY)});
                if (ImGui::Button("結算 →",
                                  {static_cast<float>(
                                       page.settleW),
                                   static_cast<float>(
                                       page.settleH)})) {
                    Campaign::WorldState& ws =
                        run->state.GetWorld();
                    const bool won =
                        battle->Outcome().winnerSide == 0;
                    const auto cas = Game::CasualtyReport(
                        battle->Controller(), asmb, 0);
                    auto s = Campaign::SettleEncounter(
                        run->state, ws, map, *pendingEnc, won,
                        0, battle->Events(), cas,
                        static_cast<std::uint64_t>(
                            battle->Outcome().elapsedTicks + 1),
                        &run->state.GetMyth(),
                        battle->Outcome().subverted);
                    if (Game::PresentSettlementResult(
                            flow, actionNotice, s)) {
                        lastSettle = s.value;
                        haveSettle = true;
                        // 補鈔 — the codex hears the fold.
                        Campaign::CodexSignals sig;
                        sig.ledger =
                            &run->state.GetLedger();
                        sig.mythLog = &run->state.GetMyth();
                        sig.chapterId = pendingEnc->id;
                        Campaign::ResolveBencaoUnlocks(
                            bencaoLib, sig,
                            run->state.GetCodex());
                        run->state.GetCodex().TakePending(8);
                        settleReady = true;
                    }
                }
                break;
            }
            }
            // Field hit-test — submitted LAST so HUD widgets
            // claim their own clicks; only bare map clicks
            // reach the projection. Window padding is zero, so
            // mouse position IS the fb pixel.
            if (battle->Beat() !=
                    Gameplay::BattleBeat::Aftermath &&
                battleProj) {
                ImGui::SetCursorPos({190, 50});
                ImGui::InvisibleButton("##field", {770, 480});
                if (ImGui::IsItemClicked()) {
                    const ImVec2 mp = ImGui::GetIO().MousePos;
                    const int rgn = battleProj->Hit(
                        static_cast<int>(mp.x),
                        static_cast<int>(mp.y), 40);
                    if (rgn >= 0) {
                        const std::size_t region =
                            static_cast<std::size_t>(rgn);
                        if (battle->Beat() ==
                            Gameplay::BattleBeat::Planning) {
                            intent.ExtendPath(*battleMap,
                                              region);
                        } else if (intent.Armed()) {
                            switch (Game::TargetOf(
                                intent.VerbKind())) {
                            case Game::VerbTarget::Region:
                                intent.FeedRegion(region);
                                break;
                            case Game::VerbTarget::Path:
                                intent.ExtendPath(
                                    *battleMap, region);
                                break;
                            case Game::VerbTarget::EnemyCloud:
                            case Game::VerbTarget::
                                CloudPair: {
                                const int cloud =
                                    Game::CloudAtRegion(
                                        bc.Fog(0), region);
                                if (cloud >= 0) {
                                    intent.FeedCloud(bc, 0,
                                                     cloud);
                                }
                                break;
                            }
                            default:
                                break;
                            }
                        }
                    }
                }
            }
            ImGui::End();
            ImGui::PopStyleVar();
            break;
        }

        case Game::ScreenId::Settlement: {
            ImGui::SetNextWindowPos({280, 220});
            ImGui::SetNextWindowSize({400, 160});
            ImGui::Begin("##settle", nullptr,
                         ImGuiWindowFlags_NoDecoration);
            if (haveSettle) {
                const auto strip =
                    Game::BuildSettlementStrip(lastSettle);
                ImGui::Text("裁決：%s", strip.verdict.c_str());
                for (const auto& r : strip.rows) {
                    ImGui::BulletText("%s：%s",
                                      r.label.c_str(),
                                      r.value.c_str());
                }
            } else {
                ImGui::Text("裁決：未定");
            }
            ImGui::Separator();
            // The campaign's arc ends when the world holds no
            // more armed questions — every encounter resolved
            // means the book closes.
            const bool arcDone =
                run && Game::EncounterRows(
                           run->state.GetWorld(), map, encs)
                           .empty() &&
                encs.Size() > 0;
            if (arcDone) {
                if (ImGui::Button("蓋棺 →")) {
                    flow.Request(Game::ScreenId::Ending);
                }
            } else {
                if (ImGui::Button("返回大地圖")) {
                    flow.Request(Game::ScreenId::Overworld);
                }
            }
            endBattle();
            ImGui::End();
            break;
        }

        case Game::ScreenId::Ending: {
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize({960, 600});
            ImGui::Begin("##ending", nullptr,
                         ImGuiWindowFlags_NoDecoration);
            if (run) {
                const auto page = Game::BuildEndingPage(
                    run->state.GetLedger(), 960, 600);
                ImGui::SetCursorPos({430, 60});
                ImGui::Text("%s", page.title.c_str());
                ImGui::SetCursorPos({120, 110});
                ImGui::BeginChild("##folio", {720, 400});
                ImGui::TextWrapped("%s", page.text.c_str());
                ImGui::EndChild();
                ImGui::SetCursorPos(
                    {static_cast<float>(page.returnX),
                     static_cast<float>(page.returnY)});
                if (ImGui::Button("回標題",
                                  {static_cast<float>(
                                       page.returnW),
                                   static_cast<float>(
                                       page.returnH)})) {
                    run.reset();
                    flow.AbandonToTitle();
                }
            }
            ImGui::End();
            break;
        }
        }

        // ---------- Esc overlay ----------
        if (flow.Paused()) {
            const auto slots = Game::TitleSlots(saves);
            Potato::Game::PauseInput pin;
            pin.w = 960;
            pin.h = 600;
            pin.canSave = flow.CanSave();
            pin.slots = slots;
            const auto lay = Game::BuildPauseLayout(pin);
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize({960, 600});
            ImGui::Begin("##pause", nullptr,
                         ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_NoMove);
            static const char* kLabels[] = {
                "繼續", "存檔", "", "設定", "回標題", "離開"};
            for (const auto& w : lay.widgets) {
                ImGui::SetCursorPos(
                    {static_cast<float>(w.x),
                     static_cast<float>(w.y)});
                if (!w.enabled) ImGui::BeginDisabled();
                bool hit = false;
                std::string label;
                switch (w.action) {
                case Game::PauseAction::LoadSlot:
                    for (const auto& s : slots) {
                        if (s.slot == w.aux) {
                            label = "讀檔 " + s.label;
                        }
                    }
                    hit = ImGui::Button(
                        label.c_str(), {320, 30});
                    break;
                default:
                    hit = ImGui::Button(
                        kLabels[static_cast<int>(w.action)],
                        {320, 30});
                    break;
                }
                if (!w.enabled) ImGui::EndDisabled();
                if (!hit) continue;
                switch (w.action) {
                case Game::PauseAction::Resume:
                    flow.SetPaused(false);
                    break;
                case Game::PauseAction::Save:
                    if (run) {
                        const auto saved = saves.Save(0, run->state);
                        Game::ShowActionResult(actionNotice, saved,
                                               "存檔");
                    }
                    break;
                case Game::PauseAction::LoadSlot: {
                    auto s = saves.Load(w.aux);
                    if (s.ok()) {
                        run = std::make_unique<Run>();
                        run->state = std::move(s.value);
                        if (run->state.GetAuction().LotId().empty()) {
                            run->state.GetAuction().Start(
                                auctionLot, run->state.GetWorld().Day());
                        }
                        endBattle();
                        flow.AbandonToTitle();
                        flow.Request(
                            Game::ScreenId::Overworld);
                    }
                    Game::ShowActionResult(actionNotice, s, "讀檔");
                    break;
                }
                case Game::PauseAction::Settings:
                    draft = settings;
                    settingsOpen = true;
                    break;
                case Game::PauseAction::QuitToTitle:
                    endBattle();
                    flow.AbandonToTitle();
                    break;
                case Game::PauseAction::Quit:
                    running = false;
                    break;
                }
            }
            ImGui::End();
        }

        // ---------- settings modal ----------
        // Floats over everything (title or pause overlay): the
        // draft is the working copy — 儲存 commits+writes+
        // applies, 返回 discards.
        if (settingsOpen) {
            ImGui::SetNextWindowPos({300, 120});
            ImGui::SetNextWindowSize({360, 280});
            ImGui::Begin("設定", &settingsOpen);
            ImGui::Text("音量（未接音訊）");
            ImGui::SameLine(170);
            if (ImGui::Button("-##vol"))
                Game::StepVolume(draft, -1);
            ImGui::SameLine();
            ImGui::Text("%d", draft.volume);
            ImGui::SameLine();
            if (ImGui::Button("+##vol"))
                Game::StepVolume(draft, 1);
            ImGui::Text("解析度");
            ImGui::SameLine(170);
            if (ImGui::Button("-##res"))
                Game::CycleResolution(draft, -1);
            ImGui::SameLine();
            ImGui::Text("%dx%d", draft.Res().w,
                        draft.Res().h);
            ImGui::SameLine();
            if (ImGui::Button("+##res"))
                Game::CycleResolution(draft, 1);
            ImGui::Text("介面密度");
            ImGui::SameLine(170);
            if (ImGui::Button("-##den"))
                Game::CycleDensity(draft, -1);
            ImGui::SameLine();
            ImGui::Text("%s",
                        Game::HudDensityLabel(draft.density));
            ImGui::SameLine();
            if (ImGui::Button("+##den"))
                Game::CycleDensity(draft, 1);
            ImGui::Separator();
            if (ImGui::Button("儲存")) {
                const auto saved = draft.Save("settings.json");
                Game::ShowActionResult(actionNotice, saved,
                                       "設定儲存");
                if (saved.ok()) {
                    settings = draft;
                    applySettings();
                    settingsOpen = false;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("返回")) {
                settingsOpen = false;
            }
            ImGui::End();
        }

        // ---------- backdrop ----------
        if (flow.Screen() == Game::ScreenId::Battle && battle &&
            battle->Beat() == Gameplay::BattleBeat::Aftermath) {
            // The chronicle's page — deterministic chrome under
            // the ImGui text layer (design §4 stage one).
            Game::ReportInput rin;
            rin.seal = asmb.seed;
            auto fb = Game::RenderReportPage(rin);
            if (fb.ok()) shell.Present(fb.value);
        } else if (flow.Screen() == Game::ScreenId::Battle &&
                   battle && battleMap != nullptr) {
            // Truth boundary, drawn: own squads paint directly;
            // the enemy is only ever the observer's fog.
            std::vector<Gameplay::Squad> own;
            for (const auto& sq :
                 battle->Controller().Squads()) {
                if (sq.side == 0) own.push_back(sq);
            }
            Game::BattleViewInput vin;
            vin.w = 960;
            vin.h = 600;
            vin.fog = &battle->Controller().Fog(0);
            vin.ownSquads = own;
            auto fb = Game::RenderBattleView(*battleMap, atlas,
                                             vin);
            if (fb.ok()) shell.Present(fb.value);
        } else {
            Game::OverworldInput viewIn;
            viewIn.encounters = &encs;
            const Campaign::WorldState& ws =
                run && run->state.HasWorld()
                    ? run->state.GetWorld()
                    : idleWs;
            auto fb =
                Game::RenderOverworld(map, ws, atlas, viewIn);
            if (fb.ok()) shell.Present(fb.value);
        }
        if (actionNotice.visible) {
            ImGui::SetNextWindowPos({260, 36});
            ImGui::SetNextWindowSize({440, 0});
            ImGui::Begin("##action-notice", nullptr,
                         ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_AlwaysAutoResize);
            const ImVec4 color = actionNotice.success
                ? ImVec4{0.55f, 0.9f, 0.6f, 1.0f}
                : ImVec4{1.0f, 0.45f, 0.4f, 1.0f};
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            ImGui::TextWrapped("%s", actionNotice.text.c_str());
            ImGui::PopStyleColor();
            ImGui::End();
        }
        shell.Swap();
    }

    shell.Shutdown();
    return 0;
}
