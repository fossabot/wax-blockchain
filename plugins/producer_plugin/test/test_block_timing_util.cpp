#include <boost/test/unit_test.hpp>
#include <eosio/producer_plugin/block_timing_util.hpp>
#include <fc/mock_time.hpp>

namespace fc {
std::ostream& boost_test_print_type(std::ostream& os, const time_point& t) { return os << t.to_iso_string(); }
std::ostream& boost_test_print_type(std::ostream& os, const std::optional<time_point>& t) { return os << (t ? t->to_iso_string() : "null"); }
} // namespace fc

static_assert(eosio::chain::config::block_interval_ms == 500);

constexpr auto       block_interval = fc::microseconds(eosio::chain::config::block_interval_us);
constexpr auto       cpu_effort_us  = 400000;
constexpr auto       cpu_effort     = fc::microseconds(cpu_effort_us);
constexpr auto       production_round_1st_block_slot = 100 * eosio::chain::config::producer_repetitions;


BOOST_AUTO_TEST_SUITE(block_timing_util)

BOOST_AUTO_TEST_CASE(test_production_round_block_start_time) {
   const fc::time_point production_round_1st_block_time =
      eosio::chain::block_timestamp_type(production_round_1st_block_slot).to_time_point();
   auto expected_start_time = production_round_1st_block_time - block_interval;
   for (int i = 0; i < eosio::chain::config::producer_repetitions;
        ++i, expected_start_time = expected_start_time + cpu_effort) {
      auto block_time = eosio::chain::block_timestamp_type(production_round_1st_block_slot + i);
      BOOST_CHECK_EQUAL(eosio::block_timing_util::production_round_block_start_time(cpu_effort_us, block_time), expected_start_time);
   }
}

BOOST_AUTO_TEST_CASE(test_calculate_block_deadline) {
   using namespace eosio::block_timing_util;
   const fc::time_point production_round_1st_block_time =
      eosio::chain::block_timestamp_type(production_round_1st_block_slot).to_time_point();

   {
      // Scenario 1:
      // In producing mode, the deadline of a block will be ahead of its block_time from 100, 200, 300, ...ms,
      // depending on the its index to the starting block of a production round. These deadlines are referred
      // as optimized deadlines.
      fc::mock_time_traits::set_now(production_round_1st_block_time - block_interval + fc::milliseconds(10));
      for (int i = 0; i < eosio::chain::config::producer_repetitions; ++i) {
         auto block_time        = eosio::chain::block_timestamp_type(production_round_1st_block_slot + i);
         auto expected_deadline = block_time.to_time_point() - fc::milliseconds((i + 1) * 100);
         BOOST_CHECK_EQUAL(calculate_producing_block_deadline(cpu_effort_us, block_time),
                           expected_deadline);
         fc::mock_time_traits::set_now(expected_deadline);
      }
   }
   {
      // Scenario 2:
      // In producing mode and it is already too late to meet the optimized deadlines,
      // the returned deadline can never be later than the hard deadlines.

      auto second_block_time = eosio::chain::block_timestamp_type(production_round_1st_block_slot + 1);
      fc::mock_time_traits::set_now(second_block_time.to_time_point() - fc::milliseconds(200));
      auto second_block_hard_deadline = second_block_time.to_time_point() - fc::milliseconds(100);
      BOOST_CHECK_EQUAL(calculate_producing_block_deadline(cpu_effort_us, second_block_time),
                        second_block_hard_deadline);
      // use previous deadline as now
      fc::mock_time_traits::set_now(second_block_hard_deadline);
      auto third_block_time = eosio::chain::block_timestamp_type(production_round_1st_block_slot + 2);
      BOOST_CHECK_EQUAL(calculate_producing_block_deadline(cpu_effort_us, third_block_time),
                        third_block_time.to_time_point() - fc::milliseconds(300));

      // use previous deadline as now
      fc::mock_time_traits::set_now(third_block_time.to_time_point() - fc::milliseconds(300));
      auto forth_block_time = eosio::chain::block_timestamp_type(production_round_1st_block_slot + 3);
      BOOST_CHECK_EQUAL(calculate_producing_block_deadline(cpu_effort_us, forth_block_time),
                        forth_block_time.to_time_point() - fc::milliseconds(400));

      ///////////////////////////////////////////////////////////////////////////////////////////////////

      auto seventh_block_time = eosio::chain::block_timestamp_type(production_round_1st_block_slot + 6);
      fc::mock_time_traits::set_now(seventh_block_time.to_time_point() - fc::milliseconds(500));

      BOOST_CHECK_EQUAL(calculate_producing_block_deadline(cpu_effort_us, seventh_block_time),
                        seventh_block_time.to_time_point() - fc::milliseconds(100));

      // use previous deadline as now
      fc::mock_time_traits::set_now(seventh_block_time.to_time_point() - fc::milliseconds(100));
      auto eighth_block_time = eosio::chain::block_timestamp_type(production_round_1st_block_slot + 7);

      BOOST_CHECK_EQUAL(calculate_producing_block_deadline(cpu_effort_us, eighth_block_time),
                        eighth_block_time.to_time_point() - fc::milliseconds(200));

      // use previous deadline as now
      fc::mock_time_traits::set_now(eighth_block_time.to_time_point() - fc::milliseconds(200));
      auto ninth_block_time = eosio::chain::block_timestamp_type(production_round_1st_block_slot + 8);

      BOOST_CHECK_EQUAL(calculate_producing_block_deadline(cpu_effort_us, ninth_block_time),
                        ninth_block_time.to_time_point() - fc::milliseconds(300));
   }
}

BOOST_AUTO_TEST_CASE(test_calculate_producer_wake_up_time) {
   using namespace eosio;
   using namespace eosio::chain;
   using namespace eosio::chain::literals;
   using namespace eosio::block_timing_util;

   producer_watermarks empty_watermarks;
   // use full cpu effort for these tests since calculate_producing_block_deadline is tested above
   constexpr uint32_t full_cpu_effort = eosio::chain::config::block_interval_us;

//    std::optional<fc::time_point> calculate_producer_wake_up_time(uint32_t cpu_effort_us, uint32_t block_num,
//                                                                        const chain::block_timestamp_type& ref_block_time,
//                                                                        const std::set<chain::account_name>& producers,
//                                                                        const std::vector<chain::producer_authority>& active_schedule,
//                                                                        const producer_watermarks& prod_watermarks);

   // no producers
   BOOST_CHECK_EQUAL(calculate_producer_wake_up_time(full_cpu_effort, 2, chain::block_timestamp_type{}, {}, {}, empty_watermarks), std::optional<fc::time_point>{});
   { // producers not in active_schedule
      std::set<chain::account_name>          producers{"p1"_n, "p2"_n};
      std::vector<chain::producer_authority> active_schedule{{"active1"_n}, {"active2"_n}};
      BOOST_CHECK_EQUAL(calculate_producer_wake_up_time(full_cpu_effort, 2, chain::block_timestamp_type{}, producers, active_schedule, empty_watermarks), std::optional<fc::time_point>{});
   }
   { // Only producer in active_schedule
      std::set<chain::account_name>          producers{"p1"_n, "p2"_n};
      std::vector<chain::producer_authority> active_schedule{{"p1"_n}};
      const uint32_t prod_round_1st_block_slot = 100 * active_schedule.size() * eosio::chain::config::producer_repetitions - 1;
      for (uint32_t i = 0; i < static_cast<uint32_t>(config::producer_repetitions * active_schedule.size() * 3); ++i) { // 3 rounds to test boundaries
         block_timestamp_type block_timestamp(prod_round_1st_block_slot + i);
         auto block_time = block_timestamp.to_time_point();
         BOOST_CHECK_EQUAL(calculate_producer_wake_up_time(full_cpu_effort, 2, block_timestamp, producers, active_schedule, empty_watermarks), block_time);
      }
   }
   { // Only producers in active_schedule
      std::set<chain::account_name>          producers{"p1"_n, "p2"_n, "p3"_n};
      std::vector<chain::producer_authority> active_schedule{{"p1"_n}, {"p2"_n}};
      const uint32_t prod_round_1st_block_slot = 100 * active_schedule.size() * eosio::chain::config::producer_repetitions - 1;
      for (uint32_t i = 0; i < static_cast<uint32_t>(config::producer_repetitions * active_schedule.size() * 3); ++i) { // 3 rounds to test boundaries
         block_timestamp_type block_timestamp(prod_round_1st_block_slot + i);
         auto block_time = block_timestamp.to_time_point();
         BOOST_CHECK_EQUAL(calculate_producer_wake_up_time(full_cpu_effort, 2, block_timestamp, producers, active_schedule, empty_watermarks), block_time);
      }
   }
   { // Only producers in active_schedule 21
      std::set<account_name> producers = {
         "inita"_n, "initb"_n, "initc"_n, "initd"_n, "inite"_n, "initf"_n, "initg"_n, "p1"_n,
         "inith"_n, "initi"_n, "initj"_n, "initk"_n, "initl"_n, "initm"_n, "initn"_n,
         "inito"_n, "initp"_n, "initq"_n, "initr"_n, "inits"_n, "initt"_n, "initu"_n, "p2"_n
      };
      std::vector<chain::producer_authority> active_schedule{
         {"inita"_n}, {"initb"_n}, {"initc"_n}, {"initd"_n}, {"inite"_n}, {"initf"_n}, {"initg"_n},
         {"inith"_n}, {"initi"_n}, {"initj"_n}, {"initk"_n}, {"initl"_n}, {"initm"_n}, {"initn"_n},
         {"inito"_n}, {"initp"_n}, {"initq"_n}, {"initr"_n}, {"inits"_n}, {"initt"_n}, {"initu"_n}
      };
      const uint32_t prod_round_1st_block_slot = 100 * active_schedule.size() * eosio::chain::config::producer_repetitions - 1;
      for (uint32_t i = 0; i < static_cast<uint32_t>(config::producer_repetitions * active_schedule.size() * 3); ++i) { // 3 rounds to test boundaries
         block_timestamp_type block_timestamp(prod_round_1st_block_slot + i);
         auto block_time = block_timestamp.to_time_point();
         BOOST_CHECK_EQUAL(calculate_producer_wake_up_time(full_cpu_effort, 2, block_timestamp, producers, active_schedule, empty_watermarks), block_time);
      }
   }
   { // One of many producers
      std::vector<chain::producer_authority> active_schedule{ // 21
         {"inita"_n}, {"initb"_n}, {"initc"_n}, {"initd"_n}, {"inite"_n}, {"initf"_n}, {"initg"_n},
         {"inith"_n}, {"initi"_n}, {"initj"_n}, {"initk"_n}, {"initl"_n}, {"initm"_n}, {"initn"_n},
         {"inito"_n}, {"initp"_n}, {"initq"_n}, {"initr"_n}, {"inits"_n}, {"initt"_n}, {"initu"_n}
      };
      const uint32_t prod_round_1st_block_slot = 100 * active_schedule.size() * eosio::chain::config::producer_repetitions - 1; // block production time

      // initb is second in the schedule, so it will produce config::producer_repetitions after
      std::set<account_name> producers = { "initb"_n };
      block_timestamp_type block_timestamp(prod_round_1st_block_slot);
      auto expected_block_time = block_timestamp_type(prod_round_1st_block_slot + config::producer_repetitions).to_time_point();
      BOOST_CHECK_EQUAL(calculate_producer_wake_up_time(full_cpu_effort, 2, block_timestamp_type{block_timestamp.slot-1}, producers, active_schedule, empty_watermarks), expected_block_time); // same
      BOOST_CHECK_EQUAL(calculate_producer_wake_up_time(full_cpu_effort, 2, block_timestamp_type{block_timestamp.slot+config::producer_repetitions-1}, producers, active_schedule, empty_watermarks), expected_block_time); // same
      BOOST_CHECK_EQUAL(calculate_producer_wake_up_time(full_cpu_effort, 2, block_timestamp_type{block_timestamp.slot+config::producer_repetitions-2}, producers, active_schedule, empty_watermarks), expected_block_time); // same
      BOOST_CHECK_EQUAL(calculate_producer_wake_up_time(full_cpu_effort, 2, block_timestamp_type{block_timestamp.slot+config::producer_repetitions-3}, producers, active_schedule, empty_watermarks), expected_block_time); // same
      // current which gives same expected
      BOOST_CHECK_EQUAL(calculate_producer_wake_up_time(full_cpu_effort, 2, block_timestamp_type{block_timestamp.slot+config::producer_repetitions}, producers, active_schedule, empty_watermarks), expected_block_time);
      expected_block_time += fc::microseconds(config::block_interval_us);
      BOOST_CHECK_EQUAL(calculate_producer_wake_up_time(full_cpu_effort, 2, block_timestamp_type{block_timestamp.slot+config::producer_repetitions+1}, producers, active_schedule, empty_watermarks), expected_block_time);

      producers = std::set<account_name>{ "inita"_n };
      // inita is first in the schedule, prod_round_1st_block_slot is block time of the first block, so will return the next block time as that is when current should be produced
      block_timestamp = block_timestamp_type{prod_round_1st_block_slot};
      expected_block_time = block_timestamp.to_time_point();
      BOOST_CHECK_EQUAL(calculate_producer_wake_up_time(full_cpu_effort, 2, block_timestamp_type{block_timestamp.slot-1}, producers, active_schedule, empty_watermarks), expected_block_time); // same
      BOOST_CHECK_EQUAL(calculate_producer_wake_up_time(full_cpu_effort, 2, block_timestamp_type{block_timestamp.slot-2}, producers, active_schedule, empty_watermarks), expected_block_time); // same
      BOOST_CHECK_EQUAL(calculate_producer_wake_up_time(full_cpu_effort, 2, block_timestamp_type{block_timestamp.slot-3}, producers, active_schedule, empty_watermarks), expected_block_time); // same
      for (size_t i = 0; i < config::producer_repetitions; ++i) {
         expected_block_time = block_timestamp_type(prod_round_1st_block_slot+i).to_time_point();
         BOOST_CHECK_EQUAL(calculate_producer_wake_up_time(full_cpu_effort, 2, block_timestamp, producers, active_schedule, empty_watermarks), expected_block_time);
         block_timestamp = block_timestamp.next();
      }
      expected_block_time = block_timestamp.to_time_point();
      BOOST_CHECK_NE(calculate_producer_wake_up_time(full_cpu_effort, 2, block_timestamp, producers, active_schedule, empty_watermarks), expected_block_time); // end of round, so not the next
      // initc
      producers = std::set<account_name>{ "initc"_n };
      block_timestamp = block_timestamp_type(prod_round_1st_block_slot);
      expected_block_time = block_timestamp_type(prod_round_1st_block_slot + 2*config::producer_repetitions).to_time_point();
      BOOST_CHECK_EQUAL(calculate_producer_wake_up_time(full_cpu_effort, 2, block_timestamp, producers, active_schedule, empty_watermarks), expected_block_time);
   }

}

BOOST_AUTO_TEST_SUITE_END()
