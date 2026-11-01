#include "Campaign/Ledger/Ledger.h"

#include <algorithm>
#include <utility>

namespace Potato::Campaign {
namespace {

using Gameplay::JsonValue;

constexpr std::string_view kNames[kAccountCount] = {
    "martial_merit", "popular_support", "mandate",
    "army_prestige", "materiel", "auction_escrow", "auction_treasury",
};

JsonValue LegToJson(const Leg& l) {
    JsonValue::Object o;
    o.emplace("account", JsonValue::String(AccountName(l.account)));
    o.emplace("amount", JsonValue::Int(l.amount));
    return JsonValue::MakeObject(std::move(o));
}

// The content object the entry hash commits to — the SAME fields the
// wire format emits (sans hash/prevHash), in canonical Emit order.
JsonValue EntryContentJson(const LedgerEntry& e) {
    JsonValue::Array tags;
    tags.reserve(e.tags.size());
    for (const std::string& t : e.tags) {
        tags.push_back(JsonValue::String(t));
    }
    JsonValue::Object o;
    o.emplace("seq", JsonValue::Int(static_cast<std::int64_t>(e.seq)));
    o.emplace("credit", LegToJson(e.credit));
    o.emplace("debit", LegToJson(e.debit));
    o.emplace("memo", JsonValue::String(e.memo));
    o.emplace("tags", JsonValue::MakeArray(std::move(tags)));
    // Provenance is hash-covered: the "forged" mark can't be
    // stripped without a recompute-class rewrite. `suspect` is NOT
    // here — flagging is post-hoc judgment, not entry content.
    o.emplace("provenance",
              JsonValue::String(ProvenanceName(e.provenance)));
    return JsonValue::MakeObject(std::move(o));
}

// FNV-1a — same constants as the battle record integrity root.
std::uint64_t Fnv1a(const std::string& bytes, std::uint64_t h) {
    for (unsigned char c : bytes) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

// Strict leg parse: {"account": <known name>, "amount": <int>}.
bool LegFromJson(const JsonValue& j, Leg& out) {
    if (!j.IsObject() || !j["amount"].IsInt()) return false;
    const std::string* name = j.FindString("account");
    if (name == nullptr) return false;
    Account a;
    if (!AccountFromName(*name, a)) return false;
    out = Leg{a, j["amount"].AsInt()};
    return true;
}

// Shared memo/tag invariant used by Post and FromJson — the file is
// untrusted; a load must not admit state the write path forbids.
// Fold keys are set semantics: a duplicate tag would double-count in
// any fold that counts occurrences.
const char* ValidateMeta(const std::string& memo,
                         const std::vector<std::string>& tags) {
    if (memo.size() > Ledger::MAX_MEMO_LEN) return "memo too long";
    if (tags.size() > Ledger::MAX_TAGS) return "too many tags";
    for (const std::string& t : tags) {
        if (t.empty() || t.size() > Ledger::MAX_TAG_LEN) {
            return "bad tag length";
        }
    }
    std::vector<std::string> sorted = tags;
    std::sort(sorted.begin(), sorted.end());
    if (std::adjacent_find(sorted.begin(), sorted.end()) !=
        sorted.end()) {
        return "duplicate tag";
    }
    return nullptr;
}

} // namespace

const char* AccountName(Account a) {
    const int i = static_cast<int>(a);
    return (i >= 0 && i < kAccountCount) ? kNames[i].data() : "unknown";
}

bool AccountFromName(std::string_view name, Account& out) {
    for (int i = 0; i < kAccountCount; ++i) {
        if (name == kNames[i]) {
            out = static_cast<Account>(i);
            return true;
        }
    }
    return false;
}

const char* ProvenanceName(Provenance p) {
    switch (p) {
    case Provenance::Honest:
        return "honest";
    case Provenance::Forged:
        return "forged";
    }
    return "unknown";
}

bool ProvenanceFromName(std::string_view name, Provenance& out) {
    if (name == "honest") {
        out = Provenance::Honest;
        return true;
    }
    if (name == "forged") {
        out = Provenance::Forged;
        return true;
    }
    return false;
}

const char* ValidateLegs(const Leg& credit, const Leg& debit) {
    const auto known = [](const Leg& l) {
        return static_cast<int>(l.account) < kAccountCount;
    };
    if (!known(credit) || !known(debit)) {
        return "leg names an unknown account";
    }
    if (credit.amount <= 0) return "credit amount must be > 0";
    if (debit.amount <= 0) return "debit amount must be > 0";
    if (credit.amount > Ledger::MAX_AMOUNT ||
        debit.amount > Ledger::MAX_AMOUNT) {
        return "amount exceeds MAX_AMOUNT";
    }
    if (credit.account == debit.account) {
        return "credit and debit must be different accounts";
    }
    return nullptr;
}

Gameplay::Result<std::uint64_t> Ledger::Post(Posting p) {
    return Append(std::move(p), Provenance::Honest);
}

Gameplay::Result<std::uint64_t> Ledger::Forge(Posting p) {
    return Append(std::move(p), Provenance::Forged);
}

Gameplay::Result<std::uint64_t> Ledger::Append(Posting p,
                                               Provenance provenance) {
    if (const char* why = ValidateLegs(p.credit, p.debit)) {
        return Gameplay::Fail<std::uint64_t>("unbalanced", why);
    }
    if (entries_.size() >= MAX_ENTRIES) {
        return Gameplay::Fail<std::uint64_t>("posting",
                                             "ledger at MAX_ENTRIES");
    }
    if (const char* why = ValidateMeta(p.memo, p.tags)) {
        return Gameplay::Fail<std::uint64_t>("posting", why);
    }
    // Validate-then-append: the pair lands atomically or not at all.
    // Forged entries take the same path — a forgery pretends to be
    // valid bookkeeping; its mark lives in provenance, not structure.
    LedgerEntry e;
    e.seq = static_cast<std::uint64_t>(entries_.size());
    e.credit = p.credit;
    e.debit = p.debit;
    e.memo = std::move(p.memo);
    e.tags = std::move(p.tags);
    e.provenance = provenance;
    e.prevHash = Tip();
    e.hash = EntryHash(e);
    entries_.push_back(std::move(e));
    return Gameplay::Ok(entries_.back().seq);
}

bool Ledger::SetSuspect(std::uint64_t seq, bool suspect) {
    if (seq >= entries_.size()) return false;
    entries_[static_cast<std::size_t>(seq)].suspect = suspect;
    return true;
}

std::vector<std::uint64_t> Ledger::SuspectEntries() const {
    std::vector<std::uint64_t> out;
    for (const LedgerEntry& e : entries_) {
        if (e.suspect) out.push_back(e.seq);
    }
    return out;
}

const char* Ledger::Verify() const {
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const LedgerEntry& e = entries_[i];
        if (e.seq != static_cast<std::uint64_t>(i)) {
            return "entry seq not contiguous";
        }
        const std::uint64_t expect =
            (i == 0) ? kGenesisHash : entries_[i - 1].hash;
        if (e.prevHash != expect) return "chain link broken";
        if (e.hash != EntryHash(e)) return "entry hash mismatch";
        // Full replay, not just chain integrity: a write path that
        // bypasses Post still can't produce a verifying chain.
        if (const char* why = ValidateLegs(e.credit, e.debit)) {
            return why;
        }
        if (const char* why = ValidateMeta(e.memo, e.tags)) {
            return why;
        }
    }
    return nullptr;
}

std::uint64_t Ledger::SealHash(std::uint64_t count,
                               std::uint64_t tip) {
    std::uint64_t h = kGenesisHash;
    for (int i = 0; i < 8; ++i) {
        h ^= static_cast<std::uint8_t>(count >> (i * 8));
        h *= 1099511628211ull;
    }
    for (int i = 0; i < 8; ++i) {
        h ^= static_cast<std::uint8_t>(tip >> (i * 8));
        h *= 1099511628211ull;
    }
    return h;
}

std::uint64_t Ledger::EntryHash(const LedgerEntry& e) {
    // prevHash binds the entry to its position in history; the
    // canonical content emit binds the bytes the chain attests.
    std::uint64_t h = kGenesisHash;
    for (int i = 0; i < 8; ++i) {
        h ^= static_cast<std::uint8_t>(e.prevHash >> (i * 8));
        h *= 1099511628211ull;
    }
    return Fnv1a(EntryContentJson(e).Emit(), h);
}

std::int64_t Ledger::Balance(Account a) const {
    // The no-overflow claim (header) is structural: legs must name
    // different accounts, so each entry touches `a` at most once —
    // |sum| <= MAX_ENTRIES * MAX_AMOUNT. Pin the bound.
    static_assert(MAX_ENTRIES * MAX_AMOUNT <= INT64_MAX,
                  "balance fold can overflow int64");
    std::int64_t sum = 0;
    for (const LedgerEntry& e : entries_) {
        if (e.credit.account == a) sum += e.credit.amount;
        if (e.debit.account == a) sum -= e.debit.amount;
    }
    return sum;
}

Gameplay::Result<JsonValue> Ledger::ToJson() const {
    JsonValue::Array arr;
    arr.reserve(entries_.size());
    for (const LedgerEntry& e : entries_) {
        JsonValue::Object o = EntryContentJson(e).Members();
        o.emplace("suspect", JsonValue::Bool(e.suspect));
        o.emplace("prevHash",
                  JsonValue::Int(static_cast<std::int64_t>(e.prevHash)));
        o.emplace("hash",
                  JsonValue::Int(static_cast<std::int64_t>(e.hash)));
        arr.push_back(JsonValue::MakeObject(std::move(o)));
    }
    JsonValue::Object o;
    o.emplace("schema", JsonValue::String(std::string(SCHEMA)));
    o.emplace("entries", JsonValue::MakeArray(std::move(arr)));
    // The stored seal catches non-recomputing mutation, truncation,
    // and append-tampering alike: it commits to count AND tip. A
    // determined attacker can always recompute — that is the
    // documented boundary (external anchoring = Story 2.5).
    o.emplace("seal", JsonValue::Int(static_cast<std::int64_t>(Seal())));
    return Gameplay::Ok(JsonValue::MakeObject(std::move(o)));
}

Gameplay::Result<Ledger> Ledger::FromJson(const JsonValue& doc) {
    const std::string* schema = doc.FindString("schema");
    if (schema == nullptr || *schema != SCHEMA) {
        return Gameplay::Fail<Ledger>("schema",
                                      "expected potato.ledger/3");
    }
    if (!doc["seal"].IsInt()) {
        return Gameplay::Fail<Ledger>("schema", "missing chain seal");
    }
    if (!doc["entries"].IsArray() ||
        doc["entries"].Size() > MAX_ENTRIES) {
        return Gameplay::Fail<Ledger>("schema",
                                      "entries missing or oversized");
    }
    Ledger out;
    std::uint64_t expectSeq = 0;
    for (const JsonValue& j : doc["entries"].Items()) {
        if (!j.IsObject() || !j["seq"].IsInt() ||
            !j["memo"].IsString() || !j["tags"].IsArray() ||
            !j["provenance"].IsString() || !j["suspect"].IsBool() ||
            !j["prevHash"].IsInt() || !j["hash"].IsInt()) {
            return Gameplay::Fail<Ledger>("schema",
                                          "mistyped entry field");
        }
        if (static_cast<std::uint64_t>(j["seq"].AsInt()) != expectSeq) {
            return Gameplay::Fail<Ledger>(
                "schema", "entry seq not contiguous from 0");
        }
        LedgerEntry e;
        e.seq = expectSeq++;
        e.memo = j["memo"].AsString();
        if (!LegFromJson(j["credit"], e.credit) ||
            !LegFromJson(j["debit"], e.debit)) {
            return Gameplay::Fail<Ledger>("schema", "bad entry leg");
        }
        // Re-validate the invariant on load — the file is untrusted.
        if (const char* why = ValidateLegs(e.credit, e.debit)) {
            return Gameplay::Fail<Ledger>("unbalanced", why);
        }
        for (const JsonValue& t : j["tags"].Items()) {
            if (!t.IsString()) {
                return Gameplay::Fail<Ledger>("schema",
                                              "non-string tag");
            }
            e.tags.push_back(t.AsString());
        }
        if (const char* why = ValidateMeta(e.memo, e.tags)) {
            return Gameplay::Fail<Ledger>("schema", why);
        }
        if (!ProvenanceFromName(j["provenance"].AsString(),
                                e.provenance)) {
            return Gameplay::Fail<Ledger>("schema",
                                          "unknown provenance");
        }
        e.suspect = j["suspect"].AsBool();
        e.prevHash = static_cast<std::uint64_t>(j["prevHash"].AsInt());
        e.hash = static_cast<std::uint64_t>(j["hash"].AsInt());
        out.entries_.push_back(std::move(e));
    }
    // The file is untrusted: replay the chain before accepting. A
    // mutated entry fails verification; load is rejected, not warned.
    if (const char* why = out.Verify()) {
        return Gameplay::Fail<Ledger>("chain", why);
    }
    // Seal check: commits to the entry count too, so truncation needs
    // at least a recompute (same class as a wholesale rehash).
    if (static_cast<std::uint64_t>(doc["seal"].AsInt()) != out.Seal()) {
        return Gameplay::Fail<Ledger>("chain", "seal mismatch");
    }
    return Gameplay::Ok(std::move(out));
}

} // namespace Potato::Campaign
