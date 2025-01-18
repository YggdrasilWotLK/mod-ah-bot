/*
 * Copyright (C) 2008-2010 Trinity <http://www.trinitycore.org/>
 * Copyright (C) 2005-2009 MaNGOS <http://getmangos.com/>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include "ObjectMgr.h"
#include "AuctionHouseMgr.h"
#include "Config.h"
#include "Player.h"
#include "WorldSession.h"
#include "GameTime.h"
#include "DatabaseEnv.h"

#include "AuctionHouseBot.h"
#include "AuctionHouseBotCommon.h"

using namespace std;

AuctionHouseBot::AuctionHouseBot(uint32 account, uint32 id)
{
    _account        = account;
    _id             = id;

    _lastrun_a_sec  = time(NULL);
    _lastrun_h_sec  = time(NULL);
    _lastrun_n_sec  = time(NULL);

    _allianceConfig = NULL;
    _hordeConfig    = NULL;
    _neutralConfig  = NULL;
}

AuctionHouseBot::~AuctionHouseBot()
{
    // Nothing
}

uint32 AuctionHouseBot::getElement(std::set<uint32> set, int index, uint32 botId, uint32 maxDup, AuctionHouseObject* auctionHouse)
{
    std::set<uint32>::iterator it = set.begin();
    std::advance(it, index);

    if (maxDup > 0)
    {
        uint32 noStacks = 0;

        for (AuctionHouseObject::AuctionEntryMap::const_iterator itr = auctionHouse->GetAuctionsBegin(); itr != auctionHouse->GetAuctionsEnd(); ++itr)
        {
            AuctionEntry* Aentry = itr->second;

            if (Aentry->owner.GetCounter() == botId)
            {
                if (*it == Aentry->item_template)
                {
                    noStacks++;
                }
            }
        }

        if (noStacks >= maxDup)
        {
            return 0;
        }
    }

    return *it;
}

uint32 AuctionHouseBot::getStackCount(AHBConfig* config, uint32 max)
{
    if (max == 1)
    {
        return 1;
    }

    // 
    // Organize the stacks in a pseudo random way
    // 

    if (config->DivisibleStacks)
    {
        uint32 ret = 0;

        if (max % 5 == 0) // 5, 10, 15, 20
        {
            ret = urand(1, 4) * 5;
        }

        if (max % 4 == 0) // 4, 8, 12, 16
        {
            ret = urand(1, 4) * 4;
        }

        if (max % 3 == 0) // 3, 6, 9, 18
        {
            ret = urand(1, 3) * 3;
        }

        if (ret > max)
        {
            ret = max;
        }

        return ret;
    }

    // 
    // Totally random
    // 

    return urand(1, max);
}

uint32 AuctionHouseBot::getElapsedTime(uint32 timeClass)
{
    switch (timeClass)
    {
    case 2:
        return urand(1, 5) * 600;   // SHORT = In the range of one hour

    case 1:
        return urand(1, 23) * 3600; // MEDIUM = In the range of one day

    default:
        return urand(1, 3) * 86400; // LONG = More than one day but less than three
    }
}

uint32 AuctionHouseBot::getNofAuctions(AHBConfig* config, AuctionHouseObject* auctionHouse, ObjectGuid guid)
{
    //
    // All the auctions
    //

    if (!config->ConsiderOnlyBotAuctions)
    {
        return auctionHouse->Getcount();
    }

    //
    // Just the one handled by the bot
    //

    uint32 count = 0;

    for (AuctionHouseObject::AuctionEntryMap::const_iterator itr = auctionHouse->GetAuctionsBegin(); itr != auctionHouse->GetAuctionsEnd(); ++itr)
    {
        AuctionEntry* Aentry = itr->second;

        if (guid == Aentry->owner)
        {
            count++;
        }
    }

    return count;
}

// =============================================================================
// This routine performs the bidding operations for the bot
// =============================================================================

void AuctionHouseBot::Buy(Player* AHBplayer, AHBConfig* config, WorldSession* session)
{
    //
    // Check if disabled
    //

    if (!config->AHBBuyer)
    {
        return;
    }

    //
    // Retrieve items not owner by the bot and not bought by the bot
    //

    QueryResult result = CharacterDatabase.Query("SELECT id FROM auctionhouse WHERE itemowner<>{} AND buyguid<>{}", _id, _id);

    if (!result)
    {
        return;
    }

    if (result->GetRowCount() == 0)
    {
        return;
    }

    //
    // Fetches content of selected AH to look for possible bids
    //

    AuctionHouseObject* auctionHouse = sAuctionMgr->GetAuctionsMap(config->GetAHFID());
    std::set<uint32>    possibleBids;

    do
    {
        uint32 tmpdata = result->Fetch()->Get<uint32>();
        possibleBids.insert(tmpdata);
    } while (result->NextRow());

    //
    // If it's not possible to bid stop here
    //

    if (possibleBids.empty())
    {
        if (config->DebugOutBuyer)
        {
            LOG_INFO("module", "AHBot [{}]: no auctions to bid on has been recovered", _id);
        }

        return;
    }

    //
    // Perform the operation for a maximum amount of bids attempts configured
    //

    for (uint32 count = 1; count <= config->GetBidsPerInterval(); ++count)
    {
        //
        // Choose a random auction from possible auctions
        //

        uint32 randBid = urand(0, possibleBids.size() - 1);

        std::set<uint32>::iterator it = possibleBids.begin();
        std::advance(it, randBid);

        AuctionEntry* auction = auctionHouse->GetAuction(*it);

        //
        // Prevent to bid again on the same auction
        //

        possibleBids.erase(randBid);

        if (!auction)
        {
            continue;
        }

        //
        // Prevent from buying items from the other bots
        //

        if (gBotsId.find(auction->owner.GetCounter()) != gBotsId.end())
        {
            continue;
        }

        //
        // Get the item information
        //

        Item* pItem = sAuctionMgr->GetAItem(auction->item_guid);

        if (!pItem)
        {
            if (config->DebugOutBuyer)
            {
                LOG_ERROR("module", "AHBot [{}]: item {} doesn't exist, perhaps bought already?", _id, auction->item_guid.ToString());
            }

            continue;
        }

        //
        // Get the item prototype
        //

        ItemTemplate const* prototype = sObjectMgr->GetItemTemplate(auction->item_template);

        //
        // Check which price we have to use, startbid or if it is bidded already
        //

        uint32 currentprice;

        if (auction->bid)
        {
            currentprice = auction->bid;
        }
        else
        {
            currentprice = auction->startbid;
        }

        //
        // Prepare portion from maximum bid
        //

        double      bidrate = static_cast<double>(urand(1, 100)) / 100;
        long double bidMax  = 0;

        //
        // Check that bid has an acceptable value and take bid based on vendorprice, stacksize and quality
        //

        if (config->BuyMethod)
        {
            if (prototype->Quality <= AHB_MAX_QUALITY)
            {
                if (currentprice < prototype->SellPrice * pItem->GetCount() * config->GetBuyerPrice(prototype->Quality))
                {
                    bidMax = prototype->SellPrice * pItem->GetCount() * config->GetBuyerPrice(prototype->Quality);
                }
            }
            else
            {
                if (config->DebugOutBuyer)
                {
                    LOG_ERROR("module", "AHBot [{}]: Quality {} not Supported", _id, prototype->Quality);
                }

                continue;
            }
        }
        else
        {
            if (prototype->Quality <= AHB_MAX_QUALITY)
            {
                if (currentprice < prototype->BuyPrice * pItem->GetCount() * config->GetBuyerPrice(prototype->Quality))
                {
                    bidMax = prototype->BuyPrice * pItem->GetCount() * config->GetBuyerPrice(prototype->Quality);
                }
            }
            else
            {
                if (config->DebugOutBuyer)
                {
                    LOG_ERROR("module", "AHBot [{}]: Quality {} not Supported", _id, prototype->Quality);
                }

                continue;
            }
        }

        //
        // Recalculate the bid depending on the type of the item
        //

        switch (prototype->Class)
        {
            // ammo
        case 6:
            bidMax = 0;
            break;
        default:
            break;
        }

        //
        // Test the computed bid
        //

        if (bidMax == 0)
        {
            continue;
        }

        //
        // Calculate our bid
        //

        long double bidvalue = currentprice + ((bidMax - currentprice) * bidrate);
        uint32      bidprice = static_cast<uint32>(bidvalue);

        //
        // Check our bid is high enough to be valid. If not, correct it to minimum.
        //

        if ((currentprice + auction->GetAuctionOutBid()) > bidprice)
        {
            bidprice = currentprice + auction->GetAuctionOutBid();
        }

        //
        // Print out debug info
        //

        if (config->DebugOutBuyer)
        {
            LOG_INFO("module", "-------------------------------------------------");
            LOG_INFO("module", "AHBot [{}]: Info for Auction #{}:", _id, auction->Id);
            LOG_INFO("module", "AHBot [{}]: AuctionHouse: {}"     , _id, auction->GetHouseId());
            LOG_INFO("module", "AHBot [{}]: Owner: {}"            , _id, auction->owner.ToString());
            LOG_INFO("module", "AHBot [{}]: Bidder: {}"           , _id, auction->bidder.ToString());
            LOG_INFO("module", "AHBot [{}]: Starting Bid: {}"     , _id, auction->startbid);
            LOG_INFO("module", "AHBot [{}]: Current Bid: {}"      , _id, currentprice);
            LOG_INFO("module", "AHBot [{}]: Buyout: {}"           , _id, auction->buyout);
            LOG_INFO("module", "AHBot [{}]: Deposit: {}"          , _id, auction->deposit);
            LOG_INFO("module", "AHBot [{}]: Expire Time: {}"      , _id, uint32(auction->expire_time));
            LOG_INFO("module", "AHBot [{}]: Bid Rate: {}"         , _id, bidrate);
            LOG_INFO("module", "AHBot [{}]: Bid Max: {}"          , _id, bidMax);
            LOG_INFO("module", "AHBot [{}]: Bid Value: {}"        , _id, bidvalue);
            LOG_INFO("module", "AHBot [{}]: Bid Price: {}"        , _id, bidprice);
            LOG_INFO("module", "AHBot [{}]: Item GUID: {}"        , _id, auction->item_guid.ToString());
            LOG_INFO("module", "AHBot [{}]: Item Template: {}"    , _id, auction->item_template);
            LOG_INFO("module", "AHBot [{}]: Item Info:");
            LOG_INFO("module", "AHBot [{}]: Item ID: {}"          , _id, prototype->ItemId);
            LOG_INFO("module", "AHBot [{}]: Buy Price: {}"        , _id, prototype->BuyPrice);
            LOG_INFO("module", "AHBot [{}]: Sell Price: {}"       , _id, prototype->SellPrice);
            LOG_INFO("module", "AHBot [{}]: Bonding: {}"          , _id, prototype->Bonding);
            LOG_INFO("module", "AHBot [{}]: Quality: {}"          , _id, prototype->Quality);
            LOG_INFO("module", "AHBot [{}]: Item Level: {}"       , _id, prototype->ItemLevel);
            LOG_INFO("module", "AHBot [{}]: Ammo Type: {}"        , _id, prototype->AmmoType);
            LOG_INFO("module", "-------------------------------------------------");
        }

        //
        // Check whether we do normal bid, or buyout
        //

        bool bought = false;

        if ((bidprice < auction->buyout) || (auction->buyout == 0))
        {
            //
            // Perform a new bid on the auction
            //
        
            if (auction->bidder)
            {
                if (auction->bidder != AHBplayer->GetGUID())
                {
                    //
                    // Mail to last bidder and return their money
                    //
        
                    auto trans = CharacterDatabase.BeginTransaction();
        
                    sAuctionMgr->SendAuctionOutbiddedMail(auction, bidprice, session->GetPlayer(), trans);
                    CharacterDatabase.CommitTransaction  (trans);
                }
            }
        
            auction->bidder = AHBplayer->GetGUID();
            auction->bid    = bidprice;
        
            //
            // Save the auction into database
            //
        
            CharacterDatabase.Execute("UPDATE auctionhouse SET buyguid = '{}', lastbid = '{}' WHERE id = '{}'", auction->bidder.GetCounter(), auction->bid, auction->Id);
        }
        else
        {
            bought = true;

            //
            // Perform the buyout
            //

            auto trans = CharacterDatabase.BeginTransaction();

            if ((auction->bidder) && (AHBplayer->GetGUID() != auction->bidder))
            {
                //
                // Send the mail to the last bidder
                //

                sAuctionMgr->SendAuctionOutbiddedMail(auction, auction->buyout, session->GetPlayer(), trans);
            }

            auction->bidder = AHBplayer->GetGUID();
            auction->bid    = auction->buyout;

            // 
            // Send mails to buyer & seller
            // 

            sAuctionMgr->SendAuctionSuccessfulMail(auction, trans);
            sAuctionMgr->SendAuctionWonMail       (auction, trans);

            // 
            // Removes any trace of the item
            // 

            auction->DeleteFromDB(trans);

            sAuctionMgr->RemoveAItem   (auction->item_guid);
            auctionHouse->RemoveAuction(auction);

            CharacterDatabase.CommitTransaction(trans);
        }

        //
        // Tracing
        //

        if (config->TraceBuyer)
        {
            if (bought)
            {
                LOG_INFO("module", "AHBot [{}]: Bought , id={}, ah={}, item={}, start={}, current={}, buyout={}", _id, prototype->ItemId, auction->GetHouseId(), auction->item_template, auction->startbid, currentprice, auction->buyout);
            }
            else
            {
                LOG_INFO("module", "AHBot [{}]: New bid, id={}, ah={}, item={}, start={}, current={}, buyout={}", _id, prototype->ItemId, auction->GetHouseId(), auction->item_template, auction->startbid, currentprice, auction->buyout);
            }
        }
    }
}

// =============================================================================
// This routine performs the selling operations for the bot
// =============================================================================

void AuctionHouseBot::Sell(Player* AHBplayer, AHBConfig* config)
{
    // 
    // Check if disabled
    // 

    if (!config->AHBSeller)
    {
        return;
    }

    // 
    // Check the given limits
    // 

    uint32 minItems = config->GetMinItems();
    uint32 maxItems = config->GetMaxItems();

    if (maxItems == 0)
    {
        return;
    }

    // 
    // Retrieve the auction house situation
    // 

    AuctionHouseEntry const* ahEntry = sAuctionMgr->GetAuctionHouseEntryFromFactionTemplate(config->GetAHFID());

    if (!ahEntry)
    {
        return;
    }

    AuctionHouseObject* auctionHouse = sAuctionMgr->GetAuctionsMap(config->GetAHFID());

    if (!auctionHouse)
    {
        return;
    }

    auctionHouse->Update();

    // 
    // Check if we are clear to proceed
    // 

    bool   aboveMin = false;
    bool   aboveMax = false;
    uint32 auctions = getNofAuctions(config, auctionHouse, AHBplayer->GetGUID());
    uint32 items    = 0;

    if (auctions >= minItems)
    {
        aboveMin = true;

        if (config->DebugOutSeller)
        {
            LOG_ERROR("module", "AHBot [{}]: Auctions above minimum", _id);
        }

        return;
    }

    if (auctions >= maxItems)
    {
        aboveMax = true;

        if (config->DebugOutSeller)
        {
            LOG_ERROR("module", "AHBot [{}]: Auctions at or above maximum", _id);
        }

        return;
    }

    if ((maxItems - auctions) >= config->ItemsPerCycle)
    {
        items = config->ItemsPerCycle;
    }
    else
    {
        items = (maxItems - auctions);
    }

    // 
    // Retrieve the configuration for this run
    // 

    uint32 greyTGcount   = config->GetMaximum(AHB_GREY_TG);
    uint32 whiteTGcount  = config->GetMaximum(AHB_WHITE_TG);
    uint32 greenTGcount  = config->GetMaximum(AHB_GREEN_TG);
    uint32 blueTGcount   = config->GetMaximum(AHB_BLUE_TG);
    uint32 purpleTGcount = config->GetMaximum(AHB_PURPLE_TG);
    uint32 orangeTGcount = config->GetMaximum(AHB_ORANGE_TG);
    uint32 yellowTGcount = config->GetMaximum(AHB_YELLOW_TG);

    uint32 greyIcount    = config->GetMaximum(AHB_GREY_I);
    uint32 whiteIcount   = config->GetMaximum(AHB_WHITE_I);
    uint32 greenIcount   = config->GetMaximum(AHB_GREEN_I);
    uint32 blueIcount    = config->GetMaximum(AHB_BLUE_I);
    uint32 purpleIcount  = config->GetMaximum(AHB_PURPLE_I);
    uint32 orangeIcount  = config->GetMaximum(AHB_ORANGE_I);
    uint32 yellowIcount  = config->GetMaximum(AHB_YELLOW_I);

    uint32 greyTGoods    = config->GetItemCounts(AHB_GREY_TG);
    uint32 whiteTGoods   = config->GetItemCounts(AHB_WHITE_TG);
    uint32 greenTGoods   = config->GetItemCounts(AHB_GREEN_TG);
    uint32 blueTGoods    = config->GetItemCounts(AHB_BLUE_TG);
    uint32 purpleTGoods  = config->GetItemCounts(AHB_PURPLE_TG);
    uint32 orangeTGoods  = config->GetItemCounts(AHB_ORANGE_TG);
    uint32 yellowTGoods  = config->GetItemCounts(AHB_YELLOW_TG);

    uint32 greyItems     = config->GetItemCounts(AHB_GREY_I);
    uint32 whiteItems    = config->GetItemCounts(AHB_WHITE_I);
    uint32 greenItems    = config->GetItemCounts(AHB_GREEN_I);
    uint32 blueItems     = config->GetItemCounts(AHB_BLUE_I);
    uint32 purpleItems   = config->GetItemCounts(AHB_PURPLE_I);
    uint32 orangeItems   = config->GetItemCounts(AHB_ORANGE_I);
    uint32 yellowItems   = config->GetItemCounts(AHB_YELLOW_I);

    //
    // Loop variables
    //

    uint32 noSold    = 0; // Tracing counter
    uint32 binEmpty  = 0; // Tracing counter
    uint32 noNeed    = 0; // Tracing counter
    uint32 tooMany   = 0; // Tracing counter
    uint32 loopBrk   = 0; // Tracing counter
    uint32 err       = 0; // Tracing counter

    for (uint32 cnt = 1; cnt <= items; cnt++)
    {
        uint32 choice      = 0;
        uint32 itemID      = 0;
        uint32 loopbreaker = 0;

        //
        // Select, in rarity order, a new random item
        //

        while (itemID == 0 && loopbreaker <= AUCTION_HOUSE_BOT_LOOP_BREAKER)
        {
            loopbreaker++;

            // Poor

            if ((config->GreyItemsBin.size() > 0) && (greyItems < greyIcount))
            {
                choice = 0;
                itemID = getElement(config->GreyItemsBin, urand(0, config->GreyItemsBin.size() - 1), _id, config->DuplicatesCount, auctionHouse);
            }

            if (itemID == 0 && (config->GreyTradeGoodsBin.size() > 0) && (greyTGoods < greyTGcount))
            {
                choice = 7;
                itemID = getElement(config->GreyTradeGoodsBin, urand(0, config->GreyTradeGoodsBin.size() - 1), _id, config->DuplicatesCount, auctionHouse);
            }

            // Normal

            if (itemID == 0 && (config->WhiteItemsBin.size() > 0) && (whiteItems < whiteIcount))
            {
                choice = 1;
                itemID = getElement(config->WhiteItemsBin, urand(0, config->WhiteItemsBin.size() - 1), _id, config->DuplicatesCount, auctionHouse);
            }

            if (itemID == 0 && (config->WhiteTradeGoodsBin.size() > 0) && (whiteTGoods < whiteTGcount))
            {
                choice = 8;
                itemID = getElement(config->WhiteTradeGoodsBin, urand(0, config->WhiteTradeGoodsBin.size() - 1), _id, config->DuplicatesCount, auctionHouse);
            }

            // Uncommon

            if (itemID == 0 && (config->GreenItemsBin.size() > 0) && (greenItems < greenIcount))
            {
                choice = 2;
                itemID = getElement(config->GreenItemsBin, urand(0, config->GreenItemsBin.size() - 1), _id, config->DuplicatesCount, auctionHouse);
            }

            if (itemID == 0 && (config->GreenTradeGoodsBin.size() > 0) && (greenTGoods < greenTGcount))
            {
                choice = 9;
                itemID = getElement(config->GreenTradeGoodsBin, urand(0, config->GreenTradeGoodsBin.size() - 1), _id, config->DuplicatesCount, auctionHouse);
            }

            // Rare

            if (itemID == 0 && (config->BlueItemsBin.size() > 0) && (blueItems < blueIcount))
            {
                choice = 3;
                itemID = getElement(config->BlueItemsBin, urand(0, config->BlueItemsBin.size() - 1), _id, config->DuplicatesCount, auctionHouse);
            }

            if (itemID == 0 && (config->BlueTradeGoodsBin.size() > 0) && (blueTGoods < blueTGcount))
            {
                choice = 10;
                itemID = getElement(config->BlueTradeGoodsBin, urand(0, config->BlueTradeGoodsBin.size() - 1), _id, config->DuplicatesCount, auctionHouse);
            }

            // Epic

            if (itemID == 0 && (config->PurpleItemsBin.size() > 0) && (purpleItems < purpleIcount))
            {
                choice = 4;
                itemID = getElement(config->PurpleItemsBin, urand(0, config->PurpleItemsBin.size() - 1), _id, config->DuplicatesCount, auctionHouse);
            }

            if (itemID == 0 && (config->PurpleTradeGoodsBin.size() > 0) && (purpleTGoods < purpleTGcount))
            {
                choice = 11;
                itemID = getElement(config->PurpleTradeGoodsBin, urand(0, config->PurpleTradeGoodsBin.size() - 1), _id, config->DuplicatesCount, auctionHouse);
            }

            // Legendary

            if (itemID == 0 && (config->OrangeItemsBin.size() > 0) && (orangeItems < orangeIcount))
            {
                choice = 5;
                itemID = getElement(config->OrangeItemsBin, urand(0, config->OrangeItemsBin.size() - 1), _id, config->DuplicatesCount, auctionHouse);
            }

            if (itemID == 0 && (config->OrangeTradeGoodsBin.size() > 0) && (orangeTGoods < orangeTGcount))
            {
                choice = 12;
                itemID = getElement(config->OrangeTradeGoodsBin, urand(0, config->OrangeTradeGoodsBin.size() - 1), _id, config->DuplicatesCount, auctionHouse);
            }

            // Artifact

            if (itemID == 0 && (config->YellowItemsBin.size() > 0) && (yellowItems < yellowIcount))
            {
                choice = 6;
                itemID = getElement(config->YellowItemsBin, urand(0, config->YellowItemsBin.size() - 1), _id, config->DuplicatesCount, auctionHouse);
            }

            if (itemID == 0 && (config->YellowTradeGoodsBin.size() > 0) && (yellowTGoods < yellowTGcount))
            {
                choice = 13;
                itemID = getElement(config->YellowTradeGoodsBin, urand(0, config->YellowTradeGoodsBin.size() - 1), _id, config->DuplicatesCount, auctionHouse);
            }

            if (itemID == 0)
            {
                binEmpty++;
            
                if (config->DebugOutSeller)
                {
                    LOG_ERROR("module", "AHBot [{}]: No item could be selected from the bins", _id);
                }
            
                break;
            }
        }

        if (itemID == 0 || loopbreaker > AUCTION_HOUSE_BOT_LOOP_BREAKER)
        {
            loopBrk++;
            continue;
        }

        // 
        // Retrieve information about the selected item
        // 

        ItemTemplate const* prototype = sObjectMgr->GetItemTemplate(itemID);

        if (prototype == NULL)
        {
            err++;

            if (config->DebugOutSeller)
            {
                LOG_ERROR("module", "AHBot [{}]: could not get prototype of item {}", _id, itemID);
            }

            continue;
        }

        Item* item = Item::CreateItem(itemID, 1, AHBplayer);

        if (item == NULL)
        {
            err++;

            if (config->DebugOutSeller)
            {
                LOG_ERROR("module", "AHBot [{}]: could not create item from prototype {}", _id, itemID);
            }

            continue;
        }

        // 
        // Start interacting with the item by adding a random property
        // 

        item->AddToUpdateQueueOf(AHBplayer);

        uint32 randomPropertyId = Item::GenerateItemRandomPropertyId(itemID);

        if (randomPropertyId != 0)
        {
            item->SetItemRandomProperties(randomPropertyId);
        }

        if (prototype->Quality > AHB_MAX_QUALITY)
        {
            err++;

            if (config->DebugOutSeller)
            {
                LOG_ERROR("module", "AHBot [{}]: Quality {} TOO HIGH for item {}", _id, prototype->Quality, itemID);
            }

            item->RemoveFromUpdateQueueOf(AHBplayer);
            continue;
        }

        // 
        // Determine the price
        // 

        uint64 buyoutPrice = 0;
        uint64 bidPrice    = 0;
        uint32 stackCount  = 1;

        if (config->SellAtMarketPrice)
        {
            buyoutPrice = config->GetItemPrice(itemID);
        }

        if (buyoutPrice == 0)
        {
            if (config->SellMethod)
            {
                buyoutPrice = prototype->BuyPrice;
            }
            else
            {
                buyoutPrice = prototype->SellPrice;
            }
        }

        buyoutPrice = buyoutPrice * urand(config->GetMinPrice(prototype->Quality), config->GetMaxPrice(prototype->Quality));
        buyoutPrice = buyoutPrice / 100;

        bidPrice    = buyoutPrice * urand(config->GetMinBidPrice(prototype->Quality), config->GetMaxBidPrice(prototype->Quality));
        bidPrice    = bidPrice / 100;

        // 
        // Determine the stack size
        // 

        if (config->GetMaxStack(prototype->Quality) > 1 && item->GetMaxStackCount() > 1)
        {
            stackCount = minValue(getStackCount(config, item->GetMaxStackCount()), config->GetMaxStack(prototype->Quality));
        }
        else if (config->GetMaxStack(prototype->Quality) == 0 && item->GetMaxStackCount() > 1)
        {
            stackCount = getStackCount(config, item->GetMaxStackCount());
        }
        else
        {
            stackCount = 1;
        }

        item->SetCount(stackCount);

        // 
        // Determine the auction time
        // 

        uint32 etime = getElapsedTime(config->ElapsingTimeClass);

        // 
        // Determine the deposit
        // 

        uint32 dep   = sAuctionMgr->GetAuctionDeposit(ahEntry, etime, item, stackCount);

        // 
        // Perform the auction
        // 

        if (prototype && prototype->SellPrice && prototype->SellPrice > 0)
        {
            uint32 itemEntry = prototype->ItemId;
            uint32 minPrice = 0;
            uint32 maxPrice = 0;
            bool priceOverridden = false;

            // Helper function to check if item is in a list
            auto isInList = [](uint32 id, const std::initializer_list<uint32>& list) {
                return std::find(list.begin(), list.end(), id) != list.end();
            };

            // Define the item lists
            const std::initializer_list<uint32> gems = {    36766, 36767, 36919, 36922, 36925, 36928, 36931, 36934, 40111, 40112, 40113, 40114, 40115, 40116, 40117, 40118, 40119, 40120, 40121, 40122, 40123, 40124, 40125, 40126, 40127, 40128, 40129, 40130, 40131, 40132, 40133, 40134, 40135, 40136, 40137, 40138, 40139, 40140, 40141, 40142, 40143, 40144, 40145, 40146, 40147, 40148, 40149, 40150, 40151, 40152, 40153, 40154, 40155, 40156, 40157, 40158, 40159, 40160, 40161, 40162, 40163, 40164, 40165, 40166, 40167, 40168, 40169, 40170, 40171, 40172, 40173, 40174, 40175, 40176, 40177, 40178, 40179, 40180, 40181, 40182, 42142, 42143, 42144, 42145, 42146, 42148, 42149, 42150, 42151, 42152, 42153, 42154, 42155, 42156, 42157, 42158, 44066, 45862, 45879, 45880, 45881, 45882, 45883, 45987, 49110};
            const std::initializer_list<uint32> CraftedHeadChestLegs245 = {    42987, 47589, 47590, 47591, 47592, 47593, 47594, 47595, 47596, 47597, 47598, 47599, 47600, 47601, 47602, 47603, 47604, 47605, 47606, 50001, 50038};
            const std::initializer_list<uint32> ilvl200to245 = {	37835, 39194, 39235, 39283, 39310, 39472, 39717, 39733, 39762, 40187, 40206, 40246, 40270, 40282, 40302, 40305, 40338, 40347, 40362, 40426, 40439, 40474, 40553, 40558, 40733, 40734, 40735, 40736, 40737, 40738, 40739, 40740, 40741, 41168, 41257, 41383, 41384, 41386, 41387, 41388, 41391, 41392, 41394, 41609, 41610, 42100, 42101, 42102, 42103, 42111, 42113, 42435, 42508, 42642, 42643, 42644, 42645, 42646, 42647, 42989, 42990, 43458, 43459, 43461, 43469, 43481, 43484, 43495, 43502, 43565, 43566, 43573, 43582, 43583, 43584, 43585, 43586, 43587, 43588, 43590, 43591, 43592, 43593, 43594, 43595, 43611, 43612, 43613, 44308, 44309, 44310, 44311, 44312, 44313, 44504, 44926, 44930, 44931, 44948, 45085, 45107, 45141, 45167, 45237, 45247, 45274, 45291, 45301, 45316, 45322, 45435, 45450, 45468, 45480, 45493, 45504, 45550, 45551, 45552, 45553, 45554, 45555, 45556, 45557, 45558, 45559, 45560, 45561, 45562, 45563, 45564, 45565, 45566, 45567, 45680, 45704, 45709, 45859, 45874, 45927, 45975, 46009, 48663, 52252};
            const std::initializer_list<uint32> OtherBOEs = {    37835, 42988, 44253, 44254, 44255, 33350, 43573, 46970, 47089, 47105, 47149, 47223, 47257, 47278, 47291, 47297, 47315, 47570, 47571, 47572, 47573, 47574, 47575, 47576, 47577, 47579, 47580, 47581, 47582, 47583, 47584, 47585, 47586, 47587, 47588, 49890, 49891, 49892, 49893, 49894, 49895, 49896, 49897, 49898, 49899, 49900, 49901, 49902, 49903, 49904, 49905, 49906, 49907, 49967, 49994, 50015, 50020, 50069, 50175, 50182, 50444, 50447, 50449, 50450, 50451, 50452, 50453, 50472};

            // Check which category the item belongs to and set price range
            if (isInList(itemEntry, gems))
            {
                minPrice = 1000000;
                maxPrice = 2000000;
                priceOverridden = true;
            }
            else if (isInList(itemEntry, CraftedHeadChestLegs245))
            {
                minPrice = 28000000;
                maxPrice = 37000000;
                priceOverridden = true;
            }
            else if (isInList(itemEntry, ilvl200to245))
            {
                minPrice = 8000000;
                maxPrice = 12000000;
                priceOverridden = true;
            }
            else if (isInList(itemEntry, OtherBOEs))
            {
                minPrice = 12500000;
                maxPrice = 25000000;
                priceOverridden = true;
            }

            // If the item was found in any list, override the sell price
            if (priceOverridden)
            {
                buyoutPrice = minPrice + (rand() % (maxPrice - minPrice + 1));
                bidPrice = buyoutPrice * (0.7 + ((double)rand() / RAND_MAX) * 0.1);
            }
        }
        
        auto trans = CharacterDatabase.BeginTransaction();

        AuctionEntry* auctionEntry      = new AuctionEntry();
        auctionEntry->Id                = sObjectMgr->GenerateAuctionID();
        auctionEntry->houseId           = AuctionHouseId(config->GetAHID());
        auctionEntry->item_guid         = item->GetGUID();
        auctionEntry->item_template     = item->GetEntry();
        auctionEntry->itemCount         = item->GetCount();
        auctionEntry->owner             = AHBplayer->GetGUID();
        auctionEntry->startbid          = bidPrice * stackCount;
        auctionEntry->buyout            = buyoutPrice * stackCount;
        auctionEntry->bid               = 0;
        auctionEntry->deposit           = dep;
        auctionEntry->expire_time       = (time_t)etime + time(NULL);
        auctionEntry->auctionHouseEntry = ahEntry;

        item->SaveToDB(trans);
        item->RemoveFromUpdateQueueOf(AHBplayer);
        sAuctionMgr->AddAItem(item);
        auctionHouse->AddAuction(auctionEntry);
        auctionEntry->SaveToDB(trans);

        CharacterDatabase.CommitTransaction(trans);

        // 
        // Increments the number of items presents in the auction
        // 

        switch (choice)
        {
        case 0:
            ++greyItems;
            break;

        case 1:
            ++whiteItems;
            break;

        case 2:
            ++greenItems;
            break;

        case 3:
            ++blueItems;
            break;

        case 4:
            ++purpleItems;
            break;

        case 5:
            ++orangeItems;
            break;

        case 6:
            ++yellowItems;
            break;

        case 7:
            ++greyTGoods;
            break;

        case 8:
            ++whiteTGoods;
            break;

        case 9:
            ++greenTGoods;
            break;

        case 10:
            ++blueTGoods;
            break;

        case 11:
            ++purpleTGoods;
            break;

        case 12:
            ++orangeTGoods;
            break;

        case 13:
            ++yellowTGoods;
            break;

        default:
            break;
        }

        noSold++;

        if (config->TraceSeller)
        {
            LOG_INFO("module", "AHBot [{}]: New stack ah={}, id={}, stack={}, bid={}, buyout={}", _id, config->GetAHID(), itemID, stackCount, auctionEntry->startbid, auctionEntry->buyout);
        }
    }

    if (config->TraceSeller)
    {
        LOG_INFO("module", "AHBot [{}]: auctionhouse {}, req={}, sold={}, aboveMin={}, aboveMax={}, loopBrk={}, noNeed={}, tooMany={}, binEmpty={}, err={}", _id, config->GetAHID(), items, noSold, aboveMin, aboveMax, loopBrk, noNeed, tooMany, binEmpty, err);
    }
}

// =============================================================================
// Perform an update cycle
// =============================================================================

void AuctionHouseBot::Update()
{
    time_t _newrun = time(NULL);

    //
    // If no configuration is associated, then stop here
    //

    if (!_allianceConfig && !_hordeConfig && !_neutralConfig)
    {
        return;
    }

    //
    // Preprare for operation
    //

    std::string accountName = "AuctionHouseBot" + std::to_string(_account);

    WorldSession _session(_account, std::move(accountName), nullptr, SEC_PLAYER, sWorld->getIntConfig(CONFIG_EXPANSION), 0, LOCALE_enUS, 0, false, false, 0);

    Player _AHBplayer(&_session);
    _AHBplayer.Initialize(_id);

    ObjectAccessor::AddObject(&_AHBplayer);

    //
    // Perform update for the factions markets
    //

    if (!sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_AUCTION))
    {
        //
        // Alliance
        //

        if (_allianceConfig)
        {
            Sell(&_AHBplayer, _allianceConfig);

            if (((_newrun - _lastrun_a_sec) >= (_allianceConfig->GetBiddingInterval() * MINUTE)) && (_allianceConfig->GetBidsPerInterval() > 0))
            {
                Buy(&_AHBplayer, _allianceConfig, &_session);
                _lastrun_a_sec = _newrun;
            }
        }

        //
        // Horde
        //

        if (_hordeConfig)
        {
            Sell(&_AHBplayer, _hordeConfig);

            if (((_newrun - _lastrun_h_sec) >= (_hordeConfig->GetBiddingInterval() * MINUTE)) && (_hordeConfig->GetBidsPerInterval() > 0))
            {
                Buy(&_AHBplayer, _hordeConfig, &_session);
                _lastrun_h_sec = _newrun;
            }
        }

    }

    //
    // Neutral
    //

    if (_neutralConfig)
    {
        Sell(&_AHBplayer, _neutralConfig);

        if (((_newrun - _lastrun_n_sec) >= (_neutralConfig->GetBiddingInterval() * MINUTE)) && (_neutralConfig->GetBidsPerInterval() > 0))
        {
            Buy(&_AHBplayer, _neutralConfig, &_session);
            _lastrun_n_sec = _newrun;
        }
    }

    ObjectAccessor::RemoveObject(&_AHBplayer);
}

// =============================================================================
// Execute commands coming from the console
// =============================================================================

void AuctionHouseBot::Commands(AHBotCommand command, uint32 ahMapID, uint32 col, char* args)
{
    //
    // Retrieve the auction house configuration
    //

    AHBConfig *config = NULL;

    switch (ahMapID)
    {
    case 2:
        config = _allianceConfig;
        break;
    case 6:
        config = _hordeConfig;
        break;
    default:
        config = _neutralConfig;
        break;
    }

    //
    // Retrive the item quality
    //

    std::string color;

    switch (col)
    {
    case AHB_GREY:
        color = "grey";
        break;
    case AHB_WHITE:
        color = "white";
        break;
    case AHB_GREEN:
        color = "green";
        break;
    case AHB_BLUE:
        color = "blue";
        break;
    case AHB_PURPLE:
        color = "purple";
        break;
    case AHB_ORANGE:
        color = "orange";
        break;
    case AHB_YELLOW:
        color = "yellow";
        break;
    default:
        break;
    }

    //
    // Perform the command
    //

    switch (command)
    {
    case AHBotCommand::buyer:
    {
        char* param1 = strtok(args, " ");
        uint32 state = (uint32)strtoul(param1, NULL, 0);

        if (state == 0)
        {
            _allianceConfig->AHBBuyer = false;
            _hordeConfig->AHBBuyer    = false;
            _neutralConfig->AHBBuyer  = false;
        }
        else
        {
            _allianceConfig->AHBBuyer = true;
            _hordeConfig->AHBBuyer    = true;
            _neutralConfig->AHBBuyer  = true;
        }

        break;
    }
    case AHBotCommand::seller:
    {
        char* param1 = strtok(args, " ");
        uint32 state = (uint32)strtoul(param1, NULL, 0);

        if (state == 0)
        {
            _allianceConfig->AHBSeller = false;
            _hordeConfig->AHBSeller    = false;
            _neutralConfig->AHBSeller  = false;
        }
        else
        {
            _allianceConfig->AHBSeller = true;
            _hordeConfig->AHBSeller    = true;
            _neutralConfig->AHBSeller  = true;
        }

        break;
    }
    case AHBotCommand::useMarketPrice:
    {
        char* param1 = strtok(args, " ");
        uint32 state = (uint32)strtoul(param1, NULL, 0);

        if (state == 0)
        {
            _allianceConfig->SellAtMarketPrice = false;
            _hordeConfig->SellAtMarketPrice    = false;
            _neutralConfig->SellAtMarketPrice  = false;
        }
        else
        {
            _allianceConfig->SellAtMarketPrice = true;
            _hordeConfig->SellAtMarketPrice    = true;
            _neutralConfig->SellAtMarketPrice  = true;
        }

        break;
    }
    case AHBotCommand::ahexpire:
    {
        AuctionHouseObject* auctionHouse = sAuctionMgr->GetAuctionsMap(config->GetAHFID());

        AuctionHouseObject::AuctionEntryMap::iterator itr;
        itr = auctionHouse->GetAuctionsBegin();

        //
        // Iterate through all the autions and if they belong to the bot, make them expired
        //

        while (itr != auctionHouse->GetAuctionsEnd())
        {
            if (itr->second->owner.GetCounter() == _id)
            {
                // Expired NOW.
                itr->second->expire_time = GameTime::GetGameTime().count();

                uint32 id                = itr->second->Id;
                uint32 expire_time       = itr->second->expire_time;

                CharacterDatabase.Execute("UPDATE auctionhouse SET time = '{}' WHERE id = '{}'", expire_time, id);
            }

            ++itr;
        }

        break;
    }
    case AHBotCommand::minitems:
    {
        char * param1   = strtok(args, " ");
        uint32 minItems = (uint32) strtoul(param1, NULL, 0);

        WorldDatabase.Execute("UPDATE mod_auctionhousebot SET minitems = '{}' WHERE auctionhouse = '{}'", minItems, ahMapID);

        config->SetMinItems(minItems);

        break;
    }
    case AHBotCommand::maxitems:
    {
        char * param1   = strtok(args, " ");
        uint32 maxItems = (uint32) strtoul(param1, NULL, 0);

        WorldDatabase.Execute("UPDATE mod_auctionhousebot SET maxitems = '{}' WHERE auctionhouse = '{}'", maxItems, ahMapID);

        config->SetMaxItems(maxItems);
        config->CalculatePercents();
        break;
    }
    case AHBotCommand::percentages:
    {
        char * param1   = strtok(args, " ");
        char * param2   = strtok(NULL, " ");
        char * param3   = strtok(NULL, " ");
        char * param4   = strtok(NULL, " ");
        char * param5   = strtok(NULL, " ");
        char * param6   = strtok(NULL, " ");
        char * param7   = strtok(NULL, " ");
        char * param8   = strtok(NULL, " ");
        char * param9   = strtok(NULL, " ");
        char * param10  = strtok(NULL, " ");
        char * param11  = strtok(NULL, " ");
        char * param12  = strtok(NULL, " ");
        char * param13  = strtok(NULL, " ");
        char * param14  = strtok(NULL, " ");

        uint32 greytg   = (uint32) strtoul(param1, NULL, 0);
        uint32 whitetg  = (uint32) strtoul(param2, NULL, 0);
        uint32 greentg  = (uint32) strtoul(param3, NULL, 0);
        uint32 bluetg   = (uint32) strtoul(param4, NULL, 0);
        uint32 purpletg = (uint32) strtoul(param5, NULL, 0);
        uint32 orangetg = (uint32) strtoul(param6, NULL, 0);
        uint32 yellowtg = (uint32) strtoul(param7, NULL, 0);
        uint32 greyi    = (uint32) strtoul(param8, NULL, 0);
        uint32 whitei   = (uint32) strtoul(param9, NULL, 0);
        uint32 greeni   = (uint32) strtoul(param10, NULL, 0);
        uint32 bluei    = (uint32) strtoul(param11, NULL, 0);
        uint32 purplei  = (uint32) strtoul(param12, NULL, 0);
        uint32 orangei  = (uint32) strtoul(param13, NULL, 0);
        uint32 yellowi  = (uint32) strtoul(param14, NULL, 0);

        //
        // Setup the percentage in the configuration first, so validity test can be performed
        //

        config->SetPercentages(greytg, whitetg, greentg, bluetg, purpletg, orangetg, yellowtg, greyi, whitei, greeni, bluei, purplei, orangei, yellowi);

        //
        // Save the results into the database (after the tests)
        //

        auto trans = WorldDatabase.BeginTransaction();

        trans->Append("UPDATE mod_auctionhousebot SET percentgreytradegoods   = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_GREY_TG)  , ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentwhitetradegoods  = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_WHITE_TG) , ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentgreentradegoods  = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_GREEN_TG) , ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentbluetradegoods   = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_BLUE_TG)  , ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentpurpletradegoods = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_PURPLE_TG), ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentorangetradegoods = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_ORANGE_TG), ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentyellowtradegoods = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_YELLOW_TG), ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentgreyitems        = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_GREY_I)   , ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentwhiteitems       = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_WHITE_I)  , ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentgreenitems       = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_GREEN_I)  , ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentblueitems        = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_BLUE_I)   , ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentpurpleitems      = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_PURPLE_I) , ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentorangeitems      = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_ORANGE_I) , ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentyellowitems      = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_YELLOW_I) , ahMapID);

        WorldDatabase.CommitTransaction(trans);

        break;
    }
    case AHBotCommand::minprice:
    {
        char * param1   = strtok(args, " ");
        uint32 minPrice = (uint32) strtoul(param1, NULL, 0);

        WorldDatabase.Execute("UPDATE mod_auctionhousebot SET minprice{} = '{}' WHERE auctionhouse = '{}'", color, minPrice, ahMapID);

        config->SetMinPrice(col, minPrice);

        break;
    }
    case AHBotCommand::maxprice:
    {
        char * param1   = strtok(args, " ");
        uint32 maxPrice = (uint32) strtoul(param1, NULL, 0);

        WorldDatabase.Execute("UPDATE mod_auctionhousebot SET maxprice{} = '{}' WHERE auctionhouse = '{}'", color, maxPrice, ahMapID);

        config->SetMaxPrice(col, maxPrice);

        break;
    }
    case AHBotCommand::minbidprice:
    {
        char * param1      = strtok(args, " ");
        uint32 minBidPrice = (uint32) strtoul(param1, NULL, 0);

        WorldDatabase.Execute("UPDATE mod_auctionhousebot SET minbidprice{} = '{}' WHERE auctionhouse = '{}'", color, minBidPrice, ahMapID);

        config->SetMinBidPrice(col, minBidPrice);

        break;
    }
    case AHBotCommand::maxbidprice:
    {
        char * param1      = strtok(args, " ");
        uint32 maxBidPrice = (uint32) strtoul(param1, NULL, 0);

        WorldDatabase.Execute("UPDATE mod_auctionhousebot SET maxbidprice{} = '{}' WHERE auctionhouse = '{}'", color, maxBidPrice, ahMapID);

        config->SetMaxBidPrice(col, maxBidPrice);

        break;
    }
    case AHBotCommand::maxstack:
    {
        char * param1   = strtok(args, " ");
        uint32 maxStack = (uint32) strtoul(param1, NULL, 0);

        WorldDatabase.Execute("UPDATE mod_auctionhousebot SET maxstack{} = '{}' WHERE auctionhouse = '{}'", color, maxStack, ahMapID);

        config->SetMaxStack(col, maxStack);

        break;
    }
    case AHBotCommand::buyerprice:
    {
        char * param1     = strtok(args, " ");
        uint32 buyerPrice = (uint32) strtoul(param1, NULL, 0);

        WorldDatabase.Execute("UPDATE mod_auctionhousebot SET buyerprice{} = '{}' WHERE auctionhouse = '{}'", color, buyerPrice, ahMapID);

        config->SetBuyerPrice(col, buyerPrice);

        break;
    }
    case AHBotCommand::bidinterval:
    {
        char * param1      = strtok(args, " ");
        uint32 bidInterval = (uint32) strtoul(param1, NULL, 0);

        WorldDatabase.Execute("UPDATE mod_auctionhousebot SET buyerbiddinginterval = '{}' WHERE auctionhouse = '{}'", bidInterval, ahMapID);

        config->SetBiddingInterval(bidInterval);

        break;
    }
    case AHBotCommand::bidsperinterval:
    {
        char * param1          = strtok(args, " ");
        uint32 bidsPerInterval = (uint32) strtoul(param1, NULL, 0);

        WorldDatabase.Execute("UPDATE mod_auctionhousebot SET buyerbidsperinterval = '{}' WHERE auctionhouse = '{}'", bidsPerInterval, ahMapID);

        config->SetBidsPerInterval(bidsPerInterval);

        break;
    }
    default:
        break;
    }
}

// =============================================================================
// Initialization of the bot
// =============================================================================

void AuctionHouseBot::Initialize(AHBConfig* allianceConfig, AHBConfig* hordeConfig, AHBConfig* neutralConfig)
{
    // 
    // Save the pointer for the configurations
    // 

    _allianceConfig = allianceConfig;
    _hordeConfig    = hordeConfig;
    _neutralConfig  = neutralConfig;

    //
    // Done
    //

    LOG_INFO("module", "AHBot [{}]: initialization complete", uint32(_id));
}
