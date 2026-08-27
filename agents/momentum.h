#pragma once
#include <cstdint>
#include <random>
#include <vector>
#include <deque>
#include <cmath>
#include "events.h"
#include "config.h"
#include "base_lob_engine.h"

struct MomentumTrader {
    uint32_t index = 0;
    uint32_t trailing_price = 0;
    uint64_t last_action_time = 0;

    // add agent clock
    uint64_t agent_clock = 0 ; 

    // Position tracking to prevent death spirals
    int32_t inventory = 0;
    int32_t max_position = 500; // Stop buying/selling after accumulating +-500 shares
    uint64_t active_order_id = 0;
    char last_side = '0' ;
    
    // Latencies: Slower than HFT MMs (Mean ~50us)
    uint64_t l1_ns = 50000;
    uint64_t l2_ns = 50000;

    double trigger_threshold = 0.002;
};

inline void initialize_momentum(std::vector<MomentumTrader>& mom_pool, std::mt19937& gen) {
    std::normal_distribution<double> dist_l1(50000.0, 5000.0);
    std::normal_distribution<double> dist_l2(50000.0, 5000.0);
    // Randomize thresholds between 0.001 (0.1%) and 0.006 (0.6%)
    std::uniform_real_distribution<double> dist_thresh(0.001, 0.006);

    for (size_t i = 0; i < mom_pool.size(); ++i) {
        mom_pool[i].index = i;
        mom_pool[i].l1_ns = static_cast<uint64_t>(std::max(10000.0, dist_l1(gen)));
        mom_pool[i].l2_ns = static_cast<uint64_t>(std::max(10000.0, dist_l2(gen)));
        mom_pool[i].trigger_threshold = dist_thresh(gen);
    }
}

inline void momentum_react(
    MomentumTrader& mom,
    const Event& event,
    uint32_t current_price,
    std::deque<Event>& q,
    std::mt19937& gen,
    uint64_t& seq,
    uint64_t& ord_id,
    uint64_t tick_size
) {
    // 1. INVENTORY TRACKING: Match side explicitly
    if (event.msg_type == MsgType::Fill && event.p.fill.order_id == mom.active_order_id) {
        if (mom.last_side == 'B') mom.inventory += static_cast<int32_t>(event.p.fill.fill_shares);
        else if (mom.last_side == 'S') mom.inventory -= static_cast<int32_t>(event.p.fill.fill_shares);
    }

    // only evaluate on ITCH Execution events (ignore cancels/adds)
    if (event.msg_type != MsgType::OrderExec) return;

    // initialize trailing price on first trade
    if (mom.trailing_price == 0) {
        mom.trailing_price = current_price;
        return;
    }

    // cooldown: Momentum traders don't trade more than once every 100ms (100,000,000 ns)
    if ( (event.timestamp + mom.l1_ns) - mom.last_action_time < 100000000) return;

    // calculate fractional return against trailing price
    double ret = static_cast<double>(static_cast<int32_t>(current_price) - static_cast<int32_t>(mom.trailing_price)) / mom.trailing_price;

    // threshold: +0.05% triggers BUY, -0.05% triggers SELL
    char side = '0';
    if (ret > mom.trigger_threshold) {
        side = 'B';
    } else if (ret < -mom.trigger_threshold) {
        side = 'S';
    } else {
        return; // Trend not strong enough to trade
    }

    // POSITION LIMIT CHECK: If we are already long 500 shares, don't buy more! If short 500, don't sell more!
    if (side == 'B' && mom.inventory >= mom.max_position) return;
    if (side == 'S' && mom.inventory <= -mom.max_position) return;

    // calculate arrival time with jitter
    std::uniform_int_distribution<int> jitter_dist(-1000, 1000);
    uint64_t hit_time = event.timestamp + mom.l1_ns + mom.l2_ns + jitter_dist(gen);

    // Cap aggressiveness at 10 ticks away from current price to prevent vacuuming the book to infinity/zero
    uint32_t collar = 10 * static_cast<uint32_t>(tick_size);

    uint32_t agg_price = current_price ; // greater then max allowed price is market order.
    
    if (side == 'B') {
        agg_price = current_price + collar;
    } else {
        agg_price = (current_price > collar) ? (current_price - collar) : tick_size;
    }

    auto locate = static_cast<uint16_t>(event.stock_locate) ;
    mom.active_order_id = ord_id++;
    mom.last_side = side;

    EnterOrder req{mom.active_order_id , agg_price, 100, side, 0} ; 

    mom.last_action_time = event.timestamp + mom.l1_ns;
    mom.trailing_price = current_price; // Reset reference price

    q.push_back(Event{hit_time, seq++, event.sequence_num, EventType::OUCH, MsgType::EnterOrder, locate,
        {mom.index, AgentTier::MOM}, req});
}