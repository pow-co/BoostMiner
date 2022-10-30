#include <gigamonkey/script/typed_data_bip_276.hpp>
#include <gigamonkey/redeem.hpp>
#include <sv/uint256.h>
#include <miner.hpp>
#include <logger.hpp>

namespace BoostPOW {
    using uint256 = Gigamonkey::uint256;

    // A cpu miner function. 
    work::proof cpu_solve(const work::puzzle& p, const work::solution& initial, double max_time_seconds) {
        
        uint32 initial_time = initial.Share.Timestamp.Value;
        uint32 local_initial_time = Bitcoin::timestamp::now().Value;
        
        uint64_big extra_nonce_2; 
        std::copy(initial.Share.ExtraNonce2.begin(), initial.Share.ExtraNonce2.end(), extra_nonce_2.begin());
        
        uint256 target = p.Candidate.Target.expand();
        if (target == 0) return {};
        
        N total_hashes{0};
        N nonce_increment{"0x0100000000"};
        uint32 display_increment = 0x00800000;
        
        work::proof pr{p, initial};
        
        uint32 begin{Bitcoin::timestamp::now()};
        
        while(true) {
            uint256 hash = pr.string().hash();
            total_hashes++;
            
            if (pr.Solution.Share.Nonce % display_increment == 0) {
                pr.Solution.Share.Timestamp.Value = initial_time + uint32(Bitcoin::timestamp::now().Value - local_initial_time);
                
                if (uint32(pr.Solution.Share.Timestamp) - begin > max_time_seconds) return {};
            }
            
            if (hash < target) {
                return pr;
            }
            
            pr.Solution.Share.Nonce++;
            
            if (pr.Solution.Share.Nonce == 0) {
                extra_nonce_2++;
                std::copy(extra_nonce_2.begin(), extra_nonce_2.end(), pr.Solution.Share.ExtraNonce2.begin());
            }
        }
        
        return pr;
    }
    
    std::pair<digest256, Boost::candidate> select(random &r, const jobs &j, double minimum_profitability) {
        
        if (j.size() == 0) return {};
        
        double total_profitability = 0;
        for (const std::pair<digest256, Boost::candidate> &p : j) if (p.second.profitability() > minimum_profitability) 
            total_profitability += (p.second.profitability() - minimum_profitability);
        
        double random = r.range01() * total_profitability;
        
        double accumulated_profitability = 0;
        for (const std::pair<digest256, Boost::candidate> &p : j) if (p.second.profitability() > minimum_profitability) {
            accumulated_profitability += (p.second.profitability() - minimum_profitability);
            
            if (accumulated_profitability >= random) return p;
        }
        
        // shouldn't happen. 
        return {};
    }
    
    Bitcoin::satoshi calculate_fee(
        size_t inputs_size, 
        size_t pay_script_size, 
        double fee_rate) {
        
        return inputs_size                              // inputs
            + 4                                         // tx version
            + 1                                         // var int value 1 (number of outputs)
            + 8                                         // satoshi value size
            + Bitcoin::var_int::size(pay_script_size)   // size of output script size
            + pay_script_size                           // output script size
            + 4;                                        // locktime

    }
    
    JSON solution_to_JSON(work::solution x) {
        
        JSON share {
            {"timestamp", data::encoding::hex::write(x.Share.Timestamp.Value)},
            {"nonce", data::encoding::hex::write(x.Share.Nonce)},
            {"extra_nonce_2", data::encoding::hex::write(x.Share.ExtraNonce2) }
        };
        
        if (x.Share.Bits) share["bits"] = data::encoding::hex::write(*x.Share.Bits);
        
        return JSON {
            {"share", share}, 
            {"extra_nonce_1", data::encoding::hex::write(x.ExtraNonce1)}
        };
    }
    
    work::proof solve(random &r, const work::puzzle& p, double max_time_seconds) {
        
        Stratum::session_id extra_nonce_1{r.uint32()};
        uint64_big extra_nonce_2{r.uint64()};
        
        work::solution initial{Bitcoin::timestamp::now(), 0, bytes_view(extra_nonce_2), extra_nonce_1};
        
        if (p.Mask != -1) initial.Share.Bits = r.uint32();
        
        return cpu_solve(p, initial, max_time_seconds);
        
    }
    
    string write(const Bitcoin::txid &txid) {
        std::stringstream txid_stream;
        txid_stream << txid;
        string txid_string = txid_stream.str();
        if (txid_string.size() < 73) throw string {"warning: txid string was "} + txid_string;
        return txid_string.substr(7, 66);
    }
    
    string write(const Bitcoin::outpoint &o) {
        std::stringstream ss;
        ss << write(o.Digest) << ":" << o.Index;
        return ss.str();
    }
    
    Bitcoin::transaction redeem_puzzle(const Boost::puzzle &puzzle, const work::solution &solution, list<Bitcoin::output> pay) {
        bytes redeem_tx = puzzle.redeem(solution, pay);
        if (redeem_tx == bytes{}) return {};
        
        Bitcoin::transaction redeem{redeem_tx};
        
        Bitcoin::txid redeem_txid = redeem.id();
        
        std::cout << "redeem tx generated: " << redeem_tx << std::endl;
        
        for (const Bitcoin::input &in : redeem.Inputs) {
            
            bytes redeem_script = in.Script;
            
            std::string redeemhex = data::encoding::hex::write(redeem_script);
            
            logger::log("job.complete.redeemscript", JSON {
                {"solution", solution_to_JSON(solution)},
                {"asm", Bitcoin::ASM(redeem_script)},
                {"hex", redeemhex},
                {"txid", write(redeem_txid)}
            });
        }

        // the transaction 
        return redeem;
    }
    
    Bitcoin::transaction mine(
        random &r, 
        // an unredeemed Boost PoW output 
        const Boost::puzzle &puzzle, 
        // the address you want the bitcoins to go to once you have redeemed the boost output.
        // this is not the same as 'miner address'. This is just an address in your 
        // normal wallet and should not be the address that goes along with the key above.
        const Bitcoin::address &address, 
        double fee_rate, 
        double max_time_seconds, 
        double minimum_price_per_difficulty_sats, 
        double maximum_mining_difficulty) {
        
        using namespace Bitcoin;
        std::cout << "mining on script " << puzzle.id() << std::endl;
        if (!puzzle.valid()) throw string{"Boost puzzle is not valid"};
        
        Bitcoin::satoshi value = puzzle.Value;
        
        std::cout << "difficulty is " << puzzle.difficulty() << "." << std::endl;
        std::cout << "price per difficulty is " << puzzle.profitability() << "." << std::endl;
        
        // is the difficulty too high?
        if (maximum_mining_difficulty > 0, puzzle.difficulty() > maximum_mining_difficulty) 
            std::cout << "warning: difficulty " << puzzle.difficulty() << " may be too high for CPU mining." << std::endl;
        
        // is the value in the output high enough? 
        if (puzzle.profitability() < minimum_price_per_difficulty_sats)
            std::cout << "warning: price per difficulty " << puzzle.profitability() << " may be too low." << std::endl;
        
        work::proof proof = solve(r, work::puzzle(puzzle), max_time_seconds);
        if (!proof.valid()) return {};
        
        bytes pay_script = pay_to_address::script(address.Digest);
        
        Bitcoin::satoshi fee = calculate_fee(puzzle.expected_size(), pay_script.size(), fee_rate);
        
        if (fee > value) throw string{"Cannot pay tx fee with boost output"};
        
        return redeem_puzzle(puzzle, proof.Solution, {output{value - fee, pay_script}});
        
    }
    
    JSON to_JSON(const Boost::candidate::prevout &p) {
        return JSON {
            {"output", write(static_cast<Bitcoin::outpoint>(p))}, 
            {"value", int64(p.Value)}};
    }
    
    JSON to_JSON(const Boost::candidate &c) {
        
        JSON::array_t arr;
        auto prevouts = c.Prevouts;
        while (!data::empty(prevouts)) {
            arr.push_back(to_JSON(prevouts.first()));
            prevouts = prevouts.rest();
        }
        
        return JSON {
            {"script", typed_data::write(typed_data::mainnet, c.Script.write())}, 
            {"prevouts", arr}, 
            {"value", int64(c.Value)}};
    }
    
    void mining_thread(miner *m, random *r, uint32 thread_number) {
        logger::log("begin thread", JSON(thread_number));
        work::puzzle puzzle{};
        while (true) {
            
            puzzle = m->latest();
            
            if (!puzzle.valid()) break;
            work::proof proof = solve(*r, puzzle, 10);
            if (proof.valid()) {
                logger::log("solution found in thread", JSON(thread_number));
                m->solved(proof.Solution);
            }
            
            puzzle = m->latest();
        }
        
        logger::log("end thread", JSON(thread_number));
        delete r;
    }
    
    void multithreaded::start_threads() {
        if (Workers.size() != 0) return;
        std::cout << "starting " << Threads << " threads." << std::endl;
        for (int i = 1; i <= Threads; i++) 
            Workers.emplace_back(&mining_thread, 
                &static_cast<miner &>(*this), 
                new casual_random{Seed + i}, i);
    }
    
    multithreaded::~multithreaded() {
        pose({});
        
        for (auto &thread : Workers) thread.join();
    }
    
    void redeemer::mine(const Boost::puzzle &p, const Bitcoin::address &redeem, double fee_rate) {
        Current = p;
        RedeemAddress = redeem;
        FeeRate = fee_rate;
        
        pose(work::puzzle(Current));
    }
    
    void redeemer::solved(const work::solution &solution) {
        // shouldn't happen
        if (!solution.valid()) return;
    
        bool proof_valid = work::proof{this->latest(), solution}.valid();
        
        std::cout << "Solution found! valid? " << std::boolalpha << proof_valid << std::endl;
        
        auto value = Current.Value;
        bytes pay_script = pay_to_address::script(RedeemAddress.Digest);
        Bitcoin::satoshi fee = BoostPOW::calculate_fee(Current.expected_size(), pay_script.size(), FeeRate);
        
        if (fee > value) throw string{"Cannot pay tx fee with boost output"};
        
        redeemed(BoostPOW::redeem_puzzle(Current, solution, {Bitcoin::output{value - fee, pay_script}}));
    }
    
    void manager::update_jobs(const BoostPOW::jobs &j) {
        Jobs = j;
        uint32 count_jobs = Jobs.size();
        if (count_jobs == 0) return;
        
        // remove jobs that are too difficult. 
        if (MaxDifficulty > 0) {
            for (auto it = Jobs.cbegin(); it != Jobs.cend();) 
                if (it->second.difficulty() > MaxDifficulty) 
                    it = Jobs.erase(it);
                else ++it;
            
            std::cout << (count_jobs - Jobs.size()) << " jobs removed due to high difficulty." << std::endl;
        }
        
        uint32 unprofitable_jobs = 0;
        for (auto it = Jobs.cbegin(); it != Jobs.cend(); it++) 
            if (it->second.profitability() < MinProfitability) 
                unprofitable_jobs++;
        
        uint32 viable_jobs = Jobs.size() - unprofitable_jobs;
        
        std::cout << "found " << unprofitable_jobs << " unprofitable jobs. " << 
        viable_jobs << " jobs remaining " << std::endl;
        
        if (viable_jobs == 0) return;
        
        // select a new job if now job has been selected. 
        if (Selected.first == digest256{}) return select_job();
        
        auto it = j.find(Selected.first);
        
        // job has been invalidated. 
        if (it == j.end() || it->second.Value != Selected.second.Value) select_job();
    }

    void manager::select_job() {
        Selected = select(Random, Jobs, MinProfitability);
        
        logger::log("job.selected", JSON {
            {"script_hash", BoostPOW::write(Selected.first)},
            {"difficulty", Selected.second.difficulty()},
            {"profitability", Selected.second.profitability()},
            {"job", BoostPOW::to_JSON(Selected.second)}
        });
        
        this->mine(Boost::puzzle{Selected.second, Keys->next()}, Addresses->next(), .5);
        
    }

    void manager::run() {
        
        while(true) {
            std::cout << "calling API" << std::endl;
            try {
                update_jobs(Net.jobs(100));
            } catch (const networking::HTTP::exception &exception) {
                std::cout << "API problem: " << exception.what() << std::endl;
            }
            
            std::this_thread::sleep_for (std::chrono::seconds(900));
            
        }
    }

    void manager::redeemed(const Bitcoin::transaction &redeem_tx) {
        if (!redeem_tx.valid()) {
            select_job();
            return;
        }
        
        Jobs.erase(Jobs.find(Selected.first));
        select_job();
        
        if (!Net.broadcast(bytes(redeem_tx))) std::cout << "broadcast failed!" << std::endl;
    }
    
}
