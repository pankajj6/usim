#pragma once
#include<cstdint>
#include <random>
#include <vector>
#include <deque>
#include "events.h"
#include "config.h"
using namespace std;

struct ZI {
    uint32_t lamda = Config::order_rate / Config::total_zi ; // rate : average react per sec
    AgentTier tier = AgentTier::ZI;
    uint32_t index = 0 ;
    char side = 'B';

    // add agent clock
    uint64_t agent_clock = 0 ; 

} ;

void schedule_zi_wake_up
    ( ZI&  zi , uint64_t t ,deque<Event>& q, 
    exponential_distribution<double>& dist , mt19937 &gen, uint64_t &seq)
{
    // for(auto& zi: zi_pool)
    // {
        auto wait = dist(gen); // in sec . as rate is in sec.
        uint64_t wait_ns = wait*1000000000 ;
        AgentWakePayload p = {0 , zi.index, AgentTier::ZI};

        auto stock_locate = static_cast<uint16_t>(Symbol::AAPL);

        Event e = { t + wait_ns , seq++ , 0, EventType::AgentWakeUP, MsgType::WakeUp, stock_locate,
                    {zi.index, AgentTier::ZI}, p};

        q.push_back(e);
    // }

} 

void initialize_zi
    ( vector<ZI>& zi_pool , deque<Event>& q,  
    exponential_distribution<double>& dist ,mt19937 &gen, uint64_t &seq)
{
    // i have to do sizes : but agent gets it out when he is submiting his order , 
    // not like pre-set , so each agent can send any size order , not fixed roles.
    uint64_t t = Config::MARKET_OPEN_NS ; // initial base time for wake up scheduling.
    for(int i=0; i< zi_pool.size(); i++)
    {
        zi_pool[i].index = i; 
        if(i%2 == 0)
        {
            zi_pool[i].side = 'S' ;
        }
        schedule_zi_wake_up(zi_pool[i], t , q , dist, gen , seq);
    }

}



void zi_react
    (ZI& zi, uint32_t mid_price , 
    uint64_t t, deque<Event>& q ,
    std::normal_distribution<double>& price_dist, 
    exponential_distribution<double>& cancel_dist,
    mt19937& gen , uint64_t& seq , uint64_t& ord_id, uint64_t tick_size
    )
{
    double fluid_ticks = price_dist(gen);

    // 3. Round to the nearest whole integer tick to avoid illegal in-between prices.
    // 3.42 becomes 3 ticks. -1.7nb becomes -2 ticks.
    int32_t dis_ticks = static_cast<int32_t>(std::round(fluid_ticks));
    // 
    // auto dis_ticks = price_dist(gen); // gives int

    int32_t offset = dis_ticks*tick_size ;

    uint32_t mid = mid_price ;
    if(mid_price == 0){
        mid = 1000000 ;
    }

    auto stock_locate = static_cast<uint16_t>(Symbol::AAPL);

    uint32_t limit_price = static_cast<uint32_t>( static_cast<int32_t>(mid)+offset ); 
    uint32_t shares = 100; // later a dist
    
    auto order_id = ord_id++;
    
    std::uniform_real_distribution<double> num_dist(0.0,1.0) ;
    auto ran = num_dist(gen);

    auto side = 'S' ;
    if(ran >= 0.5){
        side = 'B';
    }


    EnterOrder p = {order_id, limit_price, shares, side , 0 } ;
    Event e = {t + 50, seq++, 0 , EventType::OUCH, MsgType::EnterOrder,stock_locate,
                {zi.index, AgentTier::ZI}, p } ;

    // cancellation logic: for same order.
    Event e2 ; 
    uint64_t t_ns ;

    auto t_sec = cancel_dist(gen);
    t_ns = t_sec*1000000000 ;

    CancelReq p1 = {order_id, 0 }; // 0 = reduce the order to 0 shares.

    e2 = {e.timestamp+t_ns , seq++ , 0, EventType::OUCH, MsgType::CancelReq , stock_locate ,
          {zi.index, AgentTier::ZI}, p1 };

    q.push_back(e) ; // push event.
    q.push_back(e2);
}

// call in main , make zi pool,  then first call initalise zi and then schedule zi , before while loop . 
// then add a switch case , AgentWakeUP event type , case , 
//agent submit a reaction , using that logic of lob mid price distribution , goes to sleep for waiting time.
// submit his reaction to reaction queue , l1+l2 , fixed for all zi s. then submit agent wake payload
// use schedule zi wake up function , but in loop , so it can be general . done . now initialze function do it.