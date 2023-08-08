#pragma once

#include <eosio/chain/application.hpp>
#include <eosio/net_plugin/protocol.hpp>
#include <eosio/chain_plugin/chain_plugin.hpp>

namespace eosio {
   using namespace appbase;

   struct connection_status {
      string            peer;
      string            remote_ip;
      string            remote_port;
      bool              connecting           = false;
      bool              syncing              = false;
      bool              is_bp_peer           = false;
      bool              is_socket_open       = false;
      bool              is_blocks_only       = false;
      bool              is_transactions_only = false;
      handshake_message last_handshake;
   };

   class net_plugin : public appbase::plugin<net_plugin>
   {
      public:
        net_plugin();
        virtual ~net_plugin();

        APPBASE_PLUGIN_REQUIRES((chain_plugin))
        virtual void set_program_options(options_description& cli, options_description& cfg) override;
        void handle_sighup() override;

        void plugin_initialize(const variables_map& options);
        void plugin_startup();
        void plugin_shutdown();

        string                            connect( const string& endpoint );
        string                            disconnect( const string& endpoint );
        std::optional<connection_status>  status( const string& endpoint )const;
        vector<connection_status>         connections()const;

        struct p2p_per_connection_metrics {
            explicit p2p_per_connection_metrics(size_t count) {
               addresses.reserve(count);
               ports.reserve(count);
               accepting_blocks.reserve(count);
               last_received_blocks.reserve(count);
               first_available_blocks.reserve(count);
               last_available_blocks.reserve(count);
               unique_first_block_counts.reserve(count);
               latencies.reserve(count);
               bytes_received.reserve(count);
               last_bytes_received.reserve(count);
               bytes_sent.reserve(count);
               last_bytes_sent.reserve(count);
               connection_start_times.reserve(count);
               log_p2p_addresses.reserve(count);
            }
            p2p_per_connection_metrics(p2p_per_connection_metrics&& metrics) 
               : addresses{std::move(metrics.addresses)}
               , ports{std::move(metrics.ports)}
               , accepting_blocks{std::move(metrics.accepting_blocks)}
               , last_received_blocks{std::move(metrics.last_received_blocks)}
               , first_available_blocks{std::move(metrics.first_available_blocks)}
               , last_available_blocks{std::move(metrics.last_available_blocks)}
               , unique_first_block_counts{std::move(metrics.unique_first_block_counts)}
               , latencies{std::move(metrics.latencies)}
               , bytes_received{std::move(metrics.bytes_received)}
               , last_bytes_received(std::move(metrics.last_bytes_received))
               , bytes_sent{std::move(metrics.bytes_sent)}
               , last_bytes_sent(std::move(metrics.last_bytes_sent))
               , connection_start_times{std::move(metrics.connection_start_times)}
               , log_p2p_addresses{std::move(metrics.log_p2p_addresses)}
            {}
            p2p_per_connection_metrics(const p2p_per_connection_metrics&) = delete;
            p2p_per_connection_metrics& operator=(const p2p_per_connection_metrics&) = delete;
            std::vector<boost::asio::ip::address_v6::bytes_type> addresses;
            std::vector<unsigned short> ports;
            std::vector<bool> accepting_blocks;
            std::vector<uint32_t> last_received_blocks;
            std::vector<uint32_t> first_available_blocks;
            std::vector<uint32_t> last_available_blocks;
            std::vector<std::size_t> unique_first_block_counts;
            std::vector<uint64_t> latencies;
            std::vector<std::size_t> bytes_received;
            std::vector<std::time_t> last_bytes_received;
            std::vector<std::size_t> bytes_sent;
            std::vector<std::time_t> last_bytes_sent;
            std::vector<std::chrono::nanoseconds> connection_start_times;
            std::vector<std::string> log_p2p_addresses;
        };
        struct p2p_connections_metrics {
           p2p_connections_metrics(std::size_t peers, std::size_t clients, p2p_per_connection_metrics&& statistics)
              : num_peers{peers}
              , num_clients{clients}
              , stats{std::move(statistics)}
           {}
           p2p_connections_metrics(p2p_connections_metrics&& statistics)
              : num_peers{std::move(statistics.num_peers)}
              , num_clients{std::move(statistics.num_clients)}
              , stats{std::move(statistics.stats)}
           {}
           p2p_connections_metrics(const p2p_connections_metrics&) = delete;
           std::size_t num_peers   = 0;
           std::size_t num_clients = 0;
           p2p_per_connection_metrics stats;
        };

        void register_update_p2p_connection_metrics(std::function<void(p2p_connections_metrics)>&&);
        void register_increment_failed_p2p_connections(std::function<void()>&&);
        void register_increment_dropped_trxs(std::function<void()>&&);

      private:
        std::shared_ptr<class net_plugin_impl> my;
   };

}

FC_REFLECT( eosio::connection_status, (peer)(remote_ip)(remote_port)(connecting)(syncing)(is_bp_peer)(is_socket_open)(is_blocks_only)(is_transactions_only)(last_handshake) )
