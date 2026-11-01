#pragma once

#include <string>

namespace Potato::Campaign {

class BencaoCodex;
class BencaoLibrary;
class BuchaoStore;

// Story 10.4 — the codex renders as a book, not a menu. Headless
// text layout on the HistorianReport precedent; page chrome
// (冊頁 art, palette) is Epic F shell work.
//
//   frontispiece — the 序頁 disclaimer and historical source note,
//                  always precede browsing; the in-game eight
//                  categories are identified as an adaptation of
//                  Li Shizhen's sixteen-part source organization
//   sections     — all 8 部類 headers in canonical order, even
//                  sections holding only sealed slots
//   unlocked     — full 本草體 page: 釋名/集解/性味歸經/主治/
//                  批註/出處; optional fields omit, not print bare
//   pending      — 【諱】補鈔在途: the Scribe knows it is coming —
//                  name withheld, no factual fields
//   locked       — 【諱】未錄: sealed slot, no factual fields
//
// The 諱 title never leaks what a sealed page says — collection
// slots show that a page exists, never its contents.
// Pure fold over lib.Entries() order — deterministic, no PRNG,
// no clock, mutates nothing, does no I/O.
std::string RenderCodex(const BencaoLibrary& lib,
                        const BencaoCodex& codex,
                        const BuchaoStore* buchao = nullptr);

} // namespace Potato::Campaign
