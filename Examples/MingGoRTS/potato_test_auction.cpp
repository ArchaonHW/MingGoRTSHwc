#include "Campaign/Auction/AuctionHouse.h"
#include "Campaign/State/CampaignState.h"

#include <cstdlib>
#include <iostream>

namespace {
int failures = 0;
void Check(bool ok, const char* message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}
}

int main() {
    using namespace Potato::Campaign;
    auto lotContent = AuctionLot::Load(
        std::string(POTATO_AUCTION_DIR) + "/militia_contract.json");
    Check(lotContent.ok() && lotContent.value.id == "militia_contract" &&
              lotContent.value.durationDays == 3,
          "load versioned auction lot content");
    CampaignState campaign;
    Posting seed;
    seed.credit = {Account::Materiel, 200};
    seed.debit = {Account::ArmyPrestige, 200};
    seed.memo = "seed";
    Check(campaign.GetLedger().Post(seed).ok(), "seed campaign funds");

    AuctionLot lot = std::move(lotContent.value);
    AuctionHouseState& auction = campaign.GetAuction();
    Check(auction.Start(lot, 2).ok(), "open auction");
    Check(!auction.Bid(campaign.GetLedger(), lot, 39, 2).ok(),
          "reject below opening price without mutation");
    Check(campaign.GetLedger().Balance(Account::Materiel) == 200,
          "rejected bid leaves balance intact");
    Check(auction.Bid(campaign.GetLedger(), lot, 100, 2).ok(),
          "reserve the player's proxy maximum");
    Check(campaign.GetLedger().Balance(Account::Materiel) == 100 &&
              campaign.GetLedger().Balance(Account::AuctionEscrow) == 100,
          "bid moves funds into escrow");
    Check(!auction.Bid(campaign.GetLedger(), lot, 105, 2).ok(),
          "enforce minimum increment on a raised bid");
    Check(!auction.Bid(campaign.GetLedger(), lot, 300, 2).ok(),
          "reject bids beyond available funds");
    Check(campaign.GetLedger().Balance(Account::Materiel) == 100 &&
              campaign.GetLedger().Balance(Account::AuctionEscrow) == 100,
          "rejected bid updates leave escrow and available funds unchanged");
    Check(auction.Bid(campaign.GetLedger(), lot, 110, 2).ok(),
          "raise maximum by the required increment");

    auto saved = campaign.ToJson();
    Check(saved.ok(), "serialize campaign with active auction");
    auto restored = CampaignState::FromJson(saved.value);
    Check(restored.ok() && restored.value.GetAuction().Open() &&
              restored.value.GetAuction().PlayerMaxBid() == 110,
          "round-trip active auction");
    Check(!auction.Settle(campaign, lot, 4).ok(),
          "auction cannot settle before deadline");
    Check(campaign.GetLedger().Balance(Account::AuctionEscrow) == 110,
          "early settlement attempt leaves escrow untouched");
    auto settled = auction.Settle(campaign, lot, 5);
    Check(settled.ok() && settled.value, "win against proxy bidder");
    Check(auction.ClearingPrice() == 80,
          "second-price settlement uses rival max plus increment");
    Check(campaign.GetLedger().Balance(Account::Materiel) == 120 &&
              campaign.GetLedger().Balance(Account::AuctionEscrow) == 0 &&
              campaign.GetLedger().Balance(Account::AuctionTreasury) == 80,
          "settlement charges clearing price and releases surplus escrow");
    Check(!campaign.GetRoster().empty() &&
              campaign.GetRoster().back().name == lot.rewardSquad,
          "winning bid grants recruit contract");

    auto invalid = Potato::Gameplay::JsonValue::Parse(
        R"({"schema":"bad.schema/1","lot_id":"lot","closes_on_day":3,"player_max_bid":0,"clearing_price":0,"open":false,"settled":false})");
    Check(invalid.ok() && !AuctionHouseState::FromJson(invalid.value).ok(),
          "reject unknown auction state schema");

    CampaignState losingCampaign;
    Check(losingCampaign.GetLedger().Post(seed).ok(), "seed losing campaign");
    AuctionHouseState& losingAuction = losingCampaign.GetAuction();
    Check(losingAuction.Start(lot, 0).ok(), "open second auction");
    Check(losingAuction.Bid(losingCampaign.GetLedger(), lot, 60, 0).ok(),
          "place a lower proxy bid");
    auto lost = losingAuction.Settle(losingCampaign, lot, 3);
    Check(lost.ok() && !lost.value && losingAuction.Settled(),
          "lose against higher rival bid");
    Check(losingCampaign.GetLedger().Balance(Account::Materiel) == 200 &&
              losingCampaign.GetLedger().Balance(Account::AuctionEscrow) == 0 &&
              losingCampaign.GetRoster().empty(),
          "losing auction refunds escrow without awarding a recruit");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
