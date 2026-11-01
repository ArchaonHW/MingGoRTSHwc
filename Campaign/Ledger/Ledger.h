#pragma once

#include "Gameplay/Json/JsonValue.h"
#include "Gameplay/Result.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Potato::Campaign {

// Spendable accounts. 秩序/墮落 are deliberately absent —
// they are derived accumulators folded from entry tags (Epic 4),
// never booked directly.
enum class Account : std::uint8_t {
    MartialMerit = 0,   // 武功
    PopularSupport = 1, // 民心
    Mandate = 2,        // 天命
    ArmyPrestige = 3,   // 軍威
    Materiel = 4,       // 物資
    AuctionEscrow = 5,  // 拍賣托管（不計入可用物資）
    AuctionTreasury = 6, // 拍賣成交款（NPC 市集）
};
constexpr int kAccountCount = 7;

// Stable ASCII wire ids (CJK labels are presentation-layer, Epic 8).
const char* AccountName(Account a);
bool AccountFromName(std::string_view name, Account& out);

// One leg of a double-entry posting. `amount > 0` always: the credit
// leg adds it to `account`; the debit leg subtracts it.
struct Leg {
    Account account = Account::Materiel;
    std::int64_t amount = 0;
};

// The write input — one event, two accounts.
struct Posting {
    Leg credit;
    Leg debit;
    std::string memo;              // what happened ("burned village r3")
    std::vector<std::string> tags; // fold keys ("atrocity", region ids)
};

// Which hand wrote the entry (Story 2.3). Honest = Post; Forged =
// the ForgeryChannel inject path. Provenance is hash-covered
// content: the mark can't be stripped without a recompute-class
// rewrite — the same boundary as every other mutation.
enum class Provenance : std::uint8_t {
    Honest = 0,
    Forged = 1,
};
const char* ProvenanceName(Provenance p);
bool ProvenanceFromName(std::string_view name, Provenance& out);

// The append-only unit: a validated Posting plus its position, hash-
// chained to its predecessor (Story 2.2). prevHash of entry 0 is
// kGenesisHash; hash covers all content fields + prevHash, so a
// mutated entry breaks its own stored hash and the next link.
// `suspect` sits OUTSIDE the hashed content: flagging is post-hoc
// judgment — a set flag must not break the chain it judges. Accepted
// surface: a file tamperer can flip flags freely (judgment metadata,
// not a claim).
struct LedgerEntry {
    std::uint64_t seq = 0; // stable append-order key (0-based)
    Leg credit;
    Leg debit;
    std::string memo;
    std::vector<std::string> tags;
    Provenance provenance = Provenance::Honest;
    bool suspect = false; // judgment overlay — not hashed, persisted
    std::uint64_t prevHash = 0;
    std::uint64_t hash = 0;
};

// The campaign's state substrate. Append-only; balances are folds,
// never stored — an account total can only change by posting.
// The hash chain (Story 2.2) and the forgery channel (Story 2.3) are
// deliberately separate mechanisms layered on this type.
class Ledger {
public:
    // /3 adds provenance (hash-covered) + suspect (persisted, not
    // hashed). /2 docs are rejected — a /2 loader has no provenance
    // semantics and would silently read forged entries as honest.
    // Wire note: hash-family values are uint64 bit-cast to int64
    // (bit-exact round-trip; ~half emit negative). External
    // producers writing unsigned literals >=2^63 parse as Real and
    // get rejected — producers must bit-cast too.
    static constexpr std::string_view SCHEMA = "potato.ledger/3";
    static constexpr std::uint64_t kGenesisHash =
        14695981039346656037ull; // FNV-1a offset basis
    static constexpr std::size_t MAX_ENTRIES = 1000000;
    static constexpr std::size_t MAX_TAGS = 16;
    static constexpr std::size_t MAX_TAG_LEN = 64;
    // Canonical fold-key tags — byte-exact matching is the contract
    // (compare CrossCheck's record_root: discipline). Producers and
    // folders share these spellings; anything else is just a label.
    static constexpr std::string_view TAG_ATROCITY = "atrocity";
    // Myth-layer fold key (Story 5.3): every 天命 book-keeping entry —
    // dedication credit or myth-action spend — carries "myth" so the
    // fold can total the gods' economy separately.
    static constexpr std::string_view TAG_MYTH = "myth";
    // Chapter-resolution seal (Story 4.4): a chapter-end posting
    // carries `resolution:<kind>` + `chapter:<idx>` so the historian
    // folds the verdict out of the same truth as everything else.
    static constexpr std::string_view TAG_RESOLUTION = "resolution:";
    static constexpr std::string_view TAG_CHAPTER = "chapter:";
    // Place tags (Story 12.7): `region:<id>` is RESERVED for
    // world-node ids — the one namespace per-region governance
    // folds read. `field:<n>` carries a battle-map-local region
    // index (the sim's own region space). The two never mix: a
    // world node named "2" is still a world node.
    static constexpr std::string_view TAG_REGION = "region:";
    static constexpr std::string_view TAG_FIELD = "field:";
    static constexpr std::size_t MAX_MEMO_LEN = 256;
    // Per-leg amount cap. With MAX_ENTRIES worst-case entries the
    // Balance fold stays ~1e18 — far inside int64.
    static constexpr std::int64_t MAX_AMOUNT = 1000000000000LL;

    // The honest write path. A posting is balanced iff both amounts
    // are > 0 and the two legs name different accounts. Magnitudes
    // need NOT match — asymmetric costs are the design: burning a
    // village books +40 物資 / −15 民心. The pair is atomic: a
    // rejected posting leaves the chain untouched.
    Gameplay::Result<std::uint64_t> Post(Posting p);

    // The ForgeryChannel (Story 2.3): an enemy-injected entry written
    // THROUGH the chain — same validation, same linking — but marked
    // provenance=Forged. Suspicion is data, not corruption: forged
    // entries verify clean and fold into balances.
    Gameplay::Result<std::uint64_t> Forge(Posting p);

    // Judgment overlay: set/clear the suspect flag on entry `seq`
    // (seq == index, chains are contiguous). Returns false when seq
    // is out of range. Flags live outside the hashed content — a
    // flag write never breaks the chain it judges.
    bool SetSuspect(std::uint64_t seq, bool suspect = true);

    // seqs of flagged entries — the report layer's surface (2.4).
    std::vector<std::uint64_t> SuspectEntries() const;

    // Pure fold over entries — +credit where credit.account==a,
    // −debit where debit.account==a. Negative balances are legal
    // (a 民心 deficit is a meaningful ledger state).
    std::int64_t Balance(Account a) const;

    // Append-only view — refs/iterators invalidate on the next Post
    // (std::vector semantics); index by seq instead of caching refs.
    const std::vector<LedgerEntry>& Entries() const { return entries_; }
    std::size_t Size() const { return entries_.size(); }

    // The chain tip commits to the entire history — an empty chain's
    // tip is the genesis hash.
    std::uint64_t Tip() const {
        return entries_.empty() ? kGenesisHash : entries_.back().hash;
    }

    // A seal commits to BOTH the entry count and the chain tip, so
    // truncating the tail requires recomputing it — the same effort
    // class as a wholesale rehash (everything is unkeyed: a determined
    // attacker CAN recompute; external anchoring is Story 2.5's job).
    static std::uint64_t SealHash(std::uint64_t count,
                                  std::uint64_t tip);
    std::uint64_t Seal() const {
        return SealHash(entries_.size(), Tip());
    }

    // Replays the chain: seq contiguity, prevHash linkage, stored-hash
    // recomputation, and the same leg/meta invariants the write path
    // enforces (a writer that bypasses Post still can't produce a
    // chain Verify accepts). Returns nullptr when clean, else a
    // static reason string for the first break.
    const char* Verify() const;

    // FNV-1a over prevHash (LE bytes) || canonical emit of the
    // content object {seq, credit, debit, memo, tags} — same hashing
    // convention as the battle record's integrity root.
    static std::uint64_t EntryHash(const LedgerEntry& e);

    Gameplay::Result<Gameplay::JsonValue> ToJson() const;
    static Gameplay::Result<Ledger> FromJson(const Gameplay::JsonValue& doc);

private:
    // Shared append path behind Post (Honest) and Forge (Forged).
    Gameplay::Result<std::uint64_t> Append(Posting p,
                                           Provenance provenance);

    std::vector<LedgerEntry> entries_;
};

// Shared invariant used by both Post() and FromJson() — a persisted
// unbalanced entry cannot smuggle past a load.
const char* ValidateLegs(const Leg& credit, const Leg& debit);

} // namespace Potato::Campaign
