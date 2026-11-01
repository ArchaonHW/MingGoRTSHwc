#include "Campaign/Narrative/Codex.h"

#include "Campaign/Narrative/Bencao.h"
#include "Campaign/Narrative/BencaoCodex.h"
#include "Campaign/Narrative/Buchao.h"

namespace Potato::Campaign {

namespace {

bool IsPending(const BencaoCodex& codex, std::string_view id) {
    for (const PendingPage& p : codex.Pending()) {
        if (p.id == id) return true;
    }
    return false;
}

void RenderEntry(std::string& out, const BencaoEntry& e,
                 const BuchaoStore* buchao) {
    out += "【";
    out += e.name;
    out += "】\n";
    if (!e.aliases.empty()) {
        out += "釋名：";
        for (std::size_t i = 0; i < e.aliases.size(); ++i) {
            if (i) out += "、";
            out += e.aliases[i];
        }
        out += "。\n";
    }
    if (!e.origin.empty()) {
        out += "集解：";
        out += e.origin;
        out += "\n";
    }
    out += "性味歸經：";
    out += e.nature;
    out += "\n主治：";
    out += e.indications;
    out += "\n";
    if (buchao) {
        if (const BuchaoPage* page = buchao->Find(e.id)) {
            out += "批註：";
            out += page->note;
            if (page->suspect) {
                // Judgment overlay — same surface as ledger
                // suspect flags: flagged, never hidden.
                out += "（批校：書吏疑其不實。）";
            }
            out += "\n";
        }
    }
    out += "出處：";
    out += e.source;
    out += "\n";
}

} // namespace

std::string RenderCodex(const BencaoLibrary& lib,
                        const BencaoCodex& codex,
                        const BuchaoStore* buchao) {
    std::string out = "【本草拾遺】\n\n";
    // 序頁 — worldview §4 verbatim; always precedes browsing.
    out += "序：是冊所載，皆前人之驗、草木之性，錄以備考，"
           "非為用藥之據。\n"
           "　　病家慎勿執紙上之言以試人身。\n"
           "文獻：明代藥學家李時珍（1518–1593）撰《本草綱目》，"
           "全書五十二卷、十六部，萬曆二十四年（1596）於南京刊行。\n"
           "編目：本冊依遊戲需要歸為八類，並非原書部目之轉錄。\n";

    std::size_t shown = 0;
    // The book's TOC is stable: all 8 部類 headers always emit,
    // even a section whose every page is sealed (or absent).
    for (std::size_t c = 0; c < kBencaoCategoryCount; ++c) {
        const auto cat = static_cast<BencaoCategory>(c);
        out += "\n——";
        out += BencaoCategoryTitle(cat);
        out += "——\n";
        for (const BencaoEntry& e : lib.Entries()) {
            if (e.category != cat) continue;
            if (codex.IsUnlocked(e.id)) {
                RenderEntry(out, e, buchao);
                ++shown;
            } else if (IsPending(codex, e.id)) {
                out += "【諱】補鈔在途\n";
            } else {
                out += "【諱】未錄\n";
            }
        }
    }

    out += "\n凡 ";
    out += std::to_string(lib.Size());
    out += " 種，已錄 ";
    out += std::to_string(shown);
    out += " 種。\nHistorical text reproduced for the fiction; "
           "not medical advice.\n";
    return out;
}

} // namespace Potato::Campaign
