#pragma once

#include "Campaign/Ledger/Ledger.h"
#include "Gameplay/Json/JsonValue.h"
#include "Gameplay/Result.h"

#include <cstdint>
#include <string>

namespace Potato::Campaign {

class CampaignState;

// One deterministic, single-player proxy auction for campaign recruits.
// Terms are immutable campaign content; only AuctionHouseState is saved.
struct AuctionLot {
    std::string id = "militia_contract";
    std::string title = "鄉勇營招募契約";
    std::string rewardSquad = "鄉勇營";
    std::int64_t openingBid = 40;
    std::int64_t increment = 10;
    std::int64_t rivalMaxBid = 70;
    std::int64_t durationDays = 3;
    static Gameplay::Result<AuctionLot> Load(const std::string& path);
};

class AuctionHouseState {
public:
    static constexpr std::string_view SCHEMA = "potato.auction_state/1";

    bool Open() const { return open_; }
    bool Settled() const { return settled_; }
    std::int64_t ClosesOnDay() const { return closesOnDay_; }
    std::int64_t PlayerMaxBid() const { return playerMaxBid_; }
    std::int64_t ClearingPrice() const { return clearingPrice_; }
    const std::string& LotId() const { return lotId_; }

    Gameplay::Result<bool> Start(const AuctionLot& lot,
                                 std::int64_t currentDay);
    Gameplay::Result<bool> Bid(Ledger& ledger, const AuctionLot& lot,
                               std::int64_t amount,
                               std::int64_t currentDay);
    Gameplay::Result<bool> Settle(CampaignState& campaign,
                                  const AuctionLot& lot,
                                  std::int64_t currentDay);

    Gameplay::JsonValue ToJson() const;
    static Gameplay::Result<AuctionHouseState> FromJson(
        const Gameplay::JsonValue& doc);

private:
    std::string lotId_;
    std::int64_t closesOnDay_ = 0;
    std::int64_t playerMaxBid_ = 0;
    std::int64_t clearingPrice_ = 0;
    bool open_ = false;
    bool settled_ = false;
};

} // namespace Potato::Campaign
