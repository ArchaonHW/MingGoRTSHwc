#include "Campaign/State/CampaignState.h"

namespace Potato::Campaign {

using Gameplay::JsonValue;
using Gameplay::Result;

namespace {

JsonValue ChapterToJson(const ChapterProgress& c) {
    JsonValue::Object o;
    o["current"] = JsonValue::Int(c.current);
    JsonValue::Array un, res;
    un.reserve(c.unlocked.size());
    res.reserve(c.resolved.size());
    for (bool b : c.unlocked) un.push_back(JsonValue::Bool(b));
    for (bool b : c.resolved) res.push_back(JsonValue::Bool(b));
    o["unlocked"] = JsonValue::MakeArray(std::move(un));
    o["resolved"] = JsonValue::MakeArray(std::move(res));
    return JsonValue::MakeObject(std::move(o));
}

JsonValue RosterToJson(const std::vector<RosterEntry>& roster) {
    JsonValue::Array arr;
    arr.reserve(roster.size());
    for (const RosterEntry& e : roster) {
        JsonValue::Object o;
        o["name"] = JsonValue::String(e.name);
        o["veterancy"] = JsonValue::Int(e.veterancy);
        o["casualties"] = JsonValue::Int(e.casualties);
        o["dead"] = JsonValue::Bool(e.dead);
        arr.push_back(JsonValue::MakeObject(std::move(o)));
    }
    return JsonValue::MakeArray(std::move(arr));
}

const char* ValidateChapter(const JsonValue& j,
                            ChapterProgress& out) {
    if (!j.IsObject() || !j["current"].IsInt() ||
        !j["unlocked"].IsArray() || !j["resolved"].IsArray()) {
        return "chapter: missing/mistyped field";
    }
    const std::int64_t cur = j["current"].AsInt();
    const auto& un = j["unlocked"].Items();
    const auto& res = j["resolved"].Items();
    // `current <= MAX_CHAPTERS` is legal: the campaign-complete
    // sentinel is current==size() and a full space is 64 deep.
    if (cur < 0 ||
        static_cast<std::uint64_t>(cur) > CampaignState::MAX_CHAPTERS) {
        return "chapter.current out of range";
    }
    if (un.size() > CampaignState::MAX_CHAPTERS ||
        res.size() > CampaignState::MAX_CHAPTERS) {
        return "chapter vectors exceed MAX_CHAPTERS";
    }
    // Parallel vectors must agree; `current` is an index into them
    // (empty = library not loaded yet, so current must be 0;
    // current == size = the campaign-complete sentinel).
    if (un.size() != res.size()) {
        return "chapter vectors disagree in size";
    }
    if (un.empty() ? cur != 0
                   : static_cast<std::uint64_t>(cur) > un.size()) {
        return "chapter.current outside the chapter space";
    }
    out.current = cur;
    out.unlocked.clear();
    out.resolved.clear();
    for (const JsonValue& v : un) {
        if (!v.IsBool()) return "unlocked: non-bool element";
        out.unlocked.push_back(v.AsBool());
    }
    for (const JsonValue& v : res) {
        if (!v.IsBool()) return "resolved: non-bool element";
        out.resolved.push_back(v.AsBool());
    }
    return nullptr;
}

const char* ValidateRoster(const JsonValue& j,
                           std::vector<RosterEntry>& out) {
    if (!j.IsArray()) return "roster: not an array";
    if (j.Size() > CampaignState::MAX_ROSTER) {
        return "roster exceeds MAX_ROSTER";
    }
    out.clear();
    for (const JsonValue& e : j.Items()) {
        if (!e.IsObject() || !e["name"].IsString() ||
            !e["veterancy"].IsInt() || !e["casualties"].IsInt() ||
            !e["dead"].IsBool()) {
            return "roster entry: missing/mistyped field";
        }
        const std::string& name = e["name"].AsString();
        if (name.empty() || name.size() > CampaignState::MAX_NAME_LEN) {
            return "roster name out of range";
        }
        // Names are the roster's identity field — reject duplicates
        // now rather than bake ambiguity into old saves.
        for (const RosterEntry& seen : out) {
            if (seen.name == name) return "roster name not unique";
        }
        const std::int64_t vet = e["veterancy"].AsInt();
        const std::int64_t cas = e["casualties"].AsInt();
        if (vet < 0 || vet > CampaignState::MAX_ROSTER_COUNT ||
            cas < 0 || cas > CampaignState::MAX_ROSTER_COUNT) {
            return "roster counter out of range";
        }
        RosterEntry r;
        r.name = name;
        r.veterancy = vet;
        r.casualties = cas;
        r.dead = e["dead"].AsBool();
        out.push_back(std::move(r));
    }
    return nullptr;
}

} // namespace

Result<JsonValue> CampaignState::ToJson() const {
    const std::int64_t expectedEscrow = auction_.Open()
        ? auction_.PlayerMaxBid() : 0;
    if (ledger_.Balance(Account::AuctionEscrow) != expectedEscrow) {
        return Gameplay::Fail<JsonValue>(
            "auction", "auction escrow does not match campaign ledger");
    }
    Result<JsonValue> led = ledger_.ToJson();
    if (!led.ok()) {
        return led; // propagate the ledger's failure
    }
    // GetRoster() hands out a mutable vector&, so live state CAN
    // bypass Enlist/ApplyAftermath's invariants. Re-validate the
    // emitted roster at the trust boundary — a save that
    // FromJson would reject must not be writable.
    JsonValue rosterJson = RosterToJson(roster_);
    {
        std::vector<RosterEntry> scratch;
        if (const char* why = ValidateRoster(rosterJson,
                                             scratch)) {
            return Gameplay::Fail<JsonValue>("roster", why);
        }
    }
    JsonValue::Object o;
    o["schema"] = JsonValue::String(std::string(SCHEMA));
    o["chapter"] = ChapterToJson(chapter_);
    o["roster"] = std::move(rosterJson);
    o["ledger"] = std::move(led.value);
    // Optional sub-docs (potato.campaign/2): emit only when the
    // binding exists — absent == default, so a worldless
    // campaign stays byte-minimal.
    if (hasWorld_) {
        Result<JsonValue> w = world_.ToJson();
        if (!w.ok()) {
            return Gameplay::Fail<JsonValue>("world", w.reason);
        }
        o["world"] = std::move(w.value);
    }
    if (hasCharacter_) {
        o["character"] = character_.ToJson();
    }
    // Resident stores — always emitted (a save is one document;
    // empty stores are honest state, not absence). Each rides
    // its own versioned schema inside the envelope.
    o["intel"] = intel_.ToJson();
    {
        Result<JsonValue> m = myth_.ToJson();
        if (!m.ok()) {
            return Gameplay::Fail<JsonValue>("myth", m.reason);
        }
        o["myth"] = std::move(m.value);
    }
    {
        Result<JsonValue> c = codex_.ToJson();
        if (!c.ok()) {
            return Gameplay::Fail<JsonValue>("codex", c.reason);
        }
        o["codex"] = std::move(c.value);
    }
    o["auction"] = auction_.ToJson();
    return Gameplay::Ok(JsonValue::MakeObject(std::move(o)));
}

Result<CampaignState> CampaignState::FromJson(
    const JsonValue& doc) {
    const std::string* s = doc.FindString("schema");
    if (!s || *s != SCHEMA) {
        return Gameplay::Fail<CampaignState>("schema",
                                             "expected potato.campaign/2");
    }
    CampaignState st;
    if (const char* why = ValidateChapter(doc["chapter"],
                                          st.chapter_)) {
        return Gameplay::Fail<CampaignState>("chapter", why);
    }
    if (const char* why = ValidateRoster(doc["roster"], st.roster_)) {
        return Gameplay::Fail<CampaignState>("roster", why);
    }
    Result<Ledger> led = Ledger::FromJson(doc["ledger"]);
    if (!led.ok()) {
        return Gameplay::Fail<CampaignState>(led.error, led.reason);
    }
    st.ledger_ = std::move(led.value);
    // Optional sub-docs: absent is fine, present-but-invalid
    // rejects the whole envelope. Each sub-parser gates its own
    // `schema` field — the envelope does not re-check it.
    if (doc.Has("world")) {
        const JsonValue& w = doc["world"];
        if (!w.IsObject()) {
            return Gameplay::Fail<CampaignState>(
                "world", "sub-doc must be an object");
        }
        Result<WorldState> ws = WorldState::FromJson(w);
        if (!ws.ok()) {
            return Gameplay::Fail<CampaignState>(ws.error,
                                                 ws.reason);
        }
        st.world_ = std::move(ws.value);
        st.hasWorld_ = true;
    }
    if (doc.Has("character")) {
        const JsonValue& c = doc["character"];
        if (!c.IsObject()) {
            return Gameplay::Fail<CampaignState>(
                "character", "sub-doc must be an object");
        }
        Result<Character> ch = Character::FromJson(c);
        if (!ch.ok()) {
            return Gameplay::Fail<CampaignState>(ch.error,
                                                 ch.reason);
        }
        st.character_ = std::move(ch.value);
        st.hasCharacter_ = true;
    }
    // Resident stores: absent stays empty (saves predate them);
    // present-but-invalid rejects the whole envelope — a broken
    // chronicle must not silently vanish.
    if (doc.Has("intel")) {
        Result<IntelLedger> i = IntelLedger::FromJson(doc["intel"]);
        if (!i.ok()) {
            return Gameplay::Fail<CampaignState>(i.error,
                                                 i.reason);
        }
        st.intel_ = std::move(i.value);
    }
    if (doc.Has("myth")) {
        Result<MythLog> m = MythLog::FromJson(doc["myth"]);
        if (!m.ok()) {
            return Gameplay::Fail<CampaignState>(m.error,
                                                 m.reason);
        }
        st.myth_ = std::move(m.value);
    }
    if (doc.Has("codex")) {
        Result<BencaoCodex> c = BencaoCodex::FromJson(doc["codex"]);
        if (!c.ok()) {
            return Gameplay::Fail<CampaignState>(c.error,
                                                 c.reason);
        }
        st.codex_ = std::move(c.value);
    }
    if (doc.Has("auction")) {
        Result<AuctionHouseState> auction =
            AuctionHouseState::FromJson(doc["auction"]);
        if (!auction.ok()) {
            return Gameplay::Fail<CampaignState>(auction.error,
                                                 auction.reason);
        }
        st.auction_ = std::move(auction.value);
    }
    const std::int64_t expectedEscrow = st.auction_.Open()
        ? st.auction_.PlayerMaxBid() : 0;
    if (st.ledger_.Balance(Account::AuctionEscrow) != expectedEscrow) {
        return Gameplay::Fail<CampaignState>(
            "auction", "auction escrow does not match campaign ledger");
    }
    return Gameplay::Ok(std::move(st));
}

} // namespace Potato::Campaign
