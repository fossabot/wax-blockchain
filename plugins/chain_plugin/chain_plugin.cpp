#include <eosio/chain_plugin/chain_plugin.hpp>
#include <eosio/chain_plugin/trx_retry_db.hpp>
#include <eosio/producer_plugin/producer_plugin.hpp>
#include <eosio/chain/fork_database.hpp>
#include <eosio/chain/block_log.hpp>
#include <eosio/chain/exceptions.hpp>
#include <eosio/chain/authorization_manager.hpp>
#include <eosio/chain/code_object.hpp>
#include <eosio/chain/config.hpp>
#include <eosio/chain/wasm_interface.hpp>
#include <eosio/chain/resource_limits.hpp>
#include <eosio/chain/controller.hpp>
#include <eosio/chain/generated_transaction_object.hpp>
#include <eosio/chain/snapshot.hpp>
#include <eosio/chain/deep_mind.hpp>
#include <eosio/chain_plugin/trx_finality_status_processing.hpp>
#include <eosio/chain/permission_link_object.hpp>
#include <eosio/chain/global_property_object.hpp>

#include <eosio/resource_monitor_plugin/resource_monitor_plugin.hpp>

#include <chainbase/environment.hpp>

#include <boost/signals2/connection.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/path.hpp>

#include <fc/io/json.hpp>
#include <fc/variant.hpp>
#include <cstdlib>

// reflect chainbase::environment for --print-build-info option
FC_REFLECT_ENUM( chainbase::environment::os_t,
                 (OS_LINUX)(OS_MACOS)(OS_WINDOWS)(OS_OTHER) )
FC_REFLECT_ENUM( chainbase::environment::arch_t,
                 (ARCH_X86_64)(ARCH_ARM)(ARCH_RISCV)(ARCH_OTHER) )
FC_REFLECT(chainbase::environment, (debug)(os)(arch)(boost_version)(compiler) )

const fc::string deep_mind_logger_name("deep-mind");
eosio::chain::deep_mind_handler _deep_mind_log;

namespace eosio {

//declare operator<< and validate function for read_mode in the same namespace as read_mode itself
namespace chain {

std::ostream& operator<<(std::ostream& osm, eosio::chain::db_read_mode m) {
   if ( m == eosio::chain::db_read_mode::HEAD ) {
      osm << "head";
   } else if ( m == eosio::chain::db_read_mode::IRREVERSIBLE ) {
      osm << "irreversible";
   } else if ( m == eosio::chain::db_read_mode::SPECULATIVE ) {
      osm << "speculative";
   }

   return osm;
}

void validate(boost::any& v,
              const std::vector<std::string>& values,
              eosio::chain::db_read_mode* /* target_type */,
              int)
{
  using namespace boost::program_options;

  // Make sure no previous assignment to 'v' was made.
  validators::check_first_occurrence(v);

  // Extract the first string from 'values'. If there is more than
  // one string, it's an error, and exception will be thrown.
  std::string const& s = validators::get_single_string(values);

  if ( s == "head" ) {
     v = boost::any(eosio::chain::db_read_mode::HEAD);
  } else if ( s == "irreversible" ) {
     v = boost::any(eosio::chain::db_read_mode::IRREVERSIBLE);
  } else if ( s == "speculative" ) {
     v = boost::any(eosio::chain::db_read_mode::SPECULATIVE);
  } else {
     throw validation_error(validation_error::invalid_option_value);
  }
}

std::ostream& operator<<(std::ostream& osm, eosio::chain::validation_mode m) {
   if ( m == eosio::chain::validation_mode::FULL ) {
      osm << "full";
   } else if ( m == eosio::chain::validation_mode::LIGHT ) {
      osm << "light";
   }

   return osm;
}

void validate(boost::any& v,
              const std::vector<std::string>& values,
              eosio::chain::validation_mode* /* target_type */,
              int)
{
  using namespace boost::program_options;

  // Make sure no previous assignment to 'v' was made.
  validators::check_first_occurrence(v);

  // Extract the first string from 'values'. If there is more than
  // one string, it's an error, and exception will be thrown.
  std::string const& s = validators::get_single_string(values);

  if ( s == "full" ) {
     v = boost::any(eosio::chain::validation_mode::FULL);
  } else if ( s == "light" ) {
     v = boost::any(eosio::chain::validation_mode::LIGHT);
  } else {
     throw validation_error(validation_error::invalid_option_value);
  }
}

}

using namespace eosio;
using namespace eosio::chain;
using namespace eosio::chain::config;
using namespace eosio::chain::plugin_interface;
using vm_type = wasm_interface::vm_type;
using fc::flat_map;

using boost::signals2::scoped_connection;

class chain_plugin_impl {
public:
   chain_plugin_impl()
   :pre_accepted_block_channel(app().get_channel<channels::pre_accepted_block>())
   ,accepted_block_header_channel(app().get_channel<channels::accepted_block_header>())
   ,accepted_block_channel(app().get_channel<channels::accepted_block>())
   ,irreversible_block_channel(app().get_channel<channels::irreversible_block>())
   ,accepted_transaction_channel(app().get_channel<channels::accepted_transaction>())
   ,applied_transaction_channel(app().get_channel<channels::applied_transaction>())
   ,incoming_block_sync_method(app().get_method<incoming::methods::block_sync>())
   ,incoming_transaction_async_method(app().get_method<incoming::methods::transaction_async>())
   {}

   bfs::path                        blocks_dir;
   bfs::path                        state_dir;
   bool                             readonly = false;
   flat_map<uint32_t,block_id_type> loaded_checkpoints;
   bool                             accept_transactions = false;
   bool                             api_accept_transactions = true;
   bool                             account_queries_enabled = false;

   std::optional<controller::config> chain_config;
   std::optional<controller>         chain;
   std::optional<genesis_state>      genesis;
   //txn_msg_rate_limits              rate_limits;
   std::optional<vm_type>            wasm_runtime;
   fc::microseconds                  abi_serializer_max_time_us;
   std::optional<bfs::path>          snapshot_path;


   // retained references to channels for easy publication
   channels::pre_accepted_block::channel_type&     pre_accepted_block_channel;
   channels::accepted_block_header::channel_type&  accepted_block_header_channel;
   channels::accepted_block::channel_type&         accepted_block_channel;
   channels::irreversible_block::channel_type&     irreversible_block_channel;
   channels::accepted_transaction::channel_type&   accepted_transaction_channel;
   channels::applied_transaction::channel_type&    applied_transaction_channel;

   // retained references to methods for easy calling
   incoming::methods::block_sync::method_type&        incoming_block_sync_method;
   incoming::methods::transaction_async::method_type& incoming_transaction_async_method;

   // method provider handles
   methods::get_block_by_number::method_type::handle                 get_block_by_number_provider;
   methods::get_block_by_id::method_type::handle                     get_block_by_id_provider;
   methods::get_head_block_id::method_type::handle                   get_head_block_id_provider;
   methods::get_last_irreversible_block_number::method_type::handle  get_last_irreversible_block_number_provider;

   // scoped connections for chain controller
   std::optional<scoped_connection>                                   pre_accepted_block_connection;
   std::optional<scoped_connection>                                   accepted_block_header_connection;
   std::optional<scoped_connection>                                   accepted_block_connection;
   std::optional<scoped_connection>                                   irreversible_block_connection;
   std::optional<scoped_connection>                                   accepted_transaction_connection;
   std::optional<scoped_connection>                                   applied_transaction_connection;
   std::optional<scoped_connection>                                   block_start_connection;


   std::optional<chain_apis::account_query_db>                        _account_query_db;
   const producer_plugin* producer_plug;
   std::optional<chain_apis::trx_retry_db>                            _trx_retry_db;
   chain_apis::trx_finality_status_processing_ptr                     _trx_finality_status_processing;
};

chain_plugin::chain_plugin()
:my(new chain_plugin_impl()) {
   app().register_config_type<eosio::chain::db_read_mode>();
   app().register_config_type<eosio::chain::validation_mode>();
   app().register_config_type<chainbase::pinnable_mapped_file::map_mode>();
   app().register_config_type<eosio::chain::wasm_interface::vm_type>();
}

chain_plugin::~chain_plugin(){}

void chain_plugin::set_program_options(options_description& cli, options_description& cfg)
{
   // build wasm_runtime help text
   std::string wasm_runtime_opt = "Override default WASM runtime (";
   std::string wasm_runtime_desc;
   std::string delim;
#ifdef EOSIO_EOS_VM_JIT_RUNTIME_ENABLED
   wasm_runtime_opt += " \"eos-vm-jit\"";
   wasm_runtime_desc += "\"eos-vm-jit\" : A WebAssembly runtime that compiles WebAssembly code to native x86 code prior to execution.\n";
   delim = ", ";
#endif

#ifdef EOSIO_EOS_VM_RUNTIME_ENABLED
   wasm_runtime_opt += delim + "\"eos-vm\"";
   wasm_runtime_desc += "\"eos-vm\" : A WebAssembly interpreter.\n";
   delim = ", ";
#endif

#ifdef EOSIO_EOS_VM_OC_DEVELOPER
   wasm_runtime_opt += delim + "\"eos-vm-oc\"";
   wasm_runtime_desc += "\"eos-vm-oc\" : Unsupported. Instead, use one of the other runtimes along with the option enable-eos-vm-oc.\n";
#endif
   wasm_runtime_opt += ")\n" + wasm_runtime_desc;

   std::string default_wasm_runtime_str= eosio::chain::wasm_interface::vm_type_string(eosio::chain::config::default_wasm_runtime);

   cfg.add_options()
         ("blocks-dir", bpo::value<bfs::path>()->default_value("blocks"),
          "the location of the blocks directory (absolute path or relative to application data dir)")
         ("blocks-log-stride", bpo::value<uint32_t>(),
         "split the block log file when the head block number is the multiple of the stride\n"
         "When the stride is reached, the current block log and index will be renamed '<blocks-retained-dir>/blocks-<start num>-<end num>.log/index'\n"
         "and a new current block log and index will be created with the most recent block. All files following\n"
         "this format will be used to construct an extended block log.")
         ("max-retained-block-files", bpo::value<uint32_t>(),
          "the maximum number of blocks files to retain so that the blocks in those files can be queried.\n"
          "When the number is reached, the oldest block file would be moved to archive dir or deleted if the archive dir is empty.\n"
          "The retained block log files should not be manipulated by users." )
         ("blocks-retained-dir", bpo::value<bfs::path>(),
          "the location of the blocks retained directory (absolute path or relative to blocks dir).\n"
          "If the value is empty, it is set to the value of blocks dir.")
         ("blocks-archive-dir", bpo::value<bfs::path>(),
          "the location of the blocks archive directory (absolute path or relative to blocks dir).\n"
          "If the value is empty, blocks files beyond the retained limit will be deleted.\n"
          "All files in the archive directory are completely under user's control, i.e. they won't be accessed by nodeos anymore.")
         ("state-dir", bpo::value<bfs::path>()->default_value(config::default_state_dir_name),
          "the location of the state directory (absolute path or relative to application data dir)")
         ("protocol-features-dir", bpo::value<bfs::path>()->default_value("protocol_features"),
          "the location of the protocol_features directory (absolute path or relative to application config dir)")
         ("checkpoint", bpo::value<vector<string>>()->composing(), "Pairs of [BLOCK_NUM,BLOCK_ID] that should be enforced as checkpoints.")
         ("wasm-runtime", bpo::value<eosio::chain::wasm_interface::vm_type>()->value_name("runtime")->notifier([](const auto& vm){
#ifndef EOSIO_EOS_VM_OC_DEVELOPER
            //throwing an exception here (like EOS_ASSERT) is just gobbled up with a "Failed to initialize" error :(
            if(vm == wasm_interface::vm_type::eos_vm_oc) {
               elog("EOS VM OC is a tier-up compiler and works in conjunction with the configured base WASM runtime. Enable EOS VM OC via 'eos-vm-oc-enable' option");
               EOS_ASSERT(false, plugin_exception, "");
            }
#endif
         })->default_value(eosio::chain::config::default_wasm_runtime, default_wasm_runtime_str), wasm_runtime_opt.c_str()
         )
         ("profile-account", boost::program_options::value<vector<string>>()->composing(),
          "The name of an account whose code will be profiled")
         ("abi-serializer-max-time-ms", bpo::value<uint32_t>()->default_value(config::default_abi_serializer_max_time_us / 1000),
          "Override default maximum ABI serialization time allowed in ms")
         ("chain-state-db-size-mb", bpo::value<uint64_t>()->default_value(config::default_state_size / (1024  * 1024)), "Maximum size (in MiB) of the chain state database")
         ("chain-state-db-guard-size-mb", bpo::value<uint64_t>()->default_value(config::default_state_guard_size / (1024  * 1024)), "Safely shut down node when free space remaining in the chain state database drops below this size (in MiB).")
         ("signature-cpu-billable-pct", bpo::value<uint32_t>()->default_value(config::default_sig_cpu_bill_pct / config::percent_1),
          "Percentage of actual signature recovery cpu to bill. Whole number percentages, e.g. 50 for 50%")
         ("chain-threads", bpo::value<uint16_t>()->default_value(config::default_controller_thread_pool_size),
          "Number of worker threads in controller thread pool")
         ("contracts-console", bpo::bool_switch()->default_value(false),
          "print contract's output to console")
         ("deep-mind", bpo::bool_switch()->default_value(false),
          "print deeper information about chain operations")
         ("actor-whitelist", boost::program_options::value<vector<string>>()->composing()->multitoken(),
          "Account added to actor whitelist (may specify multiple times)")
         ("actor-blacklist", boost::program_options::value<vector<string>>()->composing()->multitoken(),
          "Account added to actor blacklist (may specify multiple times)")
         ("contract-whitelist", boost::program_options::value<vector<string>>()->composing()->multitoken(),
          "Contract account added to contract whitelist (may specify multiple times)")
         ("contract-blacklist", boost::program_options::value<vector<string>>()->composing()->multitoken(),
          "Contract account added to contract blacklist (may specify multiple times)")
         ("action-blacklist", boost::program_options::value<vector<string>>()->composing()->multitoken(),
          "Action (in the form code::action) added to action blacklist (may specify multiple times)")
         ("key-blacklist", boost::program_options::value<vector<string>>()->composing()->multitoken(),
          "Public key added to blacklist of keys that should not be included in authorities (may specify multiple times)")
         ("sender-bypass-whiteblacklist", boost::program_options::value<vector<string>>()->composing()->multitoken(),
          "Deferred transactions sent by accounts in this list do not have any of the subjective whitelist/blacklist checks applied to them (may specify multiple times)")
         ("read-mode", boost::program_options::value<eosio::chain::db_read_mode>()->default_value(eosio::chain::db_read_mode::HEAD),
          "Database read mode (\"head\", \"irreversible\", \"speculative\").\n"
          "In \"head\" mode: database contains state changes up to the head block; transactions received by the node are relayed if valid.\n"
          "In \"irreversible\" mode: database contains state changes up to the last irreversible block; "
          "transactions received via the P2P network are not relayed and transactions cannot be pushed via the chain API.\n"
          "In \"speculative\" mode: database contains state changes by transactions in the blockchain "
          "up to the head block as well as some transactions not yet included in the blockchain; transactions received by the node are relayed if valid.\n"
          )
         ( "api-accept-transactions", bpo::value<bool>()->default_value(true), "Allow API transactions to be evaluated and relayed if valid.")
         ("validation-mode", boost::program_options::value<eosio::chain::validation_mode>()->default_value(eosio::chain::validation_mode::FULL),
          "Chain validation mode (\"full\" or \"light\").\n"
          "In \"full\" mode all incoming blocks will be fully validated.\n"
          "In \"light\" mode all incoming blocks headers will be fully validated; transactions in those validated blocks will be trusted \n")
         ("disable-ram-billing-notify-checks", bpo::bool_switch()->default_value(false),
          "Disable the check which subjectively fails a transaction if a contract bills more RAM to another account within the context of a notification handler (i.e. when the receiver is not the code of the action).")
#ifdef EOSIO_DEVELOPER
         ("disable-all-subjective-mitigations", bpo::bool_switch()->default_value(false),
          "Disable all subjective mitigations checks in the entire codebase.")
#endif
         ("maximum-variable-signature-length", bpo::value<uint32_t>()->default_value(16384u),
          "Subjectively limit the maximum length of variable components in a variable legnth signature to this size in bytes")
         ("trusted-producer", bpo::value<vector<string>>()->composing(), "Indicate a producer whose blocks headers signed by it will be fully validated, but transactions in those validated blocks will be trusted.")
         ("database-map-mode", bpo::value<chainbase::pinnable_mapped_file::map_mode>()->default_value(chainbase::pinnable_mapped_file::map_mode::mapped),
          "Database map mode (\"mapped\", \"heap\", or \"locked\").\n"
          "In \"mapped\" mode database is memory mapped as a file.\n"
#ifndef _WIN32
          "In \"heap\" mode database is preloaded in to swappable memory and will use huge pages if available.\n"
          "In \"locked\" mode database is preloaded, locked in to memory, and will use huge pages if available.\n"
#endif
         )

#ifdef EOSIO_EOS_VM_OC_RUNTIME_ENABLED
         ("eos-vm-oc-cache-size-mb", bpo::value<uint64_t>()->default_value(eosvmoc::config().cache_size / (1024u*1024u)), "Maximum size (in MiB) of the EOS VM OC code cache")
         ("eos-vm-oc-compile-threads", bpo::value<uint64_t>()->default_value(1u)->notifier([](const auto t) {
               if(t == 0) {
                  elog("eos-vm-oc-compile-threads must be set to a non-zero value");
                  EOS_ASSERT(false, plugin_exception, "");
               }
         }), "Number of threads to use for EOS VM OC tier-up")
         ("eos-vm-oc-enable", bpo::bool_switch(), "Enable EOS VM OC tier-up runtime")
#endif
         ("enable-account-queries", bpo::value<bool>()->default_value(false), "enable queries to find accounts by various metadata.")
         ("max-nonprivileged-inline-action-size", bpo::value<uint32_t>()->default_value(config::default_max_nonprivileged_inline_action_size), "maximum allowed size (in bytes) of an inline action for a nonprivileged account")
         ("transaction-retry-max-storage-size-gb", bpo::value<uint64_t>(),
          "Maximum size (in GiB) allowed to be allocated for the Transaction Retry feature. Setting above 0 enables this feature.")
         ("transaction-retry-interval-sec", bpo::value<uint32_t>()->default_value(20),
          "How often, in seconds, to resend an incoming transaction to network if not seen in a block.\n"
          "Needs to be at least twice as large as p2p-dedup-cache-expire-time-sec.")
         ("transaction-retry-max-expiration-sec", bpo::value<uint32_t>()->default_value(120),
          "Maximum allowed transaction expiration for retry transactions, will retry transactions up to this value.\n"
          "Should be larger than transaction-retry-interval-sec.")
         ("transaction-finality-status-max-storage-size-gb", bpo::value<uint64_t>(),
          "Maximum size (in GiB) allowed to be allocated for the Transaction Finality Status feature. Setting above 0 enables this feature.")
         ("transaction-finality-status-success-duration-sec", bpo::value<uint64_t>()->default_value(config::default_max_transaction_finality_status_success_duration_sec),
          "Duration (in seconds) a successful transaction's Finality Status will remain available from being first identified.")
         ("transaction-finality-status-failure-duration-sec", bpo::value<uint64_t>()->default_value(config::default_max_transaction_finality_status_failure_duration_sec),
          "Duration (in seconds) a failed transaction's Finality Status will remain available from being first identified.")
         ("integrity-hash-on-start", bpo::bool_switch(), "Log the state integrity hash on startup")
         ("integrity-hash-on-stop", bpo::bool_switch(), "Log the state integrity hash on shutdown");

    cfg.add_options()("block-log-retain-blocks", bpo::value<uint32_t>(), "If set to greater than 0, periodically prune the block log to store only configured number of most recent blocks.\n"
        "If set to 0, no blocks are be written to the block log; block log file is removed after startup.");


// TODO: rate limiting
         /*("per-authorized-account-transaction-msg-rate-limit-time-frame-sec", bpo::value<uint32_t>()->default_value(default_per_auth_account_time_frame_seconds),
          "The time frame, in seconds, that the per-authorized-account-transaction-msg-rate-limit is imposed over.")
         ("per-authorized-account-transaction-msg-rate-limit", bpo::value<uint32_t>()->default_value(default_per_auth_account),
          "Limits the maximum rate of transaction messages that an account is allowed each per-authorized-account-transaction-msg-rate-limit-time-frame-sec.")
          ("per-code-account-transaction-msg-rate-limit-time-frame-sec", bpo::value<uint32_t>()->default_value(default_per_code_account_time_frame_seconds),
           "The time frame, in seconds, that the per-code-account-transaction-msg-rate-limit is imposed over.")
          ("per-code-account-transaction-msg-rate-limit", bpo::value<uint32_t>()->default_value(default_per_code_account),
           "Limits the maximum rate of transaction messages that an account's code is allowed each per-code-account-transaction-msg-rate-limit-time-frame-sec.")*/

   cli.add_options()
         ("genesis-json", bpo::value<bfs::path>(), "File to read Genesis State from")
         ("genesis-timestamp", bpo::value<string>(), "override the initial timestamp in the Genesis State file")
         ("print-genesis-json", bpo::bool_switch()->default_value(false),
          "extract genesis_state from blocks.log as JSON, print to console, and exit")
         ("extract-genesis-json", bpo::value<bfs::path>(),
          "extract genesis_state from blocks.log as JSON, write into specified file, and exit")
         ("print-build-info", bpo::bool_switch()->default_value(false),
          "print build environment information to console as JSON and exit")
         ("extract-build-info", bpo::value<bfs::path>(),
          "extract build environment information as JSON, write into specified file, and exit")
         ("force-all-checks", bpo::bool_switch()->default_value(false),
          "do not skip any validation checks while replaying blocks (useful for replaying blocks from untrusted source)")
         ("disable-replay-opts", bpo::bool_switch()->default_value(false),
          "disable optimizations that specifically target replay")
         ("replay-blockchain", bpo::bool_switch()->default_value(false),
          "clear chain state database and replay all blocks")
         ("hard-replay-blockchain", bpo::bool_switch()->default_value(false),
          "clear chain state database, recover as many blocks as possible from the block log, and then replay those blocks")
         ("delete-all-blocks", bpo::bool_switch()->default_value(false),
          "clear chain state database and block log")
         ("truncate-at-block", bpo::value<uint32_t>()->default_value(0),
          "stop hard replay / block log recovery at this block number (if set to non-zero number)")
         ("terminate-at-block", bpo::value<uint32_t>()->default_value(0),
          "terminate after reaching this block number (if set to a non-zero number)")
         ("snapshot", bpo::value<bfs::path>(), "File to read Snapshot State from")
         ;

}

#define LOAD_VALUE_SET(options, op_name, container) \
if( options.count(op_name) ) { \
   const std::vector<std::string>& ops = options[op_name].as<std::vector<std::string>>(); \
   for( const auto& v : ops ) { \
      container.emplace( eosio::chain::name( v ) ); \
   } \
}

fc::time_point calculate_genesis_timestamp( string tstr ) {
   fc::time_point genesis_timestamp;
   if( strcasecmp (tstr.c_str(), "now") == 0 ) {
      genesis_timestamp = fc::time_point::now();
   } else {
      genesis_timestamp = time_point::from_iso_string( tstr );
   }

   auto epoch_us = genesis_timestamp.time_since_epoch().count();
   auto diff_us = epoch_us % config::block_interval_us;
   if (diff_us > 0) {
      auto delay_us = (config::block_interval_us - diff_us);
      genesis_timestamp += fc::microseconds(delay_us);
      dlog("pausing ${us} microseconds to the next interval",("us",delay_us));
   }

   ilog( "Adjusting genesis timestamp to ${timestamp}", ("timestamp", genesis_timestamp) );
   return genesis_timestamp;
}

void clear_directory_contents( const fc::path& p ) {
   using boost::filesystem::directory_iterator;

   if( !fc::is_directory( p ) )
      return;

   for( directory_iterator enditr, itr{p}; itr != enditr; ++itr ) {
      fc::remove_all( itr->path() );
   }
}

namespace {
  // This can be removed when versions of eosio that support reversible chainbase state file no longer supported.
  void upgrade_from_reversible_to_fork_db(chain_plugin_impl* my) {
     namespace bfs = boost::filesystem;
     bfs::path old_fork_db = my->chain_config->state_dir / config::forkdb_filename;
     bfs::path new_fork_db = my->blocks_dir / config::reversible_blocks_dir_name / config::forkdb_filename;
     if( bfs::exists( old_fork_db ) && bfs::is_regular_file( old_fork_db ) ) {
        bool copy_file = false;
        if( bfs::exists( new_fork_db ) && bfs::is_regular_file( new_fork_db ) ) {
           if( bfs::last_write_time( old_fork_db ) > bfs::last_write_time( new_fork_db ) ) {
              copy_file = true;
           }
        } else {
           copy_file = true;
           bfs::create_directories( my->blocks_dir / config::reversible_blocks_dir_name );
        }
        if( copy_file ) {
           fc::rename( old_fork_db, new_fork_db );
        } else {
           fc::remove( old_fork_db );
        }
     }
  }
}

void
chain_plugin::do_hard_replay(const variables_map& options) {
         ilog( "Hard replay requested: deleting state database" );
         clear_directory_contents( my->chain_config->state_dir );
         auto backup_dir = block_log::repair_log( my->blocks_dir, options.at( "truncate-at-block" ).as<uint32_t>(), config::reversible_blocks_dir_name);
}

void chain_plugin::plugin_initialize(const variables_map& options) {
   try {
      handle_sighup(); // Sets loggers
      ilog("initializing chain plugin");

      try {
         genesis_state gs; // Check if EOSIO_ROOT_KEY is bad
      } catch ( const std::exception& ) {
         elog( "EOSIO_ROOT_KEY ('${root_key}') is invalid. Recompile with a valid public key.",
               ("root_key", genesis_state::eosio_root_key));
         throw;
      }

      my->chain_config = controller::config();

      if( options.at( "print-build-info" ).as<bool>() || options.count( "extract-build-info") ) {
         if( options.at( "print-build-info" ).as<bool>() ) {
            ilog( "Build environment JSON:\n${e}", ("e", json::to_pretty_string( chainbase::environment() )) );
         }
         if( options.count( "extract-build-info") ) {
            auto p = options.at( "extract-build-info" ).as<bfs::path>();

            if( p.is_relative()) {
               p = bfs::current_path() / p;
            }

            EOS_ASSERT( fc::json::save_to_file( chainbase::environment(), p, true ), misc_exception,
                        "Error occurred while writing build info JSON to '${path}'",
                        ("path", p.generic_string())
            );

            ilog( "Saved build info JSON to '${path}'", ("path", p.generic_string()) );
         }

         EOS_THROW( node_management_success, "reported build environment information" );
      }

      LOAD_VALUE_SET( options, "sender-bypass-whiteblacklist", my->chain_config->sender_bypass_whiteblacklist );
      LOAD_VALUE_SET( options, "actor-whitelist", my->chain_config->actor_whitelist );
      LOAD_VALUE_SET( options, "actor-blacklist", my->chain_config->actor_blacklist );
      LOAD_VALUE_SET( options, "contract-whitelist", my->chain_config->contract_whitelist );
      LOAD_VALUE_SET( options, "contract-blacklist", my->chain_config->contract_blacklist );

      LOAD_VALUE_SET( options, "trusted-producer", my->chain_config->trusted_producers );

      if( options.count( "action-blacklist" )) {
         const std::vector<std::string>& acts = options["action-blacklist"].as<std::vector<std::string>>();
         auto& list = my->chain_config->action_blacklist;
         for( const auto& a : acts ) {
            auto pos = a.find( "::" );
            EOS_ASSERT( pos != std::string::npos, plugin_config_exception, "Invalid entry in action-blacklist: '${a}'", ("a", a));
            account_name code( a.substr( 0, pos ));
            action_name act( a.substr( pos + 2 ));
            list.emplace( code, act );
         }
      }

      if( options.count( "key-blacklist" )) {
         const std::vector<std::string>& keys = options["key-blacklist"].as<std::vector<std::string>>();
         auto& list = my->chain_config->key_blacklist;
         for( const auto& key_str : keys ) {
            list.emplace( key_str );
         }
      }

      if( options.count( "blocks-dir" )) {
         auto bld = options.at( "blocks-dir" ).as<bfs::path>();
         if( bld.is_relative())
            my->blocks_dir = app().data_dir() / bld;
         else
            my->blocks_dir = bld;
      }

      if( options.count( "state-dir" )) {
         auto sd = options.at( "state-dir" ).as<bfs::path>();
         if( sd.is_relative())
            my->state_dir = app().data_dir() / sd;
         else
            my->state_dir = sd;
      }

      protocol_feature_set pfs;
      {
         fc::path protocol_features_dir;
         auto pfd = options.at( "protocol-features-dir" ).as<bfs::path>();
         if( pfd.is_relative())
            protocol_features_dir = app().config_dir() / pfd;
         else
            protocol_features_dir = pfd;

         pfs = initialize_protocol_features( protocol_features_dir );
      }

      if( options.count("checkpoint") ) {
         auto cps = options.at("checkpoint").as<vector<string>>();
         my->loaded_checkpoints.reserve(cps.size());
         for( const auto& cp : cps ) {
            auto item = fc::json::from_string(cp).as<std::pair<uint32_t,block_id_type>>();
            auto itr = my->loaded_checkpoints.find(item.first);
            if( itr != my->loaded_checkpoints.end() ) {
               EOS_ASSERT( itr->second == item.second,
                           plugin_config_exception,
                          "redefining existing checkpoint at block number ${num}: original: ${orig} new: ${new}",
                          ("num", item.first)("orig", itr->second)("new", item.second)
               );
            } else {
               my->loaded_checkpoints[item.first] = item.second;
            }
         }
      }

      if( options.count( "wasm-runtime" ))
         my->wasm_runtime = options.at( "wasm-runtime" ).as<vm_type>();

      LOAD_VALUE_SET( options, "profile-account", my->chain_config->profile_accounts );

      my->abi_serializer_max_time_us = fc::microseconds(options.at("abi-serializer-max-time-ms").as<uint32_t>() * 1000);

      my->chain_config->blocks_dir = my->blocks_dir;
      my->chain_config->state_dir = my->state_dir;
      my->chain_config->read_only = my->readonly;

      if (auto resmon_plugin = app().find_plugin<resource_monitor_plugin>()) {
        resmon_plugin->monitor_directory(my->chain_config->blocks_dir);
        resmon_plugin->monitor_directory(my->chain_config->state_dir);
      }

      if( options.count( "chain-state-db-size-mb" ))
         my->chain_config->state_size = options.at( "chain-state-db-size-mb" ).as<uint64_t>() * 1024 * 1024;

      if( options.count( "chain-state-db-guard-size-mb" ))
         my->chain_config->state_guard_size = options.at( "chain-state-db-guard-size-mb" ).as<uint64_t>() * 1024 * 1024;

      if( options.count( "max-nonprivileged-inline-action-size" ))
         my->chain_config->max_nonprivileged_inline_action_size = options.at( "max-nonprivileged-inline-action-size" ).as<uint32_t>();

      if( options.count( "transaction-finality-status-max-storage-size-gb" )) {
         const uint64_t max_storage_size = options.at( "transaction-finality-status-max-storage-size-gb" ).as<uint64_t>() * 1024 * 1024 * 1024;
         if (max_storage_size > 0) {
            const fc::microseconds success_duration = fc::seconds(options.at( "transaction-finality-status-success-duration-sec" ).as<uint64_t>());
            const fc::microseconds failure_duration = fc::seconds(options.at( "transaction-finality-status-failure-duration-sec" ).as<uint64_t>());
            my->_trx_finality_status_processing.reset(
               new chain_apis::trx_finality_status_processing(max_storage_size, success_duration, failure_duration));
         }
      }

      if( options.count( "chain-threads" )) {
         my->chain_config->thread_pool_size = options.at( "chain-threads" ).as<uint16_t>();
         EOS_ASSERT( my->chain_config->thread_pool_size > 0, plugin_config_exception,
                     "chain-threads ${num} must be greater than 0", ("num", my->chain_config->thread_pool_size) );
      }

      my->chain_config->sig_cpu_bill_pct = options.at("signature-cpu-billable-pct").as<uint32_t>();
      EOS_ASSERT( my->chain_config->sig_cpu_bill_pct >= 0 && my->chain_config->sig_cpu_bill_pct <= 100, plugin_config_exception,
                  "signature-cpu-billable-pct must be 0 - 100, ${pct}", ("pct", my->chain_config->sig_cpu_bill_pct) );
      my->chain_config->sig_cpu_bill_pct *= config::percent_1;

      if( my->wasm_runtime )
         my->chain_config->wasm_runtime = *my->wasm_runtime;

      my->chain_config->force_all_checks = options.at( "force-all-checks" ).as<bool>();
      my->chain_config->disable_replay_opts = options.at( "disable-replay-opts" ).as<bool>();
      my->chain_config->contracts_console = options.at( "contracts-console" ).as<bool>();
      my->chain_config->allow_ram_billing_in_notify = options.at( "disable-ram-billing-notify-checks" ).as<bool>();

#ifdef EOSIO_DEVELOPER
      my->chain_config->disable_all_subjective_mitigations = options.at( "disable-all-subjective-mitigations" ).as<bool>();
#endif

      my->chain_config->maximum_variable_signature_length = options.at( "maximum-variable-signature-length" ).as<uint32_t>();

      if( options.count( "terminate-at-block" ))
         my->chain_config->terminate_at_block = options.at( "terminate-at-block" ).as<uint32_t>();

      // move fork_db to new location
      upgrade_from_reversible_to_fork_db( my.get() );

      bool has_partitioned_block_log_options = options.count("blocks-retained-dir") ||  options.count("blocks-archive-dir")
         || options.count("blocks-log-stride") || options.count("max-retained-block-files");
      bool has_retain_blocks_option = options.count("block-log-retain-blocks");

      EOS_ASSERT(!has_partitioned_block_log_options || !has_retain_blocks_option, plugin_config_exception,
         "block-log-retain-blocks cannot be specified together with blocks-retained-dir, blocks-archive-dir or blocks-log-stride or max-retained-block-files.");

      fc::path retained_dir;
      if (has_partitioned_block_log_options) {
         retained_dir = options.count("blocks-retained-dir") ? options.at("blocks-retained-dir").as<bfs::path>()
                                                                 : bfs::path("");
         if (retained_dir.is_relative())
            retained_dir = fc::path{my->blocks_dir}/retained_dir;
         my->chain_config->blog = eosio::chain::partitioned_blocklog_config{
            .retained_dir = retained_dir,
            .archive_dir  = options.count("blocks-archive-dir") ? options.at("blocks-archive-dir").as<bfs::path>()
                                                                : bfs::path("archive"),
            .stride       = options.count("blocks-log-stride") ? options.at("blocks-log-stride").as<uint32_t>()
                                                               : UINT32_MAX,
            .max_retained_files = options.count("max-retained-block-files")
                                        ? options.at("max-retained-block-files").as<uint32_t>()
                                        : UINT32_MAX,
         };
      } else if(has_retain_blocks_option) {
         uint32_t block_log_retain_blocks = options.at("block-log-retain-blocks").as<uint32_t>();
         if (block_log_retain_blocks == 0)
            my->chain_config->blog = eosio::chain::empty_blocklog_config{};
         else {
            EOS_ASSERT(cfile::supports_hole_punching(), plugin_config_exception,
                       "block-log-retain-blocks cannot be greater than 0 because the file system does not support hole "
                       "punching");
            my->chain_config->blog = eosio::chain::prune_blocklog_config{ .prune_blocks = block_log_retain_blocks };
         }
      }

      

      if( options.count( "extract-genesis-json" ) || options.at( "print-genesis-json" ).as<bool>()) {
         std::optional<genesis_state> gs;
         
         gs = block_log::extract_genesis_state( my->blocks_dir, retained_dir );
         EOS_ASSERT( gs,
                     plugin_config_exception,
                     "Block log at '${path}' does not contain a genesis state, it only has the chain-id.",
                     ("path", (my->blocks_dir / "blocks.log").generic_string())
         );
         

         if( options.at( "print-genesis-json" ).as<bool>()) {
            ilog( "Genesis JSON:\n${genesis}", ("genesis", json::to_pretty_string( *gs )));
         }

         if( options.count( "extract-genesis-json" )) {
            auto p = options.at( "extract-genesis-json" ).as<bfs::path>();

            if( p.is_relative()) {
               p = bfs::current_path() / p;
            }

            EOS_ASSERT( fc::json::save_to_file( *gs, p, true ),
                        misc_exception,
                        "Error occurred while writing genesis JSON to '${path}'",
                        ("path", p.generic_string())
            );

            ilog( "Saved genesis JSON to '${path}'", ("path", p.generic_string()) );
         }

         EOS_THROW( extract_genesis_state_exception, "extracted genesis state from blocks.log" );
      }

      if( options.at( "delete-all-blocks" ).as<bool>()) {
         ilog( "Deleting state database and blocks" );
         if( options.at( "truncate-at-block" ).as<uint32_t>() > 0 )
            wlog( "The --truncate-at-block option does not make sense when deleting all blocks." );
         clear_directory_contents( my->chain_config->state_dir );
         clear_directory_contents( my->blocks_dir );
      } else if( options.at( "hard-replay-blockchain" ).as<bool>()) {
         do_hard_replay(options);
      } else if( options.at( "replay-blockchain" ).as<bool>()) {
         ilog( "Replay requested: deleting state database" );
         if( options.at( "truncate-at-block" ).as<uint32_t>() > 0 )
            wlog( "The --truncate-at-block option does not work for a regular replay of the blockchain." );
         clear_directory_contents( my->chain_config->state_dir );
      } else if( options.at( "truncate-at-block" ).as<uint32_t>() > 0 ) {
         wlog( "The --truncate-at-block option can only be used with --hard-replay-blockchain." );
      }

      std::optional<chain_id_type> chain_id;
      if (options.count( "snapshot" )) {
         my->snapshot_path = options.at( "snapshot" ).as<bfs::path>();
         EOS_ASSERT( fc::exists(*my->snapshot_path), plugin_config_exception,
                     "Cannot load snapshot, ${name} does not exist", ("name", my->snapshot_path->generic_string()) );

         // recover genesis information from the snapshot
         // used for validation code below
         auto infile = std::ifstream(my->snapshot_path->generic_string(), (std::ios::in | std::ios::binary));
         istream_snapshot_reader reader(infile);
         reader.validate();
         chain_id = controller::extract_chain_id(reader);
         infile.close();

         EOS_ASSERT( options.count( "genesis-timestamp" ) == 0,
                 plugin_config_exception,
                 "--snapshot is incompatible with --genesis-timestamp as the snapshot contains genesis information");
         EOS_ASSERT( options.count( "genesis-json" ) == 0,
                     plugin_config_exception,
                     "--snapshot is incompatible with --genesis-json as the snapshot contains genesis information");

         auto shared_mem_path = my->chain_config->state_dir / "shared_memory.bin";
         EOS_ASSERT( !fc::is_regular_file(shared_mem_path),
                 plugin_config_exception,
                 "Snapshot can only be used to initialize an empty database." );

         auto block_log_chain_id = block_log::extract_chain_id(my->blocks_dir, retained_dir);

         if (block_log_chain_id) {
            EOS_ASSERT( *chain_id == *block_log_chain_id,
                           plugin_config_exception,
                           "snapshot chain ID (${snapshot_chain_id}) does not match the chain ID (${block_log_chain_id}) in the block log",
                           ("snapshot_chain_id",  *chain_id)
                           ("block_log_chain_id", *block_log_chain_id)
               );
         }

      } else {

         chain_id = controller::extract_chain_id_from_db( my->chain_config->state_dir );

         auto chain_context = block_log::extract_chain_context( my->blocks_dir, retained_dir );
         std::optional<genesis_state> block_log_genesis;
         std::optional<chain_id_type> block_log_chain_id;  

         if (chain_context) {
            std::visit(overloaded {
               [&](const genesis_state& gs) {
                  block_log_genesis = gs;
                  block_log_chain_id = gs.compute_chain_id();
               },
               [&](const chain_id_type& id) {
                  block_log_chain_id = id;
               } 
            }, *chain_context);

            if( chain_id ) {
               EOS_ASSERT( *block_log_chain_id == *chain_id, block_log_exception,
                           "Chain ID in blocks.log (${block_log_chain_id}) does not match the existing "
                           " chain ID in state (${state_chain_id}).",
                           ("block_log_chain_id", *block_log_chain_id)
                           ("state_chain_id", *chain_id)
               );
            } else if (block_log_genesis) {
               ilog( "Starting fresh blockchain state using genesis state extracted from blocks.log." );
               my->genesis = block_log_genesis;
               // Delay setting chain_id until later so that the code handling genesis-json below can know
               // that chain_id still only represents a chain ID extracted from the state (assuming it exists).
            }
         }

         if( options.count( "genesis-json" ) ) {
            bfs::path genesis_file = options.at( "genesis-json" ).as<bfs::path>();
            if( genesis_file.is_relative()) {
               genesis_file = bfs::current_path() / genesis_file;
            }

            EOS_ASSERT( fc::is_regular_file( genesis_file ),
                        plugin_config_exception,
                       "Specified genesis file '${genesis}' does not exist.",
                       ("genesis", genesis_file.generic_string()));

            genesis_state provided_genesis = fc::json::from_file( genesis_file ).as<genesis_state>();

            if( options.count( "genesis-timestamp" ) ) {
               provided_genesis.initial_timestamp = calculate_genesis_timestamp( options.at( "genesis-timestamp" ).as<string>() );

               ilog( "Using genesis state provided in '${genesis}' but with adjusted genesis timestamp",
                     ("genesis", genesis_file.generic_string()) );
            } else {
               ilog( "Using genesis state provided in '${genesis}'", ("genesis", genesis_file.generic_string()));
            }

            if( block_log_genesis ) {
               EOS_ASSERT( *block_log_genesis == provided_genesis, plugin_config_exception,
                           "Genesis state, provided via command line arguments, does not match the existing genesis state"
                           " in blocks.log. It is not necessary to provide genesis state arguments when a full blocks.log "
                           "file already exists."
               );
            } else {
               const auto& provided_genesis_chain_id = provided_genesis.compute_chain_id();
               if( chain_id ) {
                  EOS_ASSERT( provided_genesis_chain_id == *chain_id, plugin_config_exception,
                              "Genesis state, provided via command line arguments, has a chain ID (${provided_genesis_chain_id}) "
                              "that does not match the existing chain ID in the database state (${state_chain_id}). "
                              "It is not necessary to provide genesis state arguments when an initialized database state already exists.",
                              ("provided_genesis_chain_id", provided_genesis_chain_id)
                              ("state_chain_id", *chain_id)
                  );
               } else {
                  if( block_log_chain_id ) {
                     EOS_ASSERT( provided_genesis_chain_id == *block_log_chain_id, plugin_config_exception,
                                 "Genesis state, provided via command line arguments, has a chain ID (${provided_genesis_chain_id}) "
                                 "that does not match the existing chain ID in blocks.log (${block_log_chain_id}).",
                                 ("provided_genesis_chain_id", provided_genesis_chain_id)
                                 ("block_log_chain_id", *block_log_chain_id)
                     );
                  }

                  chain_id = provided_genesis_chain_id;

                  ilog( "Starting fresh blockchain state using provided genesis state." );
                  my->genesis = std::move(provided_genesis);
               }
            }
         } else {
            EOS_ASSERT( options.count( "genesis-timestamp" ) == 0,
                        plugin_config_exception,
                        "--genesis-timestamp is only valid if also passed in with --genesis-json");
         }

         if( !chain_id ) {
            if( my->genesis ) {
               // Uninitialized state database and genesis state extracted from block log
               chain_id = my->genesis->compute_chain_id();
            } else {
               // Uninitialized state database and no genesis state provided

               EOS_ASSERT( !block_log_chain_id, plugin_config_exception,
                           "Genesis state is necessary to initialize fresh blockchain state but genesis state could not be "
                           "found in the blocks log. Please either load from snapshot or find a blocks log that starts "
                           "from genesis."
               );

               ilog( "Starting fresh blockchain state using default genesis state." );
               my->genesis.emplace();
               chain_id = my->genesis->compute_chain_id();
            }
         }
      }

      if ( options.count("read-mode") ) {
         my->chain_config->read_mode = options.at("read-mode").as<db_read_mode>();
      }
      my->api_accept_transactions = options.at( "api-accept-transactions" ).as<bool>();

      if( my->chain_config->read_mode == db_read_mode::IRREVERSIBLE ) {
         if( my->api_accept_transactions ) {
            my->api_accept_transactions = false;
            wlog( "api-accept-transactions set to false due to read-mode: irreversible" );
         }
      }
      if( my->api_accept_transactions ) {
         enable_accept_transactions();
      }

      if ( options.count("validation-mode") ) {
         my->chain_config->block_validation_mode = options.at("validation-mode").as<validation_mode>();
      }

      my->chain_config->db_map_mode = options.at("database-map-mode").as<pinnable_mapped_file::map_mode>();

#ifdef EOSIO_EOS_VM_OC_RUNTIME_ENABLED
      if( options.count("eos-vm-oc-cache-size-mb") )
         my->chain_config->eosvmoc_config.cache_size = options.at( "eos-vm-oc-cache-size-mb" ).as<uint64_t>() * 1024u * 1024u;
      if( options.count("eos-vm-oc-compile-threads") )
         my->chain_config->eosvmoc_config.threads = options.at("eos-vm-oc-compile-threads").as<uint64_t>();
      if( options["eos-vm-oc-enable"].as<bool>() )
         my->chain_config->eosvmoc_tierup = true;
#endif

      my->account_queries_enabled = options.at("enable-account-queries").as<bool>();

      my->chain_config->integrity_hash_on_start = options.at("integrity-hash-on-start").as<bool>();
      my->chain_config->integrity_hash_on_stop = options.at("integrity-hash-on-stop").as<bool>();

      my->chain.emplace( *my->chain_config, std::move(pfs), *chain_id );

      if( options.count( "transaction-retry-max-storage-size-gb" )) {
         EOS_ASSERT( !options.count( "producer-name"), plugin_config_exception,
                     "Transaction retry not allowed on producer nodes." );
         const uint64_t max_storage_size = options.at( "transaction-retry-max-storage-size-gb" ).as<uint64_t>() * 1024 * 1024 * 1024;
         if( max_storage_size > 0 ) {
            const uint32_t p2p_dedup_time_s = options.at( "p2p-dedup-cache-expire-time-sec" ).as<uint32_t>();
            const uint32_t trx_retry_interval = options.at( "transaction-retry-interval-sec" ).as<uint32_t>();
            const uint32_t trx_retry_max_expire = options.at( "transaction-retry-max-expiration-sec" ).as<uint32_t>();
            EOS_ASSERT( trx_retry_interval >= 2 * p2p_dedup_time_s, plugin_config_exception,
                        "transaction-retry-interval-sec ${ri} must be greater than 2 times p2p-dedup-cache-expire-time-sec ${dd}",
                        ("ri", trx_retry_interval)("dd", p2p_dedup_time_s) );
            EOS_ASSERT( trx_retry_max_expire > trx_retry_interval, plugin_config_exception,
                        "transaction-retry-max-expiration-sec ${m} should be configured larger than transaction-retry-interval-sec ${i}",
                        ("m", trx_retry_max_expire)("i", trx_retry_interval) );
            my->_trx_retry_db.emplace( *my->chain, max_storage_size,
                                       fc::seconds(trx_retry_interval), fc::seconds(trx_retry_max_expire),
                                       my->abi_serializer_max_time_us );
         }
      }

      // initialize deep mind logging
      if ( options.at( "deep-mind" ).as<bool>() ) {
         // The actual `fc::dmlog_appender` implementation that is currently used by deep mind
         // logger is using `stdout` to prints it's log line out. Deep mind logging outputs
         // massive amount of data out of the process, which can lead under pressure to some
         // of the system calls (i.e. `fwrite`) to fail abruptly without fully writing the
         // entire line.
         //
         // Recovering from errors on a buffered (line or full) and continuing retrying write
         // is merely impossible to do right, because the buffer is actually held by the
         // underlying `libc` implementation nor the operation system.
         //
         // To ensure good functionalities of deep mind tracer, the `stdout` is made unbuffered
         // and the actual `fc::dmlog_appender` deals with retry when facing error, enabling a much
         // more robust deep mind output.
         //
         // Changing the standard `stdout` behavior from buffered to unbuffered can is disruptive
         // and can lead to weird scenarios in the logging process if `stdout` is used there too.
         //
         // In a future version, the `fc::dmlog_appender` implementation will switch to a `FIFO` file
         // approach, which will remove the dependency on `stdout` and hence this call.
         //
         // For the time being, when `deep-mind = true` is activated, we set `stdout` here to
         // be an unbuffered I/O stream.
         setbuf(stdout, NULL);

         //verify configuration is correct
         EOS_ASSERT( options.at("api-accept-transactions").as<bool>() == false, plugin_config_exception,
            "api-accept-transactions must be set to false in order to enable deep-mind logging.");

         EOS_ASSERT( options.at("p2p-accept-transactions").as<bool>() == false, plugin_config_exception,
            "p2p-accept-transactions must be set to false in order to enable deep-mind logging.");

         my->chain->enable_deep_mind( &_deep_mind_log );
      }

      // set up method providers
      my->get_block_by_number_provider = app().get_method<methods::get_block_by_number>().register_provider(
            [this]( uint32_t block_num ) -> signed_block_ptr {
               return my->chain->fetch_block_by_number( block_num );
            } );

      my->get_block_by_id_provider = app().get_method<methods::get_block_by_id>().register_provider(
            [this]( block_id_type id ) -> signed_block_ptr {
               return my->chain->fetch_block_by_id( id );
            } );

      my->get_head_block_id_provider = app().get_method<methods::get_head_block_id>().register_provider( [this]() {
         return my->chain->head_block_id();
      } );

      my->get_last_irreversible_block_number_provider = app().get_method<methods::get_last_irreversible_block_number>().register_provider(
            [this]() {
               return my->chain->last_irreversible_block_num();
            } );

      // relay signals to channels
      my->pre_accepted_block_connection = my->chain->pre_accepted_block.connect([this](const signed_block_ptr& blk) {
         auto itr = my->loaded_checkpoints.find( blk->block_num() );
         if( itr != my->loaded_checkpoints.end() ) {
            auto id = blk->calculate_id();
            EOS_ASSERT( itr->second == id, checkpoint_exception,
                        "Checkpoint does not match for block number ${num}: expected: ${expected} actual: ${actual}",
                        ("num", blk->block_num())("expected", itr->second)("actual", id)
            );
         }

         my->pre_accepted_block_channel.publish(priority::medium, blk);
      });

      my->accepted_block_header_connection = my->chain->accepted_block_header.connect(
            [this]( const block_state_ptr& blk ) {
               my->accepted_block_header_channel.publish( priority::medium, blk );
            } );

      my->accepted_block_connection = my->chain->accepted_block.connect( [this]( const block_state_ptr& blk ) {
         if (my->_account_query_db) {
            my->_account_query_db->commit_block(blk);
         }

         if (my->_trx_retry_db) {
            my->_trx_retry_db->on_accepted_block(blk);
         }

         if (my->_trx_finality_status_processing) {
            my->_trx_finality_status_processing->signal_accepted_block(blk);
         }

         my->accepted_block_channel.publish( priority::high, blk );
      } );

      my->irreversible_block_connection = my->chain->irreversible_block.connect( [this]( const block_state_ptr& blk ) {
         if (my->_trx_retry_db) {
            my->_trx_retry_db->on_irreversible_block(blk);
         }

         if (my->_trx_finality_status_processing) {
            my->_trx_finality_status_processing->signal_irreversible_block(blk);
         }

         my->irreversible_block_channel.publish( priority::low, blk );
      } );

      my->accepted_transaction_connection = my->chain->accepted_transaction.connect(
            [this]( const transaction_metadata_ptr& meta ) {
               my->accepted_transaction_channel.publish( priority::low, meta );
            } );

      my->applied_transaction_connection = my->chain->applied_transaction.connect(
            [this]( std::tuple<const transaction_trace_ptr&, const packed_transaction_ptr&> t ) {
               if (my->_account_query_db) {
                  my->_account_query_db->cache_transaction_trace(std::get<0>(t));
               }

               if (my->_trx_retry_db) {
                  my->_trx_retry_db->on_applied_transaction(std::get<0>(t), std::get<1>(t));
               }

               if (my->_trx_finality_status_processing) {
                  my->_trx_finality_status_processing->signal_applied_transaction(std::get<0>(t), std::get<1>(t));
               }

               my->applied_transaction_channel.publish( priority::low, std::get<0>(t) );
            } );

      if (my->_trx_finality_status_processing || my->_trx_retry_db) {
         my->block_start_connection = my->chain->block_start.connect(
            [this]( uint32_t block_num ) {
               if (my->_trx_retry_db) {
                  my->_trx_retry_db->on_block_start(block_num);
               }
               if (my->_trx_finality_status_processing) {
                  my->_trx_finality_status_processing->signal_block_start( block_num );
               }
            } );
      }
      my->chain->add_indices();
   } FC_LOG_AND_RETHROW()

}

void chain_plugin::plugin_startup()
{ try {
   EOS_ASSERT( my->chain_config->read_mode != db_read_mode::IRREVERSIBLE || !accept_transactions(), plugin_config_exception,
               "read-mode = irreversible. transactions should not be enabled by enable_accept_transactions" );
   try {
      my->producer_plug = app().find_plugin<producer_plugin>();
      EOS_ASSERT(my->producer_plug, plugin_exception, "Failed to find producer_plugin");

      auto shutdown = [](){ return app().quit(); };
      auto check_shutdown = [](){ return app().is_quiting(); };
      if (my->snapshot_path) {
         auto infile = std::ifstream(my->snapshot_path->generic_string(), (std::ios::in | std::ios::binary));
         auto reader = std::make_shared<istream_snapshot_reader>(infile);
         my->chain->startup(shutdown, check_shutdown, reader);
         infile.close();
      } else if( my->genesis ) {
         my->chain->startup(shutdown, check_shutdown, *my->genesis);
      } else {
         my->chain->startup(shutdown, check_shutdown);
      }
   } catch (const database_guard_exception& e) {
      log_guard_exception(e);
      // make sure to properly close the db
      my->chain.reset();
      throw;
   }

   if(!my->readonly) {
      ilog("starting chain in read/write mode");
   }

   if (my->genesis) {
      ilog("Blockchain started; head block is #${num}, genesis timestamp is ${ts}",
           ("num", my->chain->head_block_num())("ts", (std::string)my->genesis->initial_timestamp));
   }
   else {
      ilog("Blockchain started; head block is #${num}", ("num", my->chain->head_block_num()));
   }

   my->chain_config.reset();

   if (my->account_queries_enabled) {
      my->account_queries_enabled = false;
      try {
         my->_account_query_db.emplace(*my->chain);
         my->account_queries_enabled = true;
      } FC_LOG_AND_DROP(("Unable to enable account queries"));
   }


} FC_CAPTURE_AND_RETHROW() }

void chain_plugin::plugin_shutdown() {
   my->pre_accepted_block_connection.reset();
   my->accepted_block_header_connection.reset();
   my->accepted_block_connection.reset();
   my->irreversible_block_connection.reset();
   my->accepted_transaction_connection.reset();
   my->applied_transaction_connection.reset();
   my->block_start_connection.reset();
   if(app().is_quiting())
      my->chain->get_wasm_interface().indicate_shutting_down();
   my->chain.reset();
}

void chain_plugin::handle_sighup() {
   _deep_mind_log.update_logger( deep_mind_logger_name );
}

chain_apis::read_write::read_write(controller& db,
                                   std::optional<trx_retry_db>& trx_retry,
                                   const fc::microseconds& abi_serializer_max_time,
                                   const fc::microseconds& http_max_response_time,
                                   bool api_accept_transactions)
: db(db)
, trx_retry(trx_retry)
, abi_serializer_max_time(abi_serializer_max_time)
, http_max_response_time(http_max_response_time)
, api_accept_transactions(api_accept_transactions)
{
}

void chain_apis::read_write::validate() const {
   EOS_ASSERT( api_accept_transactions, missing_chain_api_plugin_exception,
               "Not allowed, node has api-accept-transactions = false" );
}

chain_apis::read_write chain_plugin::get_read_write_api(const fc::microseconds& http_max_response_time) {
   return chain_apis::read_write(chain(), my->_trx_retry_db, get_abi_serializer_max_time(), http_max_response_time, api_accept_transactions());
}

chain_apis::read_only chain_plugin::get_read_only_api(const fc::microseconds& http_max_response_time) const {
   return chain_apis::read_only(chain(), my->_account_query_db, get_abi_serializer_max_time(), http_max_response_time, my->producer_plug, my->_trx_finality_status_processing.get());
}


bool chain_plugin::accept_block(const signed_block_ptr& block, const block_id_type& id, const block_state_ptr& bsp ) {
   return my->incoming_block_sync_method(block, id, bsp);
}

void chain_plugin::accept_transaction(const chain::packed_transaction_ptr& trx, next_function<chain::transaction_trace_ptr> next) {
   my->incoming_transaction_async_method(trx, false, transaction_metadata::trx_type::input, false, std::move(next));
}

controller& chain_plugin::chain() { return *my->chain; }
const controller& chain_plugin::chain() const { return *my->chain; }

chain::chain_id_type chain_plugin::get_chain_id()const {
   return my->chain->get_chain_id();
}

fc::microseconds chain_plugin::get_abi_serializer_max_time() const {
   return my->abi_serializer_max_time_us;
}

bool chain_plugin::api_accept_transactions() const{
   return my->api_accept_transactions;
}

bool chain_plugin::accept_transactions() const {
   return my->accept_transactions;
}

void chain_plugin::enable_accept_transactions() {
   my->accept_transactions = true;
}


void chain_plugin::log_guard_exception(const chain::guard_exception&e ) {
   if (e.code() == chain::database_guard_exception::code_value) {
      elog("Database has reached an unsafe level of usage, shutting down to avoid corrupting the database.  "
           "Please increase the value set for \"chain-state-db-size-mb\" and restart the process!");
   }

   dlog("Details: ${details}", ("details", e.to_detail_string()));
}

void chain_plugin::handle_guard_exception(const chain::guard_exception& e) {
   log_guard_exception(e);

   elog("database chain::guard_exception, quitting..."); // log string searched for in: tests/nodeos_under_min_avail_ram.py
   // quit the app
   app().quit();
}

void chain_plugin::handle_db_exhaustion() {
   elog("database memory exhausted: increase chain-state-db-size-mb");
   //return 1 -- it's what programs/nodeos/main.cpp considers "BAD_ALLOC"
   std::_Exit(1);
}

void chain_plugin::handle_bad_alloc() {
   elog("std::bad_alloc - memory exhausted");
   //return -2 -- it's what programs/nodeos/main.cpp reports for std::exception
   std::_Exit(-2);
}

bool chain_plugin::account_queries_enabled() const {
   return my->account_queries_enabled;
}

bool chain_plugin::transaction_finality_status_enabled() const {
   return my->_trx_finality_status_processing.get();
}

namespace chain_apis {

const string read_only::KEYi64 = "i64";

template<typename I>
std::string itoh(I n, size_t hlen = sizeof(I)<<1) {
   static const char* digits = "0123456789abcdef";
   std::string r(hlen, '0');
   for(size_t i = 0, j = (hlen - 1) * 4 ; i < hlen; ++i, j -= 4)
      r[i] = digits[(n>>j) & 0x0f];
   return r;
}

read_only::get_info_results read_only::get_info(const read_only::get_info_params&, const fc::time_point&) const {
   const auto& rm = db.get_resource_limits_manager();

   return {
      itoh(static_cast<uint32_t>(app().version())),
      db.get_chain_id(),
      db.head_block_num(),
      db.last_irreversible_block_num(),
      db.last_irreversible_block_id(),
      db.head_block_id(),
      db.head_block_time(),
      db.head_block_producer(),
      rm.get_virtual_block_cpu_limit(),
      rm.get_virtual_block_net_limit(),
      rm.get_block_cpu_limit(),
      rm.get_block_net_limit(),
      //std::bitset<64>(db.get_dynamic_global_properties().recent_slots_filled).to_string(),
      //__builtin_popcountll(db.get_dynamic_global_properties().recent_slots_filled) / 64.0,
      app().version_string(),
      db.fork_db_head_block_num(),
      db.fork_db_head_block_id(),
      app().full_version_string(),
      rm.get_total_cpu_weight(),
      rm.get_total_net_weight(),
      db.earliest_available_block_num(),
      db.last_irreversible_block_time()
   };
}

read_only::get_transaction_status_results
read_only::get_transaction_status(const read_only::get_transaction_status_params& param, const fc::time_point& deadline) const {
   EOS_ASSERT(trx_finality_status_proc, unsupported_feature, "Transaction Status Interface not enabled.  To enable, configure nodeos with '--transaction-finality-status-max-storage-size-gb <size>'.");

   trx_finality_status_processing::chain_state ch_state = trx_finality_status_proc->get_chain_state();

   const auto trx_st = trx_finality_status_proc->get_trx_state(param.id);
   // check if block_id is set to a valid value, since trx_finality_status_proc does not use optionals for the block data
   const auto trx_block_valid = trx_st && trx_st->block_id != chain::block_id_type{};

   return {
      trx_st ? trx_st->status : "UNKNOWN",
      trx_block_valid ? std::optional<uint32_t>(chain::block_header::num_from_id(trx_st->block_id)) : std::optional<uint32_t>{},
      trx_block_valid ? std::optional<chain::block_id_type>(trx_st->block_id) : std::optional<chain::block_id_type>{},
      trx_block_valid ? std::optional<fc::time_point>(trx_st->block_timestamp) : std::optional<fc::time_point>{},
      trx_st ? std::optional<fc::time_point_sec>(trx_st->expiration) : std::optional<fc::time_point_sec>{},
      chain::block_header::num_from_id(ch_state.head_id),
      ch_state.head_id,
      ch_state.head_block_timestamp,
      chain::block_header::num_from_id(ch_state.irr_id),
      ch_state.irr_id,
      ch_state.irr_block_timestamp,
      ch_state.earliest_tracked_block_id,
      chain::block_header::num_from_id(ch_state.earliest_tracked_block_id)
   };
}

read_only::get_activated_protocol_features_results
read_only::get_activated_protocol_features( const read_only::get_activated_protocol_features_params& params,
                                            const fc::time_point& deadline )const {
   read_only::get_activated_protocol_features_results result;
   const auto& pfm = db.get_protocol_feature_manager();

   uint32_t lower_bound_value = std::numeric_limits<uint32_t>::lowest();
   uint32_t upper_bound_value = std::numeric_limits<uint32_t>::max();

   if( params.lower_bound ) {
      lower_bound_value = *params.lower_bound;
   }

   if( params.upper_bound ) {
      upper_bound_value = *params.upper_bound;
   }

   if( upper_bound_value < lower_bound_value )
      return result;

   fc::microseconds params_time_limit = params.time_limit_ms ? fc::milliseconds(*params.time_limit_ms) : fc::milliseconds(10);
   fc::time_point params_deadline = fc::time_point::now() + params_time_limit;

   auto walk_range = [&]( auto itr, auto end_itr, auto&& convert_iterator ) {
      fc::mutable_variant_object mvo;
      mvo( "activation_ordinal", 0 );
      mvo( "activation_block_num", 0 );

      auto& activation_ordinal_value   = mvo["activation_ordinal"];
      auto& activation_block_num_value = mvo["activation_block_num"];

      auto cur_time = fc::time_point::now();
      for( unsigned int count = 0;
           cur_time <= params_deadline && count < params.limit && itr != end_itr;
           ++itr, cur_time = fc::time_point::now() )
      {
         FC_CHECK_DEADLINE(deadline);
         const auto& conv_itr = convert_iterator( itr );
         activation_ordinal_value   = conv_itr.activation_ordinal();
         activation_block_num_value = conv_itr.activation_block_num();

         result.activated_protocol_features.emplace_back( conv_itr->to_variant( false, &mvo ) );
         ++count;
      }
      if( itr != end_itr ) {
         result.more = convert_iterator( itr ).activation_ordinal() ;
      }
   };

   auto get_next_if_not_end = [&pfm]( auto&& itr ) {
      if( itr == pfm.cend() ) return itr;

      ++itr;
      return itr;
   };

   auto lower = ( params.search_by_block_num ? pfm.lower_bound( lower_bound_value )
                                             : pfm.at_activation_ordinal( lower_bound_value ) );

   auto upper = ( params.search_by_block_num ? pfm.upper_bound( upper_bound_value )
                                             : get_next_if_not_end( pfm.at_activation_ordinal( upper_bound_value ) ) );

   if( params.reverse ) {
      walk_range( std::make_reverse_iterator(upper), std::make_reverse_iterator(lower),
                  []( auto&& ritr ) { return --(ritr.base()); } );
   } else {
      walk_range( lower, upper, []( auto&& itr ) { return itr; } );
   }

   return result;
}

uint64_t read_only::get_table_index_name(const read_only::get_table_rows_params& p, bool& primary) {
   using boost::algorithm::starts_with;
   // see multi_index packing of index name
   const uint64_t table = p.table.to_uint64_t();
   uint64_t index = table & 0xFFFFFFFFFFFFFFF0ULL;
   EOS_ASSERT( index == table, chain::contract_table_query_exception, "Unsupported table name: ${n}", ("n", p.table) );

   primary = false;
   uint64_t pos = 0;
   if (p.index_position.empty() || p.index_position == "first" || p.index_position == "primary" || p.index_position == "one") {
      primary = true;
   } else if (starts_with(p.index_position, "sec") || p.index_position == "two") { // second, secondary
   } else if (starts_with(p.index_position , "ter") || starts_with(p.index_position, "th")) { // tertiary, ternary, third, three
      pos = 1;
   } else if (starts_with(p.index_position, "fou")) { // four, fourth
      pos = 2;
   } else if (starts_with(p.index_position, "fi")) { // five, fifth
      pos = 3;
   } else if (starts_with(p.index_position, "six")) { // six, sixth
      pos = 4;
   } else if (starts_with(p.index_position, "sev")) { // seven, seventh
      pos = 5;
   } else if (starts_with(p.index_position, "eig")) { // eight, eighth
      pos = 6;
   } else if (starts_with(p.index_position, "nin")) { // nine, ninth
      pos = 7;
   } else if (starts_with(p.index_position, "ten")) { // ten, tenth
      pos = 8;
   } else {
      try {
         pos = fc::to_uint64( p.index_position );
      } catch(...) {
         EOS_ASSERT( false, chain::contract_table_query_exception, "Invalid index_position: ${p}", ("p", p.index_position));
      }
      if (pos < 2) {
         primary = true;
         pos = 0;
      } else {
         pos -= 2;
      }
   }
   index |= (pos & 0x000000000000000FULL);
   return index;
}

uint64_t convert_to_type(const eosio::name &n, const string &desc) {
   return n.to_uint64_t();
}

template<>
uint64_t convert_to_type(const string& str, const string& desc) {

   try {
      return boost::lexical_cast<uint64_t>(str.c_str(), str.size());
   } catch( ... ) { }

   try {
      auto trimmed_str = str;
      boost::trim(trimmed_str);
      name s(trimmed_str);
      return s.to_uint64_t();
   } catch( ... ) { }

   if (str.find(',') != string::npos) { // fix #6274 only match formats like 4,EOS
      try {
         auto symb = eosio::chain::symbol::from_string(str);
         return symb.value();
      } catch( ... ) { }
   }

   try {
      return ( eosio::chain::string_to_symbol( 0, str.c_str() ) >> 8 );
   } catch( ... ) {
      EOS_ASSERT( false, chain_type_exception, "Could not convert ${desc} string '${str}' to any of the following: "
                        "uint64_t, valid name, or valid symbol (with or without the precision)",
                  ("desc", desc)("str", str));
   }
}

template<>
double convert_to_type(const string& str, const string& desc) {
   double val{};
   try {
      val = fc::variant(str).as<double>();
   } FC_RETHROW_EXCEPTIONS(warn, "Could not convert ${desc} string '${str}' to key type.", ("desc", desc)("str",str) )

   EOS_ASSERT( !std::isnan(val), chain::contract_table_query_exception,
               "Converted ${desc} string '${str}' to NaN which is not a permitted value for the key type", ("desc", desc)("str",str) );

   return val;
}

template<typename Type>
string convert_to_string(const Type& source, const string& key_type, const string& encode_type, const string& desc) {
   try {
      return fc::variant(source).as<string>();
   } FC_RETHROW_EXCEPTIONS(warn, "Could not convert ${desc} from '${source}' to string.", ("desc", desc)("source",source) )
}

template<>
string convert_to_string(const chain::key256_t& source, const string& key_type, const string& encode_type, const string& desc) {
   try {
      if (key_type == chain_apis::sha256 || (key_type == chain_apis::i256 && encode_type == chain_apis::hex)) {
         auto byte_array = fixed_bytes<32>(source).extract_as_byte_array();
         fc::sha256 val(reinterpret_cast<char *>(byte_array.data()), byte_array.size());
         return std::string(val);
      } else if (key_type == chain_apis::i256) {
         auto byte_array = fixed_bytes<32>(source).extract_as_byte_array();
         fc::sha256 val(reinterpret_cast<char *>(byte_array.data()), byte_array.size());
         return std::string("0x") + std::string(val);
      } else if (key_type == chain_apis::ripemd160) {
         auto byte_array = fixed_bytes<20>(source).extract_as_byte_array();
         fc::ripemd160 val;
         memcpy(val._hash, byte_array.data(), byte_array.size() );
         return std::string(val);
      }
      EOS_ASSERT( false, chain_type_exception, "Incompatible key_type and encode_type for key256_t next_key" );

   } FC_RETHROW_EXCEPTIONS(warn, "Could not convert ${desc} source '${source}' to string.", ("desc", desc)("source",source) )
}

template<>
string convert_to_string(const float128_t& source, const string& key_type, const string& encode_type, const string& desc) {
   try {
      float64_t f = f128_to_f64(source);
      return fc::variant(f).as<string>();
   } FC_RETHROW_EXCEPTIONS(warn, "Could not convert ${desc} from '${source}' to string.", ("desc", desc)("source",source) )
}

abi_def get_abi( const controller& db, const name& account ) {
   const auto &d = db.db();
   const account_object *code_accnt = d.find<account_object, by_name>(account);
   EOS_ASSERT(code_accnt != nullptr, chain::account_query_exception, "Fail to retrieve account for ${account}", ("account", account) );
   abi_def abi;
   abi_serializer::to_abi(code_accnt->abi, abi);
   return abi;
}

string get_table_type( const abi_def& abi, const name& table_name ) {
   for( const auto& t : abi.tables ) {
      if( t.name == table_name ){
         return t.index_type;
      }
   }
   EOS_ASSERT( false, chain::contract_table_query_exception, "Table ${table} is not specified in the ABI", ("table",table_name) );
}

read_only::get_table_rows_result read_only::get_table_rows( const read_only::get_table_rows_params& p, const fc::time_point& deadline )const {
   abi_def abi = eosio::chain_apis::get_abi( db, p.code );
   bool primary = false;
   auto table_with_index = get_table_index_name( p, primary );
   if( primary ) {
      EOS_ASSERT( p.table == table_with_index, chain::contract_table_query_exception, "Invalid table name ${t}", ( "t", p.table ));
      auto table_type = get_table_type( abi, p.table );
      if( table_type == KEYi64 || p.key_type == "i64" || p.key_type == "name" ) {
         return get_table_rows_ex<key_value_index>(p,std::move(abi),deadline);
      }
      EOS_ASSERT( false, chain::contract_table_query_exception,  "Invalid table type ${type}", ("type",table_type)("abi",abi));
   } else {
      EOS_ASSERT( !p.key_type.empty(), chain::contract_table_query_exception, "key type required for non-primary index" );

      if (p.key_type == chain_apis::i64 || p.key_type == "name") {
         return get_table_rows_by_seckey<index64_index, uint64_t>(p, std::move(abi), deadline, [](uint64_t v)->uint64_t {
            return v;
         });
      }
      else if (p.key_type == chain_apis::i128) {
         return get_table_rows_by_seckey<index128_index, uint128_t>(p, std::move(abi), deadline, [](uint128_t v)->uint128_t {
            return v;
         });
      }
      else if (p.key_type == chain_apis::i256) {
         if ( p.encode_type == chain_apis::hex) {
            using  conv = keytype_converter<chain_apis::sha256,chain_apis::hex>;
            return get_table_rows_by_seckey<conv::index_type, conv::input_type>(p, std::move(abi), deadline, conv::function());
         }
         using  conv = keytype_converter<chain_apis::i256>;
         return get_table_rows_by_seckey<conv::index_type, conv::input_type>(p, std::move(abi), deadline, conv::function());
      }
      else if (p.key_type == chain_apis::float64) {
         return get_table_rows_by_seckey<index_double_index, double>(p, std::move(abi), deadline, [](double v)->float64_t {
            float64_t f;
            double_to_float64(v, f);
            return f;
         });
      }
      else if (p.key_type == chain_apis::float128) {
         if ( p.encode_type == chain_apis::hex) {
            return get_table_rows_by_seckey<index_long_double_index, uint128_t>(p, std::move(abi), deadline, [](uint128_t v)->float128_t{
               float128_t f;
               uint128_to_float128(v, f);
               return f;
            });
         }
         return get_table_rows_by_seckey<index_long_double_index, double>(p, std::move(abi), deadline, [](double v)->float128_t{
            float64_t f;
            double_to_float64(v, f);
            float128_t f128;
            f64_to_f128M(f, &f128);
            return f128;
         });
      }
      else if (p.key_type == chain_apis::sha256) {
         using  conv = keytype_converter<chain_apis::sha256,chain_apis::hex>;
         return get_table_rows_by_seckey<conv::index_type, conv::input_type>(p, std::move(abi), deadline, conv::function());
      }
      else if(p.key_type == chain_apis::ripemd160) {
         using  conv = keytype_converter<chain_apis::ripemd160,chain_apis::hex>;
         return get_table_rows_by_seckey<conv::index_type, conv::input_type>(p, std::move(abi), deadline, conv::function());
      }
      EOS_ASSERT(false, chain::contract_table_query_exception,  "Unsupported secondary index type: ${t}", ("t", p.key_type));
   }
}

read_only::get_table_by_scope_result read_only::get_table_by_scope( const read_only::get_table_by_scope_params& p,
                                                                    const fc::time_point& deadline )const {

   fc::microseconds params_time_limit = p.time_limit_ms ? fc::milliseconds(*p.time_limit_ms) : fc::milliseconds(10);
   fc::time_point params_deadline = fc::time_point::now() + params_time_limit;

   read_only::get_table_by_scope_result result;
   const auto& d = db.db();

   const auto& idx = d.get_index<chain::table_id_multi_index, chain::by_code_scope_table>();
   auto lower_bound_lookup_tuple = std::make_tuple( p.code, name(std::numeric_limits<uint64_t>::lowest()), p.table );
   auto upper_bound_lookup_tuple = std::make_tuple( p.code, name(std::numeric_limits<uint64_t>::max()),
                                                    (p.table.empty() ? name(std::numeric_limits<uint64_t>::max()) : p.table) );

   if( p.lower_bound.size() ) {
      uint64_t scope = convert_to_type<uint64_t>(p.lower_bound, "lower_bound scope");
      std::get<1>(lower_bound_lookup_tuple) = name(scope);
   }

   if( p.upper_bound.size() ) {
      uint64_t scope = convert_to_type<uint64_t>(p.upper_bound, "upper_bound scope");
      std::get<1>(upper_bound_lookup_tuple) = name(scope);
   }

   if( upper_bound_lookup_tuple < lower_bound_lookup_tuple )
      return result;

   auto walk_table_range = [&]( auto itr, auto end_itr ) {
      auto cur_time = fc::time_point::now();
      for( unsigned int count = 0; cur_time <= params_deadline && count < p.limit && itr != end_itr; ++itr, cur_time = fc::time_point::now() ) {
         FC_CHECK_DEADLINE(deadline);
         if( p.table && itr->table != p.table ) continue;

         result.rows.push_back( {itr->code, itr->scope, itr->table, itr->payer, itr->count} );

         ++count;
      }
      if( itr != end_itr ) {
         result.more = itr->scope.to_string();
      }
   };

   auto lower = idx.lower_bound( lower_bound_lookup_tuple );
   auto upper = idx.upper_bound( upper_bound_lookup_tuple );
   if( p.reverse && *p.reverse ) {
      walk_table_range( boost::make_reverse_iterator(upper), boost::make_reverse_iterator(lower) );
   } else {
      walk_table_range( lower, upper );
   }

   return result;
}

vector<asset> read_only::get_currency_balance( const read_only::get_currency_balance_params& p, const fc::time_point& deadline )const {

   const abi_def abi = eosio::chain_apis::get_abi( db, p.code );
   (void)get_table_type( abi, name("accounts") );

   vector<asset> results;
   walk_key_value_table(p.code, p.account, "accounts"_n, [&](const key_value_object& obj){
      EOS_ASSERT( obj.value.size() >= sizeof(asset), chain::asset_type_exception, "Invalid data on table");

      asset cursor;
      fc::datastream<const char *> ds(obj.value.data(), obj.value.size());
      fc::raw::unpack(ds, cursor);

      EOS_ASSERT( cursor.get_symbol().valid(), chain::asset_type_exception, "Invalid asset");

      if( !p.symbol || boost::iequals(cursor.symbol_name(), *p.symbol) ) {
        results.emplace_back(cursor);
      }

      // return false if we are looking for one and found it, true otherwise
      return !(p.symbol && boost::iequals(cursor.symbol_name(), *p.symbol));
   });

   return results;
}

fc::variant read_only::get_currency_stats( const read_only::get_currency_stats_params& p, const fc::time_point& deadline )const {
   fc::mutable_variant_object results;

   const abi_def abi = eosio::chain_apis::get_abi( db, p.code );
   (void)get_table_type( abi, name("stat") );

   uint64_t scope = ( eosio::chain::string_to_symbol( 0, boost::algorithm::to_upper_copy(p.symbol).c_str() ) >> 8 );

   walk_key_value_table(p.code, name(scope), "stat"_n, [&](const key_value_object& obj){
      EOS_ASSERT( obj.value.size() >= sizeof(read_only::get_currency_stats_result), chain::asset_type_exception, "Invalid data on table");

      fc::datastream<const char *> ds(obj.value.data(), obj.value.size());
      read_only::get_currency_stats_result result;

      fc::raw::unpack(ds, result.supply);
      fc::raw::unpack(ds, result.max_supply);
      fc::raw::unpack(ds, result.issuer);

      results[result.supply.symbol_name()] = result;
      return true;
   });

   return results;
}

fc::variant get_global_row( const database& db, const abi_def& abi, const abi_serializer& abis, const fc::microseconds& abi_serializer_max_time_us, bool shorten_abi_errors ) {
   const auto table_type = get_table_type(abi, "global"_n);
   EOS_ASSERT(table_type == read_only::KEYi64, chain::contract_table_query_exception, "Invalid table type ${type} for table global", ("type",table_type));

   const auto* const table_id = db.find<chain::table_id_object, chain::by_code_scope_table>(boost::make_tuple(config::system_account_name, config::system_account_name, "global"_n));
   EOS_ASSERT(table_id, chain::contract_table_query_exception, "Missing table global");

   const auto& kv_index = db.get_index<key_value_index, by_scope_primary>();
   const auto it = kv_index.find(boost::make_tuple(table_id->id, "global"_n.to_uint64_t()));
   EOS_ASSERT(it != kv_index.end(), chain::contract_table_query_exception, "Missing row in table global");

   vector<char> data;
   read_only::copy_inline_row(*it, data);
   return abis.binary_to_variant(abis.get_table_type("global"_n), data, abi_serializer::create_yield_function( abi_serializer_max_time_us ), shorten_abi_errors );
}

read_only::get_producers_result
read_only::get_producers( const read_only::get_producers_params& params, const fc::time_point& deadline ) const try {
   abi_def abi = eosio::chain_apis::get_abi(db, config::system_account_name);
   const auto table_type = get_table_type(abi, "producers"_n);
   const abi_serializer abis{ abi_def(abi), abi_serializer::create_yield_function( abi_serializer_max_time ) };
   EOS_ASSERT(table_type == KEYi64, chain::contract_table_query_exception, "Invalid table type ${type} for table producers", ("type",table_type));

   const auto& d = db.db();
   const auto lower = name{params.lower_bound};

   static const uint8_t secondary_index_num = 0;
   const auto* const table_id = d.find<chain::table_id_object, chain::by_code_scope_table>(
           boost::make_tuple(config::system_account_name, config::system_account_name, "producers"_n));
   const auto* const secondary_table_id = d.find<chain::table_id_object, chain::by_code_scope_table>(
           boost::make_tuple(config::system_account_name, config::system_account_name, name("producers"_n.to_uint64_t() | secondary_index_num)));
   EOS_ASSERT(table_id && secondary_table_id, chain::contract_table_query_exception, "Missing producers table");

   const auto& kv_index = d.get_index<key_value_index, by_scope_primary>();
   const auto& secondary_index = d.get_index<index_double_index>().indices();
   const auto& secondary_index_by_primary = secondary_index.get<by_primary>();
   const auto& secondary_index_by_secondary = secondary_index.get<by_secondary>();

   read_only::get_producers_result result;
   vector<char> data;

   auto it = [&]{
      if(lower.to_uint64_t() == 0)
         return secondary_index_by_secondary.lower_bound(
            boost::make_tuple(secondary_table_id->id, to_softfloat64(std::numeric_limits<double>::lowest()), 0));
      else
         return secondary_index.project<by_secondary>(
            secondary_index_by_primary.lower_bound(
               boost::make_tuple(secondary_table_id->id, lower.to_uint64_t())));
   }();

   fc::microseconds params_time_limit = params.time_limit_ms ? fc::milliseconds(*params.time_limit_ms) : fc::milliseconds(10);
   fc::time_point params_deadline = fc::time_point::now() + params_time_limit;

   for( ; it != secondary_index_by_secondary.end() && it->t_id == secondary_table_id->id; ++it ) {
      FC_CHECK_DEADLINE(deadline);
      if (result.rows.size() >= params.limit || fc::time_point::now() > params_deadline) {
         result.more = name{it->primary_key}.to_string();
         break;
      }
      copy_inline_row(*kv_index.find(boost::make_tuple(table_id->id, it->primary_key)), data);
      if (params.json)
         result.rows.emplace_back( abis.binary_to_variant( abis.get_table_type("producers"_n), data, abi_serializer::create_yield_function( abi_serializer_max_time ), shorten_abi_errors ) );
      else
         result.rows.emplace_back(fc::variant(data));
   }

   result.total_producer_vote_weight = get_global_row(d, abi, abis, abi_serializer_max_time, shorten_abi_errors)["total_producer_vote_weight"].as_double();
   return result;
} catch (...) {
   read_only::get_producers_result result;
   result.rows.reserve(db.active_producers().producers.size());

   for (const auto& p : db.active_producers().producers) {
      auto row = fc::mutable_variant_object()
         ("owner", p.producer_name)
         ("producer_authority", p.authority)
         ("url", "")
         ("total_votes", 0.0f);

      // detect a legacy key and maintain API compatibility for those entries
      if (std::holds_alternative<block_signing_authority_v0>(p.authority)) {
         const auto& auth = std::get<block_signing_authority_v0>(p.authority);
         if (auth.keys.size() == 1 && auth.keys.back().weight == auth.threshold) {
            row("producer_key", auth.keys.back().key);
         }
      }

      result.rows.emplace_back(std::move(row));
   }

   return result;
}

read_only::get_producer_schedule_result read_only::get_producer_schedule( const read_only::get_producer_schedule_params& p, const fc::time_point& deadline ) const {
   read_only::get_producer_schedule_result result;
   to_variant(db.active_producers(), result.active);
   if(!db.pending_producers().producers.empty())
      to_variant(db.pending_producers(), result.pending);
   auto proposed = db.proposed_producers();
   if(proposed && !proposed->producers.empty())
      to_variant(*proposed, result.proposed);
   return result;
}


auto make_resolver(const controller& control, abi_serializer::yield_function_t yield) {
   return [&control, yield{std::move(yield)}](const account_name &name) -> std::optional<abi_serializer> {
      const auto* accnt = control.db().template find<account_object, by_name>(name);
      if (accnt != nullptr) {
         if (abi_def abi; abi_serializer::to_abi(accnt->abi, abi)) {
            return abi_serializer(std::move(abi), yield);
         }
      }
      return {};
   };
}

read_only::get_scheduled_transactions_result
read_only::get_scheduled_transactions( const read_only::get_scheduled_transactions_params& p, const fc::time_point& deadline ) const {

   fc::microseconds params_time_limit = p.time_limit_ms ? fc::milliseconds(*p.time_limit_ms) : fc::milliseconds(10);
   fc::time_point params_deadline = fc::time_point::now() + params_time_limit;

   const auto& d = db.db();

   const auto& idx_by_delay = d.get_index<generated_transaction_multi_index,by_delay>();
   auto itr = ([&](){
      if (!p.lower_bound.empty()) {
         try {
            auto when = time_point::from_iso_string( p.lower_bound );
            return idx_by_delay.lower_bound(boost::make_tuple(when));
         } catch (...) {
            try {
               auto txid = transaction_id_type(p.lower_bound);
               const auto& by_txid = d.get_index<generated_transaction_multi_index,by_trx_id>();
               auto itr = by_txid.find( txid );
               if (itr == by_txid.end()) {
                  EOS_THROW(transaction_exception, "Unknown Transaction ID: ${txid}", ("txid", txid));
               }

               return d.get_index<generated_transaction_multi_index>().indices().project<by_delay>(itr);

            } catch (...) {
               return idx_by_delay.end();
            }
         }
      } else {
         return idx_by_delay.begin();
      }
   })();

   read_only::get_scheduled_transactions_result result;

   auto resolver = make_resolver(db, abi_serializer::create_yield_function( abi_serializer_max_time ));

   uint32_t remaining = p.limit;
   while (itr != idx_by_delay.end() && remaining > 0 && params_deadline > fc::time_point::now()) {
      FC_CHECK_DEADLINE(deadline);
      auto row = fc::mutable_variant_object()
              ("trx_id", itr->trx_id)
              ("sender", itr->sender)
              ("sender_id", itr->sender_id)
              ("payer", itr->payer)
              ("delay_until", itr->delay_until)
              ("expiration", itr->expiration)
              ("published", itr->published)
      ;

      if (p.json) {
         fc::variant pretty_transaction;

         transaction trx;
         fc::datastream<const char*> ds( itr->packed_trx.data(), itr->packed_trx.size() );
         fc::raw::unpack(ds,trx);

         abi_serializer::to_variant(trx, pretty_transaction, resolver, abi_serializer::create_yield_function( abi_serializer_max_time ));
         row("transaction", pretty_transaction);
      } else {
         auto packed_transaction = bytes(itr->packed_trx.begin(), itr->packed_trx.end());
         row("transaction", packed_transaction);
      }

      result.transactions.emplace_back(std::move(row));
      ++itr;
      remaining--;
   }

   if (itr != idx_by_delay.end()) {
      result.more = string(itr->trx_id);
   }

   return result;
}

chain::signed_block_ptr read_only::get_raw_block(const read_only::get_raw_block_params& params, const fc::time_point& deadline) const {
   signed_block_ptr block;
   std::optional<uint64_t> block_num;

   EOS_ASSERT( !params.block_num_or_id.empty() && params.block_num_or_id.size() <= 64,
               chain::block_id_type_exception,
               "Invalid Block number or ID, must be greater than 0 and less than 65 characters"
   );

   try {
      block_num = fc::to_uint64(params.block_num_or_id);
   } catch( ... ) {}

   if( block_num ) {
      block = db.fetch_block_by_number( *block_num );
   } else {
      try {
         block = db.fetch_block_by_id( fc::variant(params.block_num_or_id).as<block_id_type>() );
      } EOS_RETHROW_EXCEPTIONS(chain::block_id_type_exception, "Invalid block ID: ${block_num_or_id}", ("block_num_or_id", params.block_num_or_id))
   }

   EOS_ASSERT( block, unknown_block_exception, "Could not find block: ${block}", ("block", params.block_num_or_id));
   FC_CHECK_DEADLINE(deadline);

   return block;
}

read_only::get_block_header_result read_only::get_block_header(const read_only::get_block_header_params& params, const fc::time_point& deadline) const{
   std::optional<uint64_t> block_num;

   EOS_ASSERT( !params.block_num_or_id.empty() && params.block_num_or_id.size() <= 64,
               chain::block_id_type_exception,
               "Invalid Block number or ID, must be greater than 0 and less than 65 characters"
   );

   try {
      block_num = fc::to_uint64(params.block_num_or_id);
   } catch( ... ) {}

   if (!params.include_extensions) {
      std::optional<signed_block_header> header;

      if( block_num ) {
         header = db.fetch_block_header_by_number( *block_num );
      } else {
         try {
            header = db.fetch_block_header_by_id( fc::variant(params.block_num_or_id).as<block_id_type>() );
         } EOS_RETHROW_EXCEPTIONS(chain::block_id_type_exception, "Invalid block ID: ${block_num_or_id}", ("block_num_or_id", params.block_num_or_id))
      }
      EOS_ASSERT( header, unknown_block_exception, "Could not find block header: ${block}", ("block", params.block_num_or_id));
      return { header->calculate_id(), fc::variant{*header}, {}};
   } else {
      signed_block_ptr block;
      if( block_num ) {
         block = db.fetch_block_by_number( *block_num );
      } else {
         try {
            block = db.fetch_block_by_id( fc::variant(params.block_num_or_id).as<block_id_type>() );
         } EOS_RETHROW_EXCEPTIONS(chain::block_id_type_exception, "Invalid block ID: ${block_num_or_id}", ("block_num_or_id", params.block_num_or_id))
      }
      EOS_ASSERT( block, unknown_block_exception, "Could not find block header: ${block}", ("block", params.block_num_or_id));
      return { block->calculate_id(), fc::variant{static_cast<signed_block_header>(*block)}, block->block_extensions};
   }

   FC_CHECK_DEADLINE(deadline);

}

std::unordered_map<account_name, std::optional<abi_serializer>>
read_only::get_block_serializers( const chain::signed_block_ptr& block, const fc::microseconds& max_time ) const {
   auto yield = abi_serializer::create_yield_function( max_time );
   auto resolver = make_resolver(db, yield );
   std::unordered_map<account_name, std::optional<abi_serializer> > abi_cache;
   auto add_to_cache = [&]( const chain::action& a ) {
      auto it = abi_cache.find( a.account );
      if( it == abi_cache.end() ) {
         try {
            abi_cache.emplace_hint( it, a.account, resolver( a.account ) );
         } catch( ... ) {
            // keep behavior of not throwing on invalid abi, will result in hex data
         }
      }
   };
   for( const auto& receipt: block->transactions ) {
      if( std::holds_alternative<chain::packed_transaction>( receipt.trx ) ) {
         const auto& pt = std::get<chain::packed_transaction>( receipt.trx );
         const auto& t = pt.get_transaction();
         for( const auto& a: t.actions )
            add_to_cache( a );
         for( const auto& a: t.context_free_actions )
            add_to_cache( a );
      }
   }
   return abi_cache;
}

fc::variant read_only::convert_block( const chain::signed_block_ptr& block,
                                      std::unordered_map<account_name, std::optional<abi_serializer>> abi_cache,
                                      const fc::microseconds& max_time ) const {

   auto abi_serializer_resolver = [&abi_cache](const account_name& account) -> std::optional<abi_serializer> {
      auto it = abi_cache.find( account );
      if( it != abi_cache.end() )
         return it->second;
      return {};
   };

   fc::variant pretty_output;
   abi_serializer::to_variant( *block, pretty_output, abi_serializer_resolver,
                               abi_serializer::create_yield_function( max_time ) );

   const auto block_id = block->calculate_id();
   uint32_t ref_block_prefix = block_id._hash[1];

   return fc::mutable_variant_object( pretty_output.get_object() )
         ( "id", block_id )
         ( "block_num", block->block_num() )
         ( "ref_block_prefix", ref_block_prefix );
}

fc::variant read_only::get_block_info(const read_only::get_block_info_params& params, const fc::time_point& deadline) const {

   signed_block_ptr block;
   try {
         block = db.fetch_block_by_number( params.block_num );
   } catch (...)   {
      // assert below will handle the invalid block num
   }

   EOS_ASSERT( block, unknown_block_exception, "Could not find block: ${block}", ("block", params.block_num));

   const auto id = block->calculate_id();
   const uint32_t ref_block_prefix = id._hash[1];

   return fc::mutable_variant_object ()
         ("block_num", block->block_num())
         ("ref_block_num", static_cast<uint16_t>(block->block_num()))
         ("id", id)
         ("timestamp", block->timestamp)
         ("producer", block->producer)
         ("confirmed", block->confirmed)
         ("previous", block->previous)
         ("transaction_mroot", block->transaction_mroot)
         ("action_mroot", block->action_mroot)
         ("schedule_version", block->schedule_version)
         ("producer_signature", block->producer_signature)
         ("ref_block_prefix", ref_block_prefix);
}

fc::variant read_only::get_block_header_state(const get_block_header_state_params& params, const fc::time_point& deadline) const {
   block_state_ptr b;
   std::optional<uint64_t> block_num;
   std::exception_ptr e;
   try {
      block_num = fc::to_uint64(params.block_num_or_id);
   } catch( ... ) {}

   if( block_num ) {
      b = db.fetch_block_state_by_number(*block_num);
   } else {
      try {
         b = db.fetch_block_state_by_id(fc::variant(params.block_num_or_id).as<block_id_type>());
      } EOS_RETHROW_EXCEPTIONS(chain::block_id_type_exception, "Invalid block ID: ${block_num_or_id}", ("block_num_or_id", params.block_num_or_id))
   }

   EOS_ASSERT( b, unknown_block_exception, "Could not find reversible block: ${block}", ("block", params.block_num_or_id));

   fc::variant vo;
   fc::to_variant( static_cast<const block_header_state&>(*b), vo );
   return vo;
}

void read_write::push_block(read_write::push_block_params&& params, next_function<read_write::push_block_results> next) {
   try {
      app().get_method<incoming::methods::block_sync>()(std::make_shared<signed_block>( std::move(params) ), std::optional<block_id_type>{}, block_state_ptr{});
   } catch ( boost::interprocess::bad_alloc& ) {
      chain_plugin::handle_db_exhaustion();
   } catch ( const std::bad_alloc& ) {
      chain_plugin::handle_bad_alloc();
   } FC_LOG_AND_DROP()
   next(read_write::push_block_results{});
}

void read_write::push_transaction(const read_write::push_transaction_params& params, next_function<read_write::push_transaction_results> next) {
   try {
      auto pretty_input = std::make_shared<packed_transaction>();
      auto resolver = make_resolver(db, abi_serializer::create_yield_function( abi_serializer_max_time ));
      try {
         abi_serializer::from_variant(params, *pretty_input, std::move( resolver ), abi_serializer::create_yield_function( abi_serializer_max_time ));
      } EOS_RETHROW_EXCEPTIONS(chain::packed_transaction_type_exception, "Invalid packed transaction")

      app().get_method<incoming::methods::transaction_async>()(pretty_input, true, transaction_metadata::trx_type::input, false,
            [this, next](const std::variant<fc::exception_ptr, transaction_trace_ptr>& result) -> void {
         if (std::holds_alternative<fc::exception_ptr>(result)) {
            next(std::get<fc::exception_ptr>(result));
         } else {
            auto trx_trace_ptr = std::get<transaction_trace_ptr>(result);

            try {
               fc::variant output;
               try {
                  output = db.to_variant_with_abi( *trx_trace_ptr, abi_serializer::create_yield_function( abi_serializer_max_time ) );

                  // Create map of (closest_unnotified_ancestor_action_ordinal, global_sequence) with action trace
                  std::map< std::pair<uint32_t, uint64_t>, fc::mutable_variant_object > act_traces_map;
                  for( const auto& act_trace : output["action_traces"].get_array() ) {
                     if (act_trace["receipt"].is_null() && act_trace["except"].is_null()) continue;
                     auto closest_unnotified_ancestor_action_ordinal =
                           act_trace["closest_unnotified_ancestor_action_ordinal"].as<fc::unsigned_int>().value;
                     auto global_sequence = act_trace["receipt"].is_null() ?
                                                std::numeric_limits<uint64_t>::max() :
                                                act_trace["receipt"]["global_sequence"].as<uint64_t>();
                     act_traces_map.emplace( std::make_pair( closest_unnotified_ancestor_action_ordinal,
                                                             global_sequence ),
                                             act_trace.get_object() );
                  }

                  std::function<vector<fc::variant>(uint32_t)> convert_act_trace_to_tree_struct =
                  [&](uint32_t closest_unnotified_ancestor_action_ordinal) {
                     vector<fc::variant> restructured_act_traces;
                     auto it = act_traces_map.lower_bound(
                                 std::make_pair( closest_unnotified_ancestor_action_ordinal, 0)
                     );
                     for( ;
                        it != act_traces_map.end() && it->first.first == closest_unnotified_ancestor_action_ordinal; ++it )
                     {
                        auto& act_trace_mvo = it->second;

                        auto action_ordinal = act_trace_mvo["action_ordinal"].as<fc::unsigned_int>().value;
                        act_trace_mvo["inline_traces"] = convert_act_trace_to_tree_struct(action_ordinal);
                        if (act_trace_mvo["receipt"].is_null()) {
                           act_trace_mvo["receipt"] = fc::mutable_variant_object()
                              ("abi_sequence", 0)
                              ("act_digest", digest_type::hash(trx_trace_ptr->action_traces[action_ordinal-1].act))
                              ("auth_sequence", flat_map<account_name,uint64_t>())
                              ("code_sequence", 0)
                              ("global_sequence", 0)
                              ("receiver", act_trace_mvo["receiver"])
                              ("recv_sequence", 0);
                        }
                        restructured_act_traces.push_back( std::move(act_trace_mvo) );
                     }
                     return restructured_act_traces;
                  };

                  fc::mutable_variant_object output_mvo(output);
                  output_mvo["action_traces"] = convert_act_trace_to_tree_struct(0);

                  output = output_mvo;
               } catch( chain::abi_exception& ) {
                  output = *trx_trace_ptr;
               }

               const chain::transaction_id_type& id = trx_trace_ptr->id;
               next(read_write::push_transaction_results{id, output});
            } CATCH_AND_CALL(next);
         }
      });
   } catch ( boost::interprocess::bad_alloc& ) {
      chain_plugin::handle_db_exhaustion();
   } catch ( const std::bad_alloc& ) {
      chain_plugin::handle_bad_alloc();
   } CATCH_AND_CALL(next);
}

static void push_recurse(read_write* rw, int index, const std::shared_ptr<read_write::push_transactions_params>& params, const std::shared_ptr<read_write::push_transactions_results>& results, const next_function<read_write::push_transactions_results>& next) {
   auto wrapped_next = [=](const std::variant<fc::exception_ptr, read_write::push_transaction_results>& result) {
      if (std::holds_alternative<fc::exception_ptr>(result)) {
         const auto& e = std::get<fc::exception_ptr>(result);
         results->emplace_back( read_write::push_transaction_results{ transaction_id_type(), fc::mutable_variant_object( "error", e->to_detail_string() ) } );
      } else {
         const auto& r = std::get<read_write::push_transaction_results>(result);
         results->emplace_back( r );
      }

      size_t next_index = index + 1;
      if (next_index < params->size()) {
         push_recurse(rw, next_index, params, results, next );
      } else {
         next(*results);
      }
   };

   rw->push_transaction(params->at(index), wrapped_next);
}

void read_write::push_transactions(const read_write::push_transactions_params& params, next_function<read_write::push_transactions_results> next) {
   try {
      EOS_ASSERT( params.size() <= 1000, too_many_tx_at_once, "Attempt to push too many transactions at once" );
      auto params_copy = std::make_shared<read_write::push_transactions_params>(params.begin(), params.end());
      auto result = std::make_shared<read_write::push_transactions_results>();
      result->reserve(params.size());

      push_recurse(this, 0, params_copy, result, next);
   } catch ( boost::interprocess::bad_alloc& ) {
      chain_plugin::handle_db_exhaustion();
   } catch ( const std::bad_alloc& ) {
      chain_plugin::handle_bad_alloc();
   } CATCH_AND_CALL(next);
}

void read_write::send_transaction(const read_write::send_transaction_params& params, next_function<read_write::send_transaction_results> next) {

   try {
      auto pretty_input = std::make_shared<packed_transaction>();
      auto resolver = make_resolver(db, abi_serializer::create_yield_function( abi_serializer_max_time ));
      try {
         abi_serializer::from_variant(params, *pretty_input, resolver, abi_serializer::create_yield_function( abi_serializer_max_time ));
      } EOS_RETHROW_EXCEPTIONS(chain::packed_transaction_type_exception, "Invalid packed transaction")

      app().get_method<incoming::methods::transaction_async>()(pretty_input, true, transaction_metadata::trx_type::input, false,
            [this, next](const std::variant<fc::exception_ptr, transaction_trace_ptr>& result) -> void {
         if (std::holds_alternative<fc::exception_ptr>(result)) {
            next(std::get<fc::exception_ptr>(result));
         } else {
            auto trx_trace_ptr = std::get<transaction_trace_ptr>(result);

            try {
               fc::variant output;
               try {
                  output = db.to_variant_with_abi( *trx_trace_ptr, abi_serializer::create_yield_function( abi_serializer_max_time ) );
               } catch( chain::abi_exception& ) {
                  output = *trx_trace_ptr;
               }

               const chain::transaction_id_type& id = trx_trace_ptr->id;
               next(read_write::send_transaction_results{id, output});
            } CATCH_AND_CALL(next);
         }
      });
   } catch ( boost::interprocess::bad_alloc& ) {
      chain_plugin::handle_db_exhaustion();
   } catch ( const std::bad_alloc& ) {
      chain_plugin::handle_bad_alloc();
   } CATCH_AND_CALL(next);
}

void read_write::send_transaction2(const read_write::send_transaction2_params& params, next_function<read_write::send_transaction_results> next) {
   try {
      auto ptrx = std::make_shared<packed_transaction>();
      auto resolver = make_resolver(db, abi_serializer::create_yield_function( abi_serializer_max_time ));
      try {
         abi_serializer::from_variant(params.transaction, *ptrx, resolver, abi_serializer::create_yield_function( abi_serializer_max_time ));
      } EOS_RETHROW_EXCEPTIONS(chain::packed_transaction_type_exception, "Invalid packed transaction")

      bool retry = params.retry_trx;
      std::optional<uint16_t> retry_num_blocks = params.retry_trx_num_blocks;

      EOS_ASSERT( !retry || trx_retry.has_value(), unsupported_feature, "Transaction retry not enabled on node" );
      EOS_ASSERT( !retry || (ptrx->expiration() <= trx_retry->get_max_expiration_time()), tx_exp_too_far_exception,
                  "retry transaction expiration ${e} larger than allowed ${m}",
                  ("e", ptrx->expiration())("m", trx_retry->get_max_expiration_time()) );

      app().get_method<incoming::methods::transaction_async>()(ptrx, true, transaction_metadata::trx_type::input, static_cast<bool>(params.return_failure_trace),
         [this, ptrx, next, retry, retry_num_blocks](const std::variant<fc::exception_ptr, transaction_trace_ptr>& result) -> void {
            if( std::holds_alternative<fc::exception_ptr>( result ) ) {
               next( std::get<fc::exception_ptr>( result ) );
            } else {
               try {
                  auto trx_trace_ptr = std::get<transaction_trace_ptr>( result );
                  if( retry && trx_retry.has_value() && !trx_trace_ptr->except) {
                     // will be ack'ed via next later
                     trx_retry->track_transaction( ptrx, retry_num_blocks,
                        [ptrx, next](const std::variant<fc::exception_ptr, std::unique_ptr<fc::variant>>& result ) {
                           if( std::holds_alternative<fc::exception_ptr>( result ) ) {
                              next( std::get<fc::exception_ptr>( result ) );
                           } else {
                              fc::variant& output = *std::get<std::unique_ptr<fc::variant>>( result );
                              next( read_write::send_transaction_results{ptrx->id(), std::move( output )} );
                           }
                        } );
                  } else {
                     fc::variant output;
                     try {
                        output = db.to_variant_with_abi( *trx_trace_ptr, abi_serializer::create_yield_function( abi_serializer_max_time ) );
                     } catch( chain::abi_exception& ) {
                        output = *trx_trace_ptr;
                     }
                     const chain::transaction_id_type& id = trx_trace_ptr->id;
                     next( read_write::send_transaction_results{id, std::move( output )} );
                  }
               } CATCH_AND_CALL( next );
            }
         });
   } catch ( boost::interprocess::bad_alloc& ) {
      chain_plugin::handle_db_exhaustion();
   } catch ( const std::bad_alloc& ) {
      chain_plugin::handle_bad_alloc();
   } CATCH_AND_CALL(next);
}

read_only::get_abi_results read_only::get_abi( const get_abi_params& params, const fc::time_point& deadline )const {
   get_abi_results result;
   result.account_name = params.account_name;
   const auto& d = db.db();
   const auto& accnt  = d.get<account_object,by_name>( params.account_name );

   if( abi_def abi; abi_serializer::to_abi(accnt.abi, abi) ) {
      result.abi = std::move(abi);
   }

   return result;
}

read_only::get_code_results read_only::get_code( const get_code_params& params, const fc::time_point& deadline )const {
   get_code_results result;
   result.account_name = params.account_name;
   const auto& d = db.db();
   const auto& accnt_obj          = d.get<account_object,by_name>( params.account_name );
   const auto& accnt_metadata_obj = d.get<account_metadata_object,by_name>( params.account_name );

   EOS_ASSERT( params.code_as_wasm, unsupported_feature, "Returning WAST from get_code is no longer supported" );

   if( accnt_metadata_obj.code_hash != digest_type() ) {
      const auto& code_obj = d.get<code_object, by_code_hash>(accnt_metadata_obj.code_hash);
      result.wasm = string(code_obj.code.begin(), code_obj.code.end());
      result.code_hash = code_obj.code_hash;
   }

   if( abi_def abi; abi_serializer::to_abi(accnt_obj.abi, abi) ) {
      result.abi = std::move(abi);
   }

   return result;
}

read_only::get_code_hash_results read_only::get_code_hash( const get_code_hash_params& params, const fc::time_point& deadline )const {
   get_code_hash_results result;
   result.account_name = params.account_name;
   const auto& d = db.db();
   const auto& accnt  = d.get<account_metadata_object,by_name>( params.account_name );

   if( accnt.code_hash != digest_type() )
      result.code_hash = accnt.code_hash;

   return result;
}

read_only::get_raw_code_and_abi_results read_only::get_raw_code_and_abi( const get_raw_code_and_abi_params& params, const fc::time_point& deadline)const {
   get_raw_code_and_abi_results result;
   result.account_name = params.account_name;

   const auto& d = db.db();
   const auto& accnt_obj          = d.get<account_object,by_name>(params.account_name);
   const auto& accnt_metadata_obj = d.get<account_metadata_object,by_name>(params.account_name);
   if( accnt_metadata_obj.code_hash != digest_type() ) {
      const auto& code_obj = d.get<code_object, by_code_hash>(accnt_metadata_obj.code_hash);
      result.wasm = blob{{code_obj.code.begin(), code_obj.code.end()}};
   }
   result.abi = blob{{accnt_obj.abi.begin(), accnt_obj.abi.end()}};

   return result;
}

read_only::get_raw_abi_results read_only::get_raw_abi( const get_raw_abi_params& params, const fc::time_point& deadline )const {
   get_raw_abi_results result;
   result.account_name = params.account_name;

   const auto& d = db.db();
   const auto& accnt_obj          = d.get<account_object,by_name>(params.account_name);
   const auto& accnt_metadata_obj = d.get<account_metadata_object,by_name>(params.account_name);
   result.abi_hash = fc::sha256::hash( accnt_obj.abi.data(), accnt_obj.abi.size() );
   if( accnt_metadata_obj.code_hash != digest_type() )
      result.code_hash = accnt_metadata_obj.code_hash;
   if( !params.abi_hash || *params.abi_hash != result.abi_hash )
      result.abi = blob{{accnt_obj.abi.begin(), accnt_obj.abi.end()}};

   return result;
}

read_only::get_account_results read_only::get_account( const get_account_params& params, const fc::time_point& deadline )const {
   get_account_results result;
   result.account_name = params.account_name;

   const auto& d = db.db();
   const auto& rm = db.get_resource_limits_manager();

   result.head_block_num  = db.head_block_num();
   result.head_block_time = db.head_block_time();

   rm.get_account_limits( result.account_name, result.ram_quota, result.net_weight, result.cpu_weight );

   const auto& accnt_obj = db.get_account( result.account_name );
   const auto& accnt_metadata_obj = db.db().get<account_metadata_object,by_name>( result.account_name );

   result.privileged       = accnt_metadata_obj.is_privileged();
   result.last_code_update = accnt_metadata_obj.last_code_update;
   result.created          = accnt_obj.creation_date;

   uint32_t greylist_limit = db.is_resource_greylisted(result.account_name) ? 1 : config::maximum_elastic_resource_multiplier;
   const block_timestamp_type current_usage_time (db.head_block_time());
   result.net_limit.set( rm.get_account_net_limit_ex( result.account_name, greylist_limit, current_usage_time).first );
   if ( result.net_limit.last_usage_update_time && (result.net_limit.last_usage_update_time->slot == 0) ) {   // account has no action yet
      result.net_limit.last_usage_update_time = accnt_obj.creation_date;
   }
   result.cpu_limit.set( rm.get_account_cpu_limit_ex( result.account_name, greylist_limit, current_usage_time).first );
   if ( result.cpu_limit.last_usage_update_time && (result.cpu_limit.last_usage_update_time->slot == 0) ) {   // account has no action yet
      result.cpu_limit.last_usage_update_time = accnt_obj.creation_date;
   }
   result.ram_usage = rm.get_account_ram_usage( result.account_name );

   if ( producer_plug ) {  // producer_plug is null when called from chain_plugin_tests.cpp and get_table_tests.cpp
      eosio::chain::resource_limits::account_resource_limit subjective_cpu_bill_limit;
      subjective_cpu_bill_limit.used = producer_plug->get_subjective_bill( result.account_name, fc::time_point::now() );
      result.subjective_cpu_bill_limit = subjective_cpu_bill_limit;
   }

   const auto linked_action_map = ([&](){
      const auto& links = d.get_index<permission_link_index,by_permission_name>();
      auto iter = links.lower_bound( boost::make_tuple( params.account_name ) );

      std::multimap<name, linked_action> result;
      while (iter != links.end() && iter->account == params.account_name ) {
         auto action = iter->message_type.empty() ? std::optional<name>() : std::optional<name>(iter->message_type);
         result.emplace(std::make_pair(iter->required_permission, linked_action{iter->code, std::move(action)}));
         ++iter;
      }

      return result;
   })();

   auto get_linked_actions = [&](chain::name perm_name) {
      auto link_bounds = linked_action_map.equal_range(perm_name);
      auto linked_actions = std::vector<linked_action>();
      linked_actions.reserve(linked_action_map.count(perm_name));
      for (auto link = link_bounds.first; link != link_bounds.second; ++link) {
         linked_actions.push_back(link->second);
      }
      return linked_actions;
   };

   const auto& permissions = d.get_index<permission_index,by_owner>();
   auto perm = permissions.lower_bound( boost::make_tuple( params.account_name ) );
   while( perm != permissions.end() && perm->owner == params.account_name ) {
      /// TODO: lookup perm->parent name
      name parent;

      // Don't lookup parent if null
      if( perm->parent._id ) {
         const auto* p = d.find<permission_object,by_id>( perm->parent );
         if( p ) {
            EOS_ASSERT(perm->owner == p->owner, invalid_parent_permission, "Invalid parent permission");
            parent = p->name;
         }
      }

      auto linked_actions = get_linked_actions(perm->name);

      result.permissions.push_back( permission{ perm->name, parent, perm->auth.to_authority(), std::move(linked_actions)} );
      ++perm;
   }

   // add eosio.any linked authorizations
   result.eosio_any_linked_actions = get_linked_actions(chain::config::eosio_any_name);

   const auto& code_account = db.db().get<account_object,by_name>( config::system_account_name );

   if( abi_def abi; abi_serializer::to_abi(code_account.abi, abi) ) {
      abi_serializer abis( std::move(abi), abi_serializer::create_yield_function( abi_serializer_max_time ) );

      const auto token_code = "eosio.token"_n;

      auto core_symbol = extract_core_symbol();

      if (params.expected_core_symbol)
         core_symbol = *(params.expected_core_symbol);

      const auto* t_id = d.find<chain::table_id_object, chain::by_code_scope_table>(boost::make_tuple( token_code, params.account_name, "accounts"_n ));
      if( t_id != nullptr ) {
         const auto &idx = d.get_index<key_value_index, by_scope_primary>();
         auto it = idx.find(boost::make_tuple( t_id->id, core_symbol.to_symbol_code() ));
         if( it != idx.end() && it->value.size() >= sizeof(asset) ) {
            asset bal;
            fc::datastream<const char *> ds(it->value.data(), it->value.size());
            fc::raw::unpack(ds, bal);

            if( bal.get_symbol().valid() && bal.get_symbol() == core_symbol ) {
               result.core_liquid_balance = bal;
            }
         }
      }

      t_id = d.find<chain::table_id_object, chain::by_code_scope_table>(boost::make_tuple( config::system_account_name, params.account_name, "userres"_n ));
      if (t_id != nullptr) {
         const auto &idx = d.get_index<key_value_index, by_scope_primary>();
         auto it = idx.find(boost::make_tuple( t_id->id, params.account_name.to_uint64_t() ));
         if ( it != idx.end() ) {
            vector<char> data;
            copy_inline_row(*it, data);
            result.total_resources = abis.binary_to_variant( "user_resources", data, abi_serializer::create_yield_function( abi_serializer_max_time ), shorten_abi_errors );
         }
      }

      t_id = d.find<chain::table_id_object, chain::by_code_scope_table>(boost::make_tuple( config::system_account_name, params.account_name, "delband"_n ));
      if (t_id != nullptr) {
         const auto &idx = d.get_index<key_value_index, by_scope_primary>();
         auto it = idx.find(boost::make_tuple( t_id->id, params.account_name.to_uint64_t() ));
         if ( it != idx.end() ) {
            vector<char> data;
            copy_inline_row(*it, data);
            result.self_delegated_bandwidth = abis.binary_to_variant( "delegated_bandwidth", data, abi_serializer::create_yield_function( abi_serializer_max_time ), shorten_abi_errors );
         }
      }

      t_id = d.find<chain::table_id_object, chain::by_code_scope_table>(boost::make_tuple( config::system_account_name, params.account_name, "refunds"_n ));
      if (t_id != nullptr) {
         const auto &idx = d.get_index<key_value_index, by_scope_primary>();
         auto it = idx.find(boost::make_tuple( t_id->id, params.account_name.to_uint64_t() ));
         if ( it != idx.end() ) {
            vector<char> data;
            copy_inline_row(*it, data);
            result.refund_request = abis.binary_to_variant( "refund_request", data, abi_serializer::create_yield_function( abi_serializer_max_time ), shorten_abi_errors );
         }
      }

      t_id = d.find<chain::table_id_object, chain::by_code_scope_table>(boost::make_tuple( config::system_account_name, config::system_account_name, "voters"_n ));
      if (t_id != nullptr) {
         const auto &idx = d.get_index<key_value_index, by_scope_primary>();
         auto it = idx.find(boost::make_tuple( t_id->id, params.account_name.to_uint64_t() ));
         if ( it != idx.end() ) {
            vector<char> data;
            copy_inline_row(*it, data);
            result.voter_info = abis.binary_to_variant( "voter_info", data, abi_serializer::create_yield_function( abi_serializer_max_time ), shorten_abi_errors );
         }
      }

      t_id = d.find<chain::table_id_object, chain::by_code_scope_table>(boost::make_tuple( config::system_account_name, config::system_account_name, "rexbal"_n ));
      if (t_id != nullptr) {
         const auto &idx = d.get_index<key_value_index, by_scope_primary>();
         auto it = idx.find(boost::make_tuple( t_id->id, params.account_name.to_uint64_t() ));
         if( it != idx.end() ) {
            vector<char> data;
            copy_inline_row(*it, data);
            result.rex_info = abis.binary_to_variant( "rex_balance", data, abi_serializer::create_yield_function( abi_serializer_max_time ), shorten_abi_errors );
         }
      }
   }
   return result;
}

read_only::get_required_keys_result read_only::get_required_keys( const get_required_keys_params& params, const fc::time_point& deadline )const {
   transaction pretty_input;
   auto resolver = make_resolver(db, abi_serializer::create_yield_function( abi_serializer_max_time ));
   try {
      abi_serializer::from_variant(params.transaction, pretty_input, resolver, abi_serializer::create_yield_function( abi_serializer_max_time ));
   } EOS_RETHROW_EXCEPTIONS(chain::transaction_type_exception, "Invalid transaction")

   auto required_keys_set = db.get_authorization_manager().get_required_keys( pretty_input, params.available_keys, fc::seconds( pretty_input.delay_sec ));
   get_required_keys_result result;
   result.required_keys = required_keys_set;
   return result;
}

template<typename Params, typename Results>
void read_only::send_transient_transaction(const Params& params, next_function<Results> next, chain::transaction_metadata::trx_type trx_type) const {
   try {
      auto pretty_input = std::make_shared<packed_transaction>();
      auto resolver = make_resolver(db, abi_serializer::create_yield_function( abi_serializer_max_time ));
      try {
         abi_serializer::from_variant(params.transaction, *pretty_input, resolver, abi_serializer::create_yield_function( abi_serializer_max_time ));
      } EOS_RETHROW_EXCEPTIONS(chain::packed_transaction_type_exception, "Invalid packed transaction")

      app().get_method<incoming::methods::transaction_async>()(pretty_input, true /* api_trx */, trx_type, true /* return_failure_trace */,
          [this, next](const std::variant<fc::exception_ptr, transaction_trace_ptr>& result) -> void {
             if (std::holds_alternative<fc::exception_ptr>(result)) {
                   next(std::get<fc::exception_ptr>(result));
              } else {
                 auto trx_trace_ptr = std::get<transaction_trace_ptr>(result);

                 try {
                    fc::variant output;
                    try {
                        output = db.to_variant_with_abi( *trx_trace_ptr, abi_serializer::create_yield_function( abi_serializer_max_time ) );
                    } catch( chain::abi_exception& ) {
                        output = *trx_trace_ptr;
                    }

                    const chain::transaction_id_type& id = trx_trace_ptr->id;
                    next(Results{id, output});
                 } CATCH_AND_CALL(next);
              }
      });
   } catch ( boost::interprocess::bad_alloc& ) {
      chain_plugin::handle_db_exhaustion();
   } catch ( const std::bad_alloc& ) {
      chain_plugin::handle_bad_alloc();
   } CATCH_AND_CALL(next);
}

void read_only::compute_transaction(const compute_transaction_params& params, next_function<compute_transaction_results> next) const {
   return send_transient_transaction(params, next, transaction_metadata::trx_type::dry_run);
}

void read_only::send_read_only_transaction(const send_read_only_transaction_params& params, next_function<send_read_only_transaction_results> next) const {
   return send_transient_transaction(params, next, transaction_metadata::trx_type::read_only);
}

read_only::get_transaction_id_result read_only::get_transaction_id( const read_only::get_transaction_id_params& params, const fc::time_point& deadline)const {
   return params.id();
}


account_query_db::get_accounts_by_authorizers_result
read_only::get_accounts_by_authorizers( const account_query_db::get_accounts_by_authorizers_params& args, const fc::time_point& deadline) const
{
   EOS_ASSERT(aqdb.has_value(), plugin_config_exception, "Account Queries being accessed when not enabled");
   return aqdb->get_accounts_by_authorizers(args);
}

namespace detail {
   struct ram_market_exchange_state_t {
      asset  ignore1;
      asset  ignore2;
      double ignore3;
      asset  core_symbol;
      double ignore4;
   };
}

chain::symbol read_only::extract_core_symbol()const {
   symbol core_symbol(0);

   // The following code makes assumptions about the contract deployed on eosio account (i.e. the system contract) and how it stores its data.
   const auto& d = db.db();
   const auto* t_id = d.find<chain::table_id_object, chain::by_code_scope_table>(boost::make_tuple( "eosio"_n, "eosio"_n, "rammarket"_n ));
   if( t_id != nullptr ) {
      const auto &idx = d.get_index<key_value_index, by_scope_primary>();
      auto it = idx.find(boost::make_tuple( t_id->id, eosio::chain::string_to_symbol_c(4,"RAMCORE") ));
      if( it != idx.end() ) {
         detail::ram_market_exchange_state_t ram_market_exchange_state;

         fc::datastream<const char *> ds( it->value.data(), it->value.size() );

         try {
            fc::raw::unpack(ds, ram_market_exchange_state);
         } catch( ... ) {
            return core_symbol;
         }

         if( ram_market_exchange_state.core_symbol.get_symbol().valid() ) {
            core_symbol = ram_market_exchange_state.core_symbol.get_symbol();
         }
      }
   }

   return core_symbol;
}

read_only::get_consensus_parameters_results
read_only::get_consensus_parameters(const get_consensus_parameters_params&, const fc::time_point& deadline ) const {
   get_consensus_parameters_results results;

   results.chain_config = db.get_global_properties().configuration;
   if (db.is_builtin_activated(builtin_protocol_feature_t::configurable_wasm_limits)) {
      results.wasm_config = db.get_global_properties().wasm_configuration;
   }

   return results;
}

} // namespace chain_apis

fc::variant chain_plugin::get_log_trx_trace(const transaction_trace_ptr& trx_trace ) const {
    fc::variant pretty_output;
    try {
        abi_serializer::to_log_variant(trx_trace, pretty_output,
                                       chain_apis::make_resolver(chain(), abi_serializer::create_yield_function(get_abi_serializer_max_time())),
                                       abi_serializer::create_yield_function(get_abi_serializer_max_time()));
    } catch (...) {
        pretty_output = trx_trace;
    }
    return pretty_output;
}

fc::variant chain_plugin::get_log_trx(const transaction& trx) const {
    fc::variant pretty_output;
    try {
        abi_serializer::to_log_variant(trx, pretty_output,
                                       chain_apis::make_resolver(chain(), abi_serializer::create_yield_function(get_abi_serializer_max_time())),
                                       abi_serializer::create_yield_function(get_abi_serializer_max_time()));
    } catch (...) {
        pretty_output = trx;
    }
    return pretty_output;
}

const controller::config& chain_plugin::chain_config() const {
   EOS_ASSERT(my->chain_config.has_value(), plugin_exception, "chain_config not initialized");
   return *my->chain_config;
}
} // namespace eosio

FC_REFLECT( eosio::chain_apis::detail::ram_market_exchange_state_t, (ignore1)(ignore2)(ignore3)(core_symbol)(ignore4) )
