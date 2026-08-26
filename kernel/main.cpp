// kernel/main.cpp

#include <iostream>
#include <deque>
#include "sim_logger.h"
#include "base_lob_engine.h"
#include "events.h"
#include "market_state.h"
#include "config.h"
#include "custom_priority_queue.h"
#include "market_state.h"     
#include "broker_snapshot.h"

#include "mm.h"
#include "momentum.h"
#include "zi.h"
#include <random>
#include <boost/unordered/unordered_flat_map.hpp>

#include <chrono>

// ============================================================
// GLOBALS
// ============================================================
uint64_t seq_number         = 0;
uint64_t available_order_id = 1000000;


// std::vector<HFTAgent>            hft_pool;
// std::vector<RetailAgent>         retail_pool;
// std::vector<FundamentalistAgent> fund_pool;
// std::vector<InstitutionalAgent>  inst_pool;

struct Agent_info{
    AgentTier tier = AgentTier::ZI ;
    uint32_t idx = 0 ;
} ;

// ============================================================
// push bootstrap limit orders to seed the book
// Kernel pushes these as OUCH events at sim start so LOB has
// initial bid/ask before agents start trading
// ============================================================
void push_bootstrap_orders(Engine<EngineMode::Simulation>& engine , CustomPriorityQueue& gsq)
{
    Event e ;
    e.timestamp = Config::PRE_MARKET_NS + 1 ; 
    e.causal_parent_id = 0 ;    
    
    for (int l=0; l<MAX_TICKERS ; l++) {    
        // locate code
        auto locate = static_cast<uint16_t>(l) ;
        // tick size
        auto tick = engine.books[locate].TICK_SIZE ;
        uint32_t bid = Config::BOOTSTRAP_BID ;
        
        // Bid side : several levels below fair price
        for (int i = 0; i < 5; i++, bid -= tick) {
            
            e.timestamp += 1 ;
            e.sequence_num = seq_number++ ;
            e.event_type  = EventType::OUCH ;

            e.stock_locate = locate ;
            e.p.order_req = {available_order_id++ , bid , Config::BOOTSTRAP_QTY , 'B' , 0 } ;
            gsq.push(e);
        }

        // Ask side
        uint32_t ask = Config::BOOTSTRAP_ASK ;
        for (int i = 0; i < 5; i++, ask += tick) {

            e.timestamp += 1 ;
            e.sequence_num = seq_number++  ;
            e.event_type = EventType::OUCH ;
            
            e.stock_locate = locate ;
            e.p.order_req = {available_order_id++ , ask , Config::BOOTSTRAP_QTY , 'S' , 0 } ;
            
            gsq.push(e);
        }
    }
}


// ============================================================
// MAIN
// ============================================================
int main()
{
    auto Global_SQ = new CustomPriorityQueue();
    // exchange matching engine
    Engine<EngineMode::Simulation> engine ;
    // kernel maintained shadow lob parser engine
    Engine<EngineMode::Parser> kernel_parser_engine ;

    // for agents with limited info
    BrokerSnapshot broker_snap;

    //CSV logger
    SimLogger logger;

    // seed the book with initial orders so HFTs have something to trade against
    push_bootstrap_orders(engine , *Global_SQ);

    // for every order submited by agent , he has to register order id and agent info.
    // order id : { agent tier , index} map
    // boost::unordered_flat_map<uint64_t, Agent_info> order_id_to_agent_map ;

    std::deque<Event> reaction_queue;
    std::deque<Event> feed_hq;

    uint32_t lamda = Config::order_rate / Config::total_zi ; 
    uint32_t theta = 1000 ; // using L = lam*W . 400 = 5*w .  L = total order in book at any momemt.
    // w == average lifespan of a order in sec. theta was once 0.0001 also , once it was 0.01 also once it was 0.1 also , it has been tested . might have some issues .

    std::vector<MM> mm_pool(20) ;
    std::vector<MomentumTrader> mom_pool(5) ;

    std::vector<ZI> zi_pool(Config::total_zi);

    mt19937 gen(Config::MASTER_SEED); // 42.
    std::normal_distribution<double> price_dist(0,10.0);
    exponential_distribution<double> wait_dist(lamda)  ;

    exponential_distribution<double> cancel_dist(theta) ; 

    initialize_zi(zi_pool, reaction_queue , wait_dist, gen, seq_number);

    initialize_mm(mm_pool, gen);
    initialize_momentum(mom_pool, gen);

    for(auto r: reaction_queue)
    {
        Global_SQ->push(r);
    }
    reaction_queue.clear();


    // --- PROGRESS BAR SETUP ---
    uint64_t events_processed = 0;
    uint64_t total_sim_time = Config::MARKET_CLOSE_NS - Config::MARKET_OPEN_NS;
    std::cout << "\nStarting Simulation Engine...\n";


    auto start = std::chrono::high_resolution_clock::now() ;

    // ============================================================
    // MAIN LOOP
    // ============================================================
    while (!Global_SQ->empty())
    {
        Event event = Global_SQ->pop();

        // --- PROGRESS BAR LOGIC ---
        events_processed++;
        if (events_processed % 100000 == 0) {
            double progress = 0.0;
            if (event.timestamp >= Config::MARKET_OPEN_NS) {
                progress = (double)(event.timestamp - Config::MARKET_OPEN_NS) / total_sim_time * 100.0;
            }
            
            // \r overwrites the line. std::flush forces it to draw instantly.
            std::cout << "\r[ENGINE RUNNING] LOB Time: " << event.timestamp 
                      << " ns | Progress: " << progress << "% "
                      << "| Events: " << events_processed 
                      << std::flush;
        }

        

        if (event.timestamp == 0 && event.sequence_num == 0) break;

        // stop at market close
        if (event.timestamp > Config::MARKET_CLOSE_NS) break;

        // std::deque<Event> reaction_queue;
        // std::deque<Event> feed_hq;

        auto& parser_lob = kernel_parser_engine.books[event.stock_locate] ;
        auto& state = parser_lob.state ; 

        switch (event.event_type)
        {
            // ------------------------------------------------
            // MODE 2  ITCH: update market state, iterate agents
            // ------------------------------------------------
            case EventType::ITCH:
            {
            
                // update market state from this ITCH
                reconstruct_market_state(kernel_parser_engine , event);

                update_broker_snapshot(broker_snap, parser_lob);
                // see here we use event.timestamp not lob.clock , as the market state is updated according to when itch comes , so that is the reason
                logger.maybe_log(parser_lob, event.timestamp) ;
                
                // schedule agent reactions : (MM & MOM)
                for (auto& mm: mm_pool){
                    if (mm.agent_clock <= event.timestamp + mm.l1_ns){
                        // update agent_clock
                        mm.agent_clock = event.timestamp + mm.l1_ns ;
                    
                        mm_react(mm, event, state.mid_price, reaction_queue, gen, seq_number,
                            available_order_id, parser_lob.TICK_SIZE) ;
                    }
                    // otherwise skip.
                }
                for (auto& mom: mom_pool){
                    if (mom.agent_clock <= event.timestamp + mom.l1_ns){
                        // update agent_clock
                        mom.agent_clock = event.timestamp + mom.l1_ns ;
                    
                        momentum_react(mom, event, state.last_trade_price, reaction_queue, gen, seq_number, 
                            available_order_id, parser_lob.TICK_SIZE) ;
                    }
                    // otherwise skip.
                }
                
                break;
            }

            // ------------------------------------------------
            // MODE 1  OUCH: call matching engine
            // ------------------------------------------------
            case EventType::OUCH:
            {
                engine.process_ouch_request(event, feed_hq, seq_number);
                break;
            }

            // ------------------------------------------------
            // MODE 3  SPECIFIC_OUCH: update one agent, let him react
            // ------------------------------------------------
            case EventType::S_OUCH:
            {
                // pending work: 
                auto tier = event.agent.tier ;

                if(tier == AgentTier::MM){
                    auto mm = mm_pool[event.agent.index] ;
                    if (mm.agent_clock <= event.timestamp + mm.l1_ns){
                        // update agent_clock
                        mm.agent_clock = event.timestamp + mm.l1_ns ;
                    
                        mm_react(mm, event, state.mid_price, reaction_queue, gen, seq_number,
                            available_order_id, parser_lob.TICK_SIZE) ;
                    }
                }
                else if (tier == AgentTier::MOM){
                    auto mom = mom_pool[event.agent.index] ;
                    if (mom.agent_clock <= event.timestamp + mom.l1_ns){
                        // update agent_clock
                        mom.agent_clock = event.timestamp + mom.l1_ns ;
                    
                        momentum_react(mom, event, state.last_trade_price, reaction_queue, gen, seq_number, 
                            available_order_id, parser_lob.TICK_SIZE) ;
                    }
                }
                else if (tier == AgentTier::ZI){
                    auto zi = zi_pool[event.agent.index] ;
                    // well zi doesn't care about this private messsages , so skip...
                }


                // agent reacts
                // if (agent.agent_clock <= event.timestamp + agent.l1_ns){
                //     agent.agent_clock = event.timestamp + agent.l1_ns ;
                //     // agent react function according to agent type
                // }

                if (!reaction_queue.empty()) {
                    // uint64_t l1 = get_agent_l1(tier, index);
                    // uint64_t l2 = get_agent_l2(tier, index);
                    // uint64_t rt = event.timestamp + l1 + l2;
                    for (auto& req : reaction_queue) {
                        // req.timestamp    = rt;
                        // req.sequence_num = seq_number++;
                        Global_SQ->push(req);
                    }
                    reaction_queue.clear();
                }
                break;
            }
            
            //--------------------------------------------
            //MODE 4 - AgentWakeUP - submit reaction , go to sleep.
            //--------------------------------------------
            case EventType::AgentWakeUP:
            {
                auto t = event.timestamp ;
                // agent react 
                zi_react( zi_pool[event.p.wake_up.index] , 
                    state.mid_price, t ,reaction_queue, 
                    price_dist , cancel_dist , gen , seq_number, available_order_id, parser_lob.TICK_SIZE );
    
                // schedule next wake up
                schedule_zi_wake_up(zi_pool[event.p.wake_up.index], t, reaction_queue,
                                wait_dist, gen , seq_number);

                break ;
            }
        }


        // schedule ITCH agent reactions
        if (!reaction_queue.empty()) {
            for (auto r : reaction_queue) {
                Global_SQ->push(r);
            }
            reaction_queue.clear();
        }
        // Mode 1 output: engine events go straight to queue
        if (!feed_hq.empty()) {
            for (auto f : feed_hq) {
                Global_SQ->push(f);
            }
            feed_hq.clear();
        }
    }

    //performence 
    auto end = std::chrono::high_resolution_clock::now() ;
    std::chrono::duration<double> d = end - start ;

    std::cout << "\n" << std::endl ;
    std::cout << "Total Events: " << events_processed << std::endl ;
    std::cout << "Total Time: " << d << std::endl ;
    std::cout << "Total ZI agents: " << Config::total_zi << std::endl ;
    std::cout << "Total MMakersagents: " << mm_pool.size() << std::endl ;
    std::cout << "Total Momentum agents: " << mom_pool.size() << std::endl ;
    std::cout << "Simulation Duration(in minutes): " << ((Config::MARKET_CLOSE_NS - Config::MARKET_OPEN_NS )/60000000000) << std::endl ;

    std::cout << "ZI orders per sec: " << Config::order_rate << std::endl ;

    std::cout << "Events/sec: " << events_processed/d.count() << std::endl ;

    std::cout << "\nSimulation complete. Output: sim_output.csv , lob_depth.csv\n";

    delete Global_SQ;

    return 0;
}