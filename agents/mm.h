#pragma once
#include <cstdint>
#include <random>
#include <vector>
#include <deque>
#include <cmath>
#include <algorithm>
#include "events.h"
#include "config.h"

inline uint32_t global_fair_value = 1000000; // $100.00 starting center of gravity
inline uint64_t last_global_drift = 0;

struct MM {
    uint32_t index = 0;
    int32_t  inventory = 0;
    uint64_t last_quote_time = 0;
    
    // Track the last price we quoted so we can implement Lazy Quoting
    uint32_t last_r_price = 0;

    // add agent clock
    uint64_t agent_clock = 0 ; 
    
    std::vector<std::pair<uint64_t, char>> active_orders;
    
    uint64_t l1_ns = 1500;
    uint64_t l2_ns = 1500;
};

inline void initialize_mm(std::vector<MM>& mm_pool, std::mt19937& gen) {
    std::normal_distribution<double> dist_l1(1500.0, 200.0);
    std::normal_distribution<double> dist_l2(1500.0, 200.0);

    for (size_t i = 0; i < mm_pool.size(); ++i) {
        mm_pool[i].index = i;
        mm_pool[i].l1_ns = static_cast<uint64_t>(std::max(500.0, dist_l1(gen)));
        mm_pool[i].l2_ns = static_cast<uint64_t>(std::max(500.0, dist_l2(gen)));
    }
}

inline void mm_react(
    MM& mm,
    const Event& itch_event,
    uint32_t mid_price,
    std::deque<Event>& q,
    std::mt19937& gen,
    uint64_t& seq,
    uint64_t& ord_id,
    uint64_t tick_size
) {
    // 1. INVENTORY TRACKING: Update inventory when our resting orders fill
    if (itch_event.msg_type == MsgType::OrderExec) {
        auto exec_id = itch_event.p.itch_execute.order_id;
        auto exec_shares = itch_event.p.itch_execute.executed_shares;
        
        for (auto it = mm.active_orders.begin(); it != mm.active_orders.end(); ) {
            if (it->first == exec_id) {
                if (it->second == 'B') mm.inventory += static_cast<int32_t>(exec_shares);
                else mm.inventory -= static_cast<int32_t>(exec_shares);
                it = mm.active_orders.erase(it);
            } else {
                ++it;
            }
        }
    }

    // 2. Cooldown Check: 5ms minimum evaluation interval
    if (itch_event.timestamp - mm.last_quote_time < 5000000) return;
   
    // 3. EXOGENOUS FUNDAMENTAL RANDOM WALK (Academic Standard: Wah & Wellman / ABIDES)
    // External macro news arrives every 500ms (500,000,000 ns), shifting true fair value by +-1 tick.
    // All MMs share this public valuation so they don't arbitrage and cross-quote against each other.
    if (itch_event.timestamp - last_global_drift >= 500000000) {
        std::uniform_int_distribution<int> drift_dist(-1, 1);
        int32_t jump_ticks = drift_dist(gen) * static_cast<int32_t>(tick_size);
        // Clamp between $80.00 and $120.00 to keep the stock within realistic daily boundaries
        global_fair_value = static_cast<uint32_t>(std::clamp(static_cast<int32_t>(global_fair_value) + jump_ticks, 800000, 1200000));
        last_global_drift = itch_event.timestamp;
    }

    double alpha = 0.05;
    double skew_ticks = -(alpha * mm.inventory);
    int32_t int_skew = static_cast<int32_t>(std::round(skew_ticks)) * static_cast<int32_t>(tick_size);

    // Anchor individual reservation price R to the drifting global fair value
    uint32_t r_price = static_cast<uint32_t>(std::max(10000, static_cast<int32_t>(global_fair_value) + int_skew));

    
    // 4. LAZY QUOTING THRESHOLD: Do not cancel resting orders unless the reservation price
    // has shifted by at least 3 ticks (300 scaled units). This stops cancellation voids!
    if (!mm.active_orders.empty() && 
        std::abs(static_cast<int32_t>(r_price) - static_cast<int32_t>(mm.last_r_price)) < (3 * static_cast<int32_t>(tick_size))) {
        return;
    }

    std::uniform_int_distribution<int> jitter_dist(-50, 50);
    int64_t l2_jitter = jitter_dist(gen);
    uint64_t actual_l2 = static_cast<uint64_t>(std::max(100L, static_cast<int64_t>(mm.l2_ns) + l2_jitter));
    uint64_t hit_time = itch_event.timestamp + mm.l1_ns + actual_l2;

    auto locate = static_cast<uint16_t>(Symbol::AAPL);

    // 5. Cancel old quotes only when necessary
    for (const auto& pair : mm.active_orders) {
        CancelReq c_req{pair.first, 0};
        q.push_back(Event{hit_time, seq++, 0, EventType::OUCH, MsgType::CancelReq, locate,
            {mm.index, AgentTier::MM}, c_req});
    }
    mm.active_orders.clear();

    int32_t max_inventory = 1000;
    bool quote_bid = (mm.inventory < max_inventory);
    bool quote_ask = (mm.inventory > -max_inventory);

    // Quote 2 levels deep for shock absorption around our fundamental anchor
    uint32_t bid_p1 = r_price - static_cast<uint32_t>(tick_size);
    uint32_t ask_p1 = r_price + static_cast<uint32_t>(tick_size);
    uint32_t bid_p2 = (r_price > 4 * tick_size) ? (r_price - 4 * static_cast<uint32_t>(tick_size)) : tick_size;
    uint32_t ask_p2 = r_price + 4 * static_cast<uint32_t>(tick_size);

    mm.last_quote_time = hit_time;
    mm.last_r_price = r_price;
    
    if (quote_bid) {
        uint64_t b1 = ord_id++;
        uint64_t b2 = ord_id++;
        mm.active_orders.push_back({b1, 'B'});
        mm.active_orders.push_back({b2, 'B'});
        q.push_back(Event{hit_time + 1, seq++, itch_event.sequence_num, EventType::OUCH, MsgType::EnterOrder, locate,
            {mm.index, AgentTier::MM}, {EnterOrder{b1, bid_p1, 200, 'B', 0}}});
        q.push_back(Event{hit_time + 2, seq++, itch_event.sequence_num, EventType::OUCH, MsgType::EnterOrder, locate,
            {mm.index, AgentTier::MM}, {EnterOrder{b2, bid_p2, 400, 'B', 0}}});
    }

    if (quote_ask) {
        uint64_t a1 = ord_id++;
        uint64_t a2 = ord_id++;
        mm.active_orders.push_back({a1, 'S'});
        mm.active_orders.push_back({a2, 'S'});
        q.push_back(Event{hit_time + 3, seq++, itch_event.sequence_num, EventType::OUCH, MsgType::EnterOrder, locate,
            { mm.index, AgentTier::MM}, {EnterOrder{a1, ask_p1, 200, 'S', 0}}});
        q.push_back(Event{hit_time + 4, seq++, itch_event.sequence_num, EventType::OUCH, MsgType::EnterOrder, locate,
            { mm.index, AgentTier::MM}, {EnterOrder{a2, ask_p2, 400, 'S', 0}}});
    }
}