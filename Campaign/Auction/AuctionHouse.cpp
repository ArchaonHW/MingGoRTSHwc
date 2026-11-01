#include "Campaign/Auction/AuctionHouse.h"

#include "Campaign/Roster/Roster.h"
#include "Campaign/State/CampaignState.h"
#include "Gameplay/Json/Json.h"

#include <algorithm>
#include <limits>

namespace Potato::Campaign {
namespace {

Gameplay::Result<std::uint64_t> Post(Ledger& ledger, Account credit,
                                    Account debit, std::int64_t amount,
                                    const char* memo) {
    Posting p;
    p.credit = {credit, amount};
    p.debit = {debit, amount};
    p.memo = memo;
    return ledger.Post(std::move(p));
}

bool ValidLot(const AuctionLot& lot) {
    return !lot.id.empty() && lot.id.size() <= 64 &&
           !lot.title.empty() && lot.title.size() <= 128 &&
           !lot.rewardSquad.empty() && lot.rewardSquad.size() <= 64 &&
           lot.openingBid > 0 && lot.openingBid <= Ledger::MAX_AMOUNT &&
           lot.increment > 0 && lot.increment <= Ledger::MAX_AMOUNT &&
           lot.rivalMaxBid >= 0 && lot.rivalMaxBid <= Ledger::MAX_AMOUNT &&
           lot.durationDays > 0;
}

} // namespace

Gameplay::Result<AuctionLot> AuctionLot::Load(const std::string& path) {
    auto doc = Gameplay::Json::Load(path, "potato.auction_lot/1");
    if (!doc.ok()) return Gameplay::Fail<AuctionLot>(doc.error, doc.reason);
    const std::string* id = doc.value.FindString("id");
    const std::string* title = doc.value.FindString("title");
    const std::string* reward = doc.value.FindString("reward_squad");
    const auto& opening = doc.value["opening_bid"];
    const auto& increment = doc.value["minimum_increment"];
    const auto& rival = doc.value["rival_max_bid"];
    const auto& duration = doc.value["duration_days"];
    if (!id || !title || !reward || !opening.IsInt() ||
        !increment.IsInt() || !rival.IsInt() || !duration.IsInt()) {
        return Gameplay::Fail<AuctionLot>("schema", "auction lot fields missing or mistyped");
    }
    AuctionLot lot{*id, *title, *reward, opening.AsInt(), increment.AsInt(),
                   rival.AsInt(), duration.AsInt()};
    if (!ValidLot(lot)) {
        return Gameplay::Fail<AuctionLot>("schema", "auction lot values out of range");
    }
    return Gameplay::Ok(std::move(lot));
}

Gameplay::Result<bool> AuctionHouseState::Start(const AuctionLot& lot,
                                                std::int64_t day) {
    if (!ValidLot(lot) || day < 0 ||
        day > std::numeric_limits<std::int64_t>::max() - lot.durationDays) {
        return Gameplay::Fail<bool>("auction", "invalid lot or start day");
    }
    if (open_ || settled_) {
        return Gameplay::Fail<bool>("auction", "auction already started");
    }
    lotId_ = lot.id;
    closesOnDay_ = day + lot.durationDays;
    open_ = true;
    return Gameplay::Ok(true);
}

Gameplay::Result<bool> AuctionHouseState::Bid(Ledger& ledger,
                                               const AuctionLot& lot,
                                               std::int64_t amount,
                                               std::int64_t day) {
    if (!open_ || settled_ || !ValidLot(lot) || lot.id != lotId_) {
        return Gameplay::Fail<bool>("auction", "auction is not open");
    }
    if (day >= closesOnDay_) {
        return Gameplay::Fail<bool>("auction", "bidding has closed");
    }
    const std::int64_t minimum = playerMaxBid_ == 0
        ? lot.openingBid
        : (playerMaxBid_ > Ledger::MAX_AMOUNT - lot.increment
               ? Ledger::MAX_AMOUNT : playerMaxBid_ + lot.increment);
    if (amount < minimum || amount > Ledger::MAX_AMOUNT) {
        return Gameplay::Fail<bool>("auction", "bid is below the minimum increment");
    }
    const std::int64_t required = playerMaxBid_ == 0
        ? amount : amount - playerMaxBid_;
    if (ledger.Balance(Account::Materiel) < required) {
        return Gameplay::Fail<bool>("funds", "not enough available materiel");
    }
    auto posted = Post(ledger, Account::AuctionEscrow, Account::Materiel,
                       required, "拍賣出價托管");
    if (!posted.ok()) {
        return Gameplay::Fail<bool>(posted.error, posted.reason);
    }
    playerMaxBid_ = amount;
    return Gameplay::Ok(true);
}

Gameplay::Result<bool> AuctionHouseState::Settle(CampaignState& campaign,
                                                  const AuctionLot& lot,
                                                  std::int64_t day) {
    if (!open_ || settled_ || !ValidLot(lot) || lot.id != lotId_) {
        return Gameplay::Fail<bool>("auction", "auction state or lot mismatch");
    }
    if (day < closesOnDay_) {
        return Gameplay::Fail<bool>("auction", "auction has not closed");
    }
    const bool wins = playerMaxBid_ > lot.rivalMaxBid;
    const std::int64_t price = wins
        ? std::min(playerMaxBid_, std::max(lot.openingBid,
              lot.rivalMaxBid > Ledger::MAX_AMOUNT - lot.increment
                  ? Ledger::MAX_AMOUNT : lot.rivalMaxBid + lot.increment))
        : 0;
    if (wins) {
        if (const char* why = CanEnlist(campaign, lot.rewardSquad)) {
            return Gameplay::Fail<bool>("roster", why);
        }
    }

    // Work on copies so a failed ledger post or roster validation leaves
    // both the live campaign and auction untouched.
    CampaignState next = campaign;
    AuctionHouseState nextAuction = *this;
    const std::int64_t escrow = playerMaxBid_;
    if (escrow > 0) {
        auto refund = Post(next.GetLedger(), Account::Materiel,
                           Account::AuctionEscrow, escrow, "拍賣托管退回");
        if (!refund.ok()) {
            return Gameplay::Fail<bool>(refund.error, refund.reason);
        }
    }
    if (price > 0) {
        auto pay = Post(next.GetLedger(), Account::AuctionTreasury,
                        Account::Materiel, price, "拍賣成交");
        if (!pay.ok()) return Gameplay::Fail<bool>(pay.error, pay.reason);
        auto enlisted = Enlist(next, lot.rewardSquad);
        if (!enlisted.ok()) {
            return Gameplay::Fail<bool>(enlisted.error, enlisted.reason);
        }
    }
    nextAuction.open_ = false;
    nextAuction.settled_ = true;
    nextAuction.clearingPrice_ = price;
    campaign = std::move(next);
    *this = std::move(nextAuction);
    return Gameplay::Ok(wins);
}

Gameplay::JsonValue AuctionHouseState::ToJson() const {
    using Gameplay::JsonValue;
    JsonValue::Object o;
    o["schema"] = JsonValue::String(std::string(SCHEMA));
    o["lot_id"] = JsonValue::String(lotId_);
    o["closes_on_day"] = JsonValue::Int(closesOnDay_);
    o["player_max_bid"] = JsonValue::Int(playerMaxBid_);
    o["clearing_price"] = JsonValue::Int(clearingPrice_);
    o["open"] = JsonValue::Bool(open_);
    o["settled"] = JsonValue::Bool(settled_);
    return JsonValue::MakeObject(std::move(o));
}

Gameplay::Result<AuctionHouseState> AuctionHouseState::FromJson(
    const Gameplay::JsonValue& doc) {
    const std::string* schema = doc.FindString("schema");
    const std::string* lot = doc.FindString("lot_id");
    if (!schema || *schema != SCHEMA || !lot || lot->size() > 64 ||
        !doc["closes_on_day"].IsInt() ||
        !doc["player_max_bid"].IsInt() ||
        !doc["clearing_price"].IsInt() ||
        !doc["open"].IsBool() || !doc["settled"].IsBool()) {
        return Gameplay::Fail<AuctionHouseState>("schema", "invalid auction state fields");
    }
    AuctionHouseState state;
    state.lotId_ = *lot;
    state.closesOnDay_ = doc["closes_on_day"].AsInt();
    state.playerMaxBid_ = doc["player_max_bid"].AsInt();
    state.clearingPrice_ = doc["clearing_price"].AsInt();
    state.open_ = doc["open"].AsBool();
    state.settled_ = doc["settled"].AsBool();
    if (state.closesOnDay_ < 0 || state.playerMaxBid_ < 0 ||
        state.playerMaxBid_ > Ledger::MAX_AMOUNT ||
        state.clearingPrice_ < 0 || state.clearingPrice_ > state.playerMaxBid_ ||
        (state.open_ && (state.settled_ || state.lotId_.empty() ||
                         state.closesOnDay_ == 0 ||
                         state.clearingPrice_ != 0)) ||
        (state.settled_ && (state.open_ || state.lotId_.empty() ||
                            state.closesOnDay_ == 0)) ||
        (!state.open_ && !state.settled_ &&
         (!state.lotId_.empty() || state.closesOnDay_ != 0 ||
          state.playerMaxBid_ != 0 || state.clearingPrice_ != 0))) {
        return Gameplay::Fail<AuctionHouseState>("schema", "inconsistent auction state");
    }
    return Gameplay::Ok(std::move(state));
}

} // namespace Potato::Campaign
