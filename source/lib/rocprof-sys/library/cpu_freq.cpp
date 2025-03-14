// MIT License
//
// Copyright (c) 2022-2025 Advanced Micro Devices, Inc. All Rights Reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.



#include "library/cpu_freq.hpp"
#include "core/common.hpp"
#include "core/components/fwd.hpp"
#include "core/config.hpp"
#include "core/debug.hpp"
#include "core/defines.hpp"
#include "core/perfetto.hpp"
#include "core/timemory.hpp"
#include "library/components/cpu_freq.hpp"
#include "library/thread_data.hpp"
#include "library/thread_info.hpp"

#include <timemory/components/rusage/backends.hpp>
#include <timemory/mpl/types.hpp>
#include <timemory/units.hpp>
#include <timemory/utility/procfs/cpuinfo.hpp>
#include <timemory/utility/type_list.hpp>

#include <cstddef>
#include <cstdio> 
#include <cstdlib>
#include <string>
#include <sys/resource.h>
#include <tuple>
#include <utility>
#include <vector>

#include <sqlite3.h>

namespace rocprofsys
{
namespace cpu_freq
{
template <typename... Tp>
using type_list = tim::type_list<Tp...>;

namespace
{
using cpu_data_tuple_t = std::tuple<size_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                                    int64_t, int64_t, component::cpu_freq>;
std::deque<cpu_data_tuple_t> data = {};

template <typename... Types>
void init_perfetto_counter_tracks(type_list<Types...>)
{
    (perfetto_counter_track<Types>::init(), ...);
}
}  // namespace
}  // namespace cpu_freq
}  // namespace rocprofsys


namespace rocprofsys
{
namespace cpu_freq
{

// FIXME all NOT NULL are removed to avoid not-fill errors
auto table_schema = R"(
CREATE TABLE IF NOT EXISTS
    "rocpd_metadata" (
        "id" INTEGER  PRIMARY KEY AUTOINCREMENT,
        "tag" TEXT,
        "value" TEXT 
    );

CREATE TABLE IF NOT EXISTS
    "rocpd_string" (
        "id" INTEGER PRIMARY KEY AUTOINCREMENT,
        "string" TEXT  UNIQUE ON CONFLICT IGNORE
    );

CREATE TABLE IF NOT EXISTS
    "_rocpd_node" (
        "id" INTEGER,
        "hash" INTEGER,
        "machine_id" TEXT ,
        "system_name" TEXT,
        "hostname" TEXT,
        "release" TEXT,
        "version" TEXT,
        "hardware_name" TEXT,
        "domain_name" TEXT,
        PRIMARY KEY (id)
    );

CREATE TABLE IF NOT EXISTS
    "_rocpd_process" (
        "id" INTEGER,
        "node_id" INTEGER,
        "parent_pid" INTEGER,
        "init" BIGINT,
        "fini" BIGINT,
        "start" BIGINT,
        "end" BIGINT,
        "command" TEXT,
        "environment" JSONB DEFAULT "{}",
        "extdata" JSONB DEFAULT "{}",
        FOREIGN KEY (node_id) REFERENCES _rocpd_node (id),
        PRIMARY KEY (id, node_id)
    );

CREATE TABLE IF NOT EXISTS
    "_rocpd_thread" (
        "id" INTEGER,
        "node_id" INTEGER,
        "process_id" INTEGER,
        "name" TEXT,
        "start" BIGINT,
        "end" BIGINT,
        "extdata" JSONB DEFAULT "{}",
        FOREIGN KEY (node_id) REFERENCES _rocpd_node (id),
        FOREIGN KEY (process_id) REFERENCES rocpd_process (id),
        PRIMARY KEY (id, process_id, node_id)
    );

CREATE TABLE IF NOT EXISTS
    "rocpd_agent" (
        "id" INTEGER,
        "node_id" INTEGER,
        "type" TEXT CHECK ("type" IN ('CPU', 'GPU')),
        "absolute_index" INTEGER,
        "logical_index" INTEGER,
        "type_index" INTEGER,
        "uuid" INTEGER,
        "name" TEXT,
        "model_name" TEXT,
        "vendor_name" TEXT,
        "product_name" TEXT,
        "user_name" TEXT,
        "extdata" JSONB DEFAULT "{}",
        FOREIGN KEY (node_id) REFERENCES _rocpd_node (id),
        PRIMARY KEY (id)
    );

CREATE TABLE IF NOT EXISTS
    "rocpd_queue" (
        "id" INTEGER,
        "node_id" INTEGER,
        "pid" INTEGER,
        "name" TEXT,
        "extdata" JSONB DEFAULT "{}",
        FOREIGN KEY (node_id) REFERENCES _rocpd_node (id),
        PRIMARY KEY (id)
    );

CREATE TABLE IF NOT EXISTS
    "rocpd_stream" (
        "id" INTEGER,
        "node_id" INTEGER,
        "pid" INTEGER,
        "name" TEXT,
        "extdata" JSONB DEFAULT "{}",
        FOREIGN KEY (node_id) REFERENCES _rocpd_node (id),
        PRIMARY KEY (id)
    );

-- Performance monitoring counters (PMC) descriptions
CREATE TABLE IF NOT EXISTS
    "rocpd_pmc" (
        "id" INTEGER,
        "target_arch" TEXT CHECK ("target_arch" IN ('CPU', 'GPU')),
        "agent_id" INTEGER,
        "event_code" INT,
        "instance_id" INTEGER,
        "name" TEXT,
        "symbol" TEXT,
        "description" TEXT,
        "long_description" TEXT DEFAULT "",
        "component" TEXT,
        "units" TEXT DEFAULT "",
        "value_type" TEXT CHECK ("value_type" IN ('ABS', 'ACCUM', 'RELATIVE')),
        "block" TEXT,
        "expression" TEXT,
        "is_constant" INTEGER,
        "is_derived" INTEGER,
        "extdata" JSONB DEFAULT "{}",
        PRIMARY KEY (id, agent_id)
    );

CREATE TABLE IF NOT EXISTS
    "rocpd_code_object" (
        "id" INTEGER,
        "node_id" INTEGER,
        "agent_id" INTEGER,
        "uri" TEXT,
        "load_base" BIGINT,
        "load_size" BIGINT,
        "load_delta" BIGINT,
        "storage_type" TEXT CHECK ("storage_type" IN ('FILE', 'MEMORY')),
        "extdata" JSONB DEFAULT "{}",
        FOREIGN KEY (node_id) REFERENCES _rocpd_node (id),
        FOREIGN KEY (agent_id) REFERENCES rocpd_agent (id),
        PRIMARY KEY (id)
    );

CREATE TABLE IF NOT EXISTS
    "rocpd_kernel_symbol" (
        "id" INTEGER,
        "node_id" INTEGER,
        "code_object_id" INTEGER,
        "kernel_name" TEXT,
        "display_name" TEXT,
        "kernel_object" INTEGER,
        "kernarg_segment_size" INTEGER,
        "kernarg_segment_alignment" INTEGER,
        "group_segment_size" INTEGER,
        "private_segment_size" INTEGER,
        "sgpr_count" INTEGER,
        "arch_vgpr_count" INTEGER,
        "accum_vgpr_count" INTEGER,
        "extdata" JSONB DEFAULT "{}",
        FOREIGN KEY (node_id) REFERENCES _rocpd_node (id),
        FOREIGN KEY (code_object_id) REFERENCES rocpd_code_object (id),
        PRIMARY KEY (id)
    );

-- Stores repetitive info for samples
CREATE TABLE IF NOT EXISTS
    "_rocpd_track" (
        "id" INTEGER,
        "node_id" INTEGER,
        "pid" INTEGER,
        "tid" INTEGER,
        "name_id" INTEGER,
        "extdata" JSONB DEFAULT "{}",
        FOREIGN KEY (node_id) REFERENCES _rocpd_node (id),
        FOREIGN KEY (name_id) REFERENCES rocpd_string (id),
        PRIMARY KEY (id)
    );

-- Storage for a region, instant, and counter
CREATE TABLE IF NOT EXISTS
    "rocpd_event" (
        "id" INTEGER,
        "category_id" INTEGER,
        "correlation_id" INTEGER,
        "stack_id" INTEGER,
        "parent_stack_id" INTEGER,
        "args" JSONB DEFAULT "[]", -- TODO this must be removed once rocpd_arg is setlled -- 
        "metrics" JSONB DEFAULT "{}",
        "call_stack" JSONB DEFAULT "{}",
        "line_info" JSONB DEFAULT "{}",
        "extdata" JSONB DEFAULT "{}",
        FOREIGN KEY (category_id) REFERENCES rocpd_string (id),
        PRIMARY KEY (id)
    );

-- Storage for event metrics
CREATE TABLE IF NOT EXISTS
    "rocpd_metric" (
        "id" INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
        "event_id" INTEGER,
        "name_id" INTEGER,
        "value" INTEGER,
        FOREIGN KEY (event_id) REFERENCES rocpd_event (id),
        FOREIGN KEY (name_id) REFERENCES rocpd_string (id)
    );

-- stores arguments for events
CREATE TABLE IF NOT EXISTS
    "rocpd_arg" (
        "id" INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
        "event_id" INTEGER,
        "position" INTEGER,
        "type" TEXT,
        "name" TEXT,
        "value" TEXT, -- TODO: discuss make it value_id and integer, refer to string table -- 
        "extdata" JSONB DEFAULT "{}",
        FOREIGN KEY (event_id) REFERENCES rocpd_event (id)
    );

-- Region with a start/stop on the same thread (CPU)
CREATE TABLE IF NOT EXISTS
    "rocpd_pmc_event" (
        "id" INTEGER,
        "event_id" INTEGER,
        "pmc_id" INTEGER,
        "value" REAL DEFAULT 0.0,
        "extdata" JSONB DEFAULT "{}",
        FOREIGN KEY (pmc_id) REFERENCES rocpd_pmc (id),
        FOREIGN KEY (event_id) REFERENCES rocpd_event (id),
        PRIMARY KEY (id, event_id)
    );

-- Region with a start/stop on the same thread (CPU)
CREATE TABLE IF NOT EXISTS
    "_rocpd_region" (
        "id" INTEGER,
        "node_id" INTEGER,
        "pid" INTEGER,
        "tid" INTEGER,
        "start" BIGINT,
        "end" BIGINT,
        "name_id" INTEGER,
        "event_id" INTEGER,
        "extdata" JSONB DEFAULT "{}",
        FOREIGN KEY (node_id) REFERENCES _rocpd_node (id),
        FOREIGN KEY (name_id) REFERENCES rocpd_string (id),
        FOREIGN KEY (event_id) REFERENCES rocpd_event (id),
        PRIMARY KEY (id)
    );

-- Instantaneous sample
CREATE TABLE IF NOT EXISTS
    "_rocpd_sample" (
        "id" INTEGER,
        "track_id" INTEGER,
        "timestamp" BIGINT,
        "event_id" INTEGER,
        "extdata" JSONB DEFAULT "{}",
        FOREIGN KEY (track_id) REFERENCES _rocpd_track (id),
        FOREIGN KEY (event_id) REFERENCES rocpd_event (id),
        PRIMARY KEY (id)
    );

CREATE TABLE IF NOT EXISTS
    "_rocpd_kernel_dispatch" (
        "id" INTEGER,
        "node_id" INTEGER,
        "agent_id" INTEGER,
        "kernel_id" INTEGER,
        "dispatch_id" INTEGER,
        "queue_id" INTEGER,
        "stream_id" INTEGER,
        "start" BIGINT,
        "end" BIGINT,
        "private_segment_size" INTEGER,
        "group_segment_size" INTEGER,
        "workgroup_size_x" INTEGER,
        "workgroup_size_y" INTEGER,
        "workgroup_size_z" INTEGER,
        "grid_size_x" INTEGER,
        "grid_size_y" INTEGER,
        "grid_size_z" INTEGER,
        "region_name_id" INTEGER,
        "event_id" INTEGER,
        "extdata" JSONB DEFAULT "{}",
        FOREIGN KEY (node_id) REFERENCES _rocpd_node (id),
        FOREIGN KEY (agent_id) REFERENCES rocpd_agent (id),
        FOREIGN KEY (kernel_id) REFERENCES rocpd_kernel_symbol (id),
        FOREIGN KEY (queue_id) REFERENCES rocpd_queue (id),
        FOREIGN KEY (stream_id) REFERENCES rocpd_stream (id),
        FOREIGN KEY (region_name_id) REFERENCES rocpd_string (id),
        FOREIGN KEY (event_id) REFERENCES rocpd_event (id),
        PRIMARY KEY (id)
    );

CREATE TABLE IF NOT EXISTS
    "_rocpd_memory_copy" (
        "id" INTEGER,
        "node_id" INTEGER,
        "pid" INTEGER,
        "tid" INTEGER,
        "start" BIGINT,
        "end" BIGINT,
        "name_id" INTEGER,
        "dst_agent_id" INTEGER,
        "dst_address" INTEGER,
        "src_agent_id" INTEGER,
        "src_address" INTEGER,
        "size" INTEGER,
        "queue_id" INTEGER,
        "stream_id" INTEGER,
        "region_name_id" INTEGER,
        "event_id" INTEGER,
        "extdata" JSONB DEFAULT "{}",
        FOREIGN KEY (node_id) REFERENCES _rocpd_node (id),
        FOREIGN KEY (name_id) REFERENCES rocpd_string (id),
        FOREIGN KEY (dst_agent_id) REFERENCES rocpd_agent (id),
        FOREIGN KEY (src_agent_id) REFERENCES rocpd_agent (id),
        FOREIGN KEY (stream_id) REFERENCES rocpd_stream (id),
        FOREIGN KEY (queue_id) REFERENCES rocpd_queue (id),
        FOREIGN KEY (region_name_id) REFERENCES rocpd_string (id),
        FOREIGN KEY (event_id) REFERENCES rocpd_event (id),
        PRIMARY KEY (id)
    );

-- Memory allocations (real memory, virtual memory, and scratch memory)
CREATE TABLE IF NOT EXISTS
    "_rocpd_memory_allocate" (
        "id" INTEGER PRIMARY KEY AUTOINCREMENT,
        "node_id" INTEGER,
        "pid" INTEGER,
        "tid" INTEGER,
        "agent_id" INTEGER,
        "type" TEXT CHECK ("type" IN ('ALLOC', 'FREE', 'REALLOC', 'RECLAIM')),
        "level" TEXT CHECK ("level" IN ('REAL', 'VIRTUAL', 'SCRATCH')),
        "start" BIGINT,
        "end" BIGINT,
        "address" INTEGER,
        "size" INTEGER,
        "queue_id" INTEGER,
        "stream_id" INTEGER,
        "event_id" INTEGER,
        "extdata" JSONB DEFAULT "{}",
        FOREIGN KEY (node_id) REFERENCES _rocpd_node (id),
        FOREIGN KEY (agent_id) REFERENCES rocpd_agent (id),
        FOREIGN KEY (stream_id) REFERENCES rocpd_stream (id),
        FOREIGN KEY (queue_id) REFERENCES rocpd_queue (id),
        FOREIGN KEY (event_id) REFERENCES rocpd_event (id)
    );

-- View for CPU metrics
   CREATE VIEW IF NOT EXISTS cpu_metrics AS
    SELECT 
        n.id as node_id,
        a.id as agent_id,
        a.type as agent_type,
        a.absolute_index as cpu_index,
        t.id as track_id,
        s.id as sample_id,
        s.timestamp,
        e.id as event_id,
        st.string as metric_name,  -- Changed from e.string to st.string
        m.value as metric_value
    FROM _rocpd_node n
    JOIN rocpd_agent a ON a.node_id = n.id
    JOIN _rocpd_track t ON t.node_id = n.id
    JOIN _rocpd_sample s ON s.track_id = t.id
    JOIN rocpd_event e ON e.id = s.event_id
    JOIN rocpd_metric m ON m.event_id = e.id
    JOIN rocpd_string st ON st.id = m.name_id  -- Changed from e.name_id to m.name_id
    WHERE a.type = 'CPU'
    ORDER BY s.timestamp;

INSERT INTO
    "rocpd_metadata" (tag, value)
VALUES
    ("schema_version", "4");
)";


int
sql_busy_handler(void* /*data*/, int count)
{
    count = (count < 9) ? count : 8;
    usleep(1000 * (0x1 << count));
    return 1;
}

void
execute_raw_sql_statements(sqlite3* conn, std::string_view stmts)
{
    ROCPROFSYS_VERBOSE(1, "sqlite3_exec: %s\n", std::string{stmts}.c_str());

    //  TODO:
    //      - translate error code into error message
    //
    auto ret = sqlite3_exec(conn, std::string{stmts}.c_str(), nullptr, nullptr, nullptr);

    if(ret != SQLITE_OK)
    {
        ROCPROFSYS_VERBOSE(1, "sqlite3 error %x, in statement: %s\n", ret, std::string{stmts}.c_str());
    }
}

struct sql_deferred_transaction
{
    sql_deferred_transaction(sqlite3* conn)
    : m_conn{conn}
    {
        execute_raw_sql_statements(m_conn, "BEGIN DEFERRED TRANSACTION");
    }

    ~sql_deferred_transaction() { execute_raw_sql_statements(m_conn, "END TRANSACTION"); }

    sqlite3* m_conn = nullptr;
};

void
setup()
{
    init_perfetto_counter_tracks(
        type_list<category::cpu_freq, category::process_page, category::process_virt,
                  category::process_peak, category::process_context_switch,
                  category::process_page_fault, category::process_user_mode_time,
                  category::process_kernel_mode_time>{});
}

void
config()
{
    component::cpu_freq::configure();
}

void
sample()
{
    auto _ts = tim::get_clock_real_now<size_t, std::nano>();

    auto _rcache = tim::rusage_cache{ RUSAGE_SELF };
    auto _freqs  = component::cpu_freq{}.sample();

    // user and kernel mode times are in microseconds
    data.emplace_back(
        _ts, tim::get_page_rss(), tim::get_virt_mem(), _rcache.get_peak_rss(),
        _rcache.get_num_priority_context_switch() +
            _rcache.get_num_voluntary_context_switch(),
        _rcache.get_num_major_page_faults() + _rcache.get_num_minor_page_faults(),
        _rcache.get_user_mode_time() * 1000, _rcache.get_kernel_mode_time() * 1000,
        std::move(_freqs));
}

void
shutdown()
{}

namespace
{
template <typename... Types, size_t N = sizeof...(Types)>
void
config_perfetto_counter_tracks(type_list<Types...>, std::array<const char*, N> _labels,
                               std::array<const char*, N> _units)
{
    static_assert(sizeof...(Types) == N,
                  "Error! Number of types != number of labels/units");

    auto _config = [&](auto _t) {
        using type          = std::decay_t<decltype(_t)>;
        using track         = perfetto_counter_track<type>;
        constexpr auto _idx = tim::index_of<type, type_list<Types...>>::value;
        if(!track::exists(0))
        {
            auto addendum = [&](const char* _v) { return JOIN(" ", "CPU", _v, "(S)"); };
            track::emplace(0, addendum(_labels.at(_idx)), _units.at(_idx));
        }
    };

    (_config(Types{}), ...);
}

struct index
{
    size_t value = 0;
};

template <typename Tp, typename... Args>
void
write_perfetto_counter_track(Args... _args)
{
    using track = perfetto_counter_track<Tp>;
    TRACE_COUNTER(trait::name<Tp>::value, track::at(0, 0), _args...);
}

template <typename Tp, typename... Args>
void
write_perfetto_counter_track(index&& _idx, Args... _args)
{
    using track = perfetto_counter_track<Tp>;
    TRACE_COUNTER(trait::name<Tp>::value, track::at(_idx.value, 0), _args...);
}
}  // namespace

void
post_process()
{
    ROCPROFSYS_VERBOSE(1,
                       "Post-processing %zu cpu frequency and memory usage entries...\n",
                       data.size());
    auto _process_frequencies = [](size_t _idx, size_t _offset) {
        using freq_track = perfetto_counter_track<category::cpu_freq>;

        const auto& _thread_info = thread_info::get(0, InternalTID);
        ROCPROFSYS_CI_THROW(!_thread_info, "Missing thread info for thread 0");
        if(!_thread_info) return;

        if(!freq_track::exists(_idx))
        {
            auto addendum = [&](const char* _v) {
                return JOIN(" ", "CPU", _v, JOIN("", '[', _idx, ']'), "(S)");
            };
            freq_track::emplace(_idx, addendum("Frequency"), "MHz");
        }

        for(auto& itr : data)
        {
            uint64_t _ts   = std::get<0>(itr);
            double   _freq = static_cast<double>(std::get<8>(itr).at(_offset));
            if(!_thread_info->is_valid_time(_ts)) continue;
            write_perfetto_counter_track<category::cpu_freq>(index{ _idx }, _ts, _freq);
        }

        auto _end_ts = _thread_info->get_stop();
        write_perfetto_counter_track<category::cpu_freq>(index{ _idx }, _end_ts, 0);
    };

    auto _process_cpu_rusage = []() {
        config_perfetto_counter_tracks(
            type_list<category::process_page, category::process_virt,
                      category::process_peak, category::process_context_switch,
                      category::process_page_fault, category::process_user_mode_time,
                      category::process_kernel_mode_time>{},
            { "Memory Usage", "Virtual Memory Usage", "Peak Memory", "Context Switches",
              "Page Faults", "User Time", "Kernel Time" },
            { "MB", "MB", "MB", "", "", "sec", "sec" });

        const auto& _thread_info = thread_info::get(0, InternalTID);
        ROCPROFSYS_CI_THROW(!_thread_info, "Missing thread info for thread 0");
        if(!_thread_info) return;

        for(auto& itr : data)
        {
            uint64_t _ts = std::get<0>(itr);
            if(!_thread_info->is_valid_time(_ts)) continue;

            double   _page = std::get<1>(itr);
            double   _virt = std::get<2>(itr);
            double   _peak = std::get<3>(itr);
            uint64_t _cntx = std::get<4>(itr);
            uint64_t _flts = std::get<5>(itr);
            double   _user = std::get<6>(itr);
            double   _kern = std::get<7>(itr);
            write_perfetto_counter_track<category::process_page>(_ts,
                                                                 _page / units::megabyte);
            write_perfetto_counter_track<category::process_virt>(_ts,
                                                                 _virt / units::megabyte);
            write_perfetto_counter_track<category::process_peak>(_ts,
                                                                 _peak / units::megabyte);
            write_perfetto_counter_track<category::process_context_switch>(_ts, _cntx);
            write_perfetto_counter_track<category::process_page_fault>(_ts, _flts);
            write_perfetto_counter_track<category::process_user_mode_time>(
                _ts, _user / units::sec);
            write_perfetto_counter_track<category::process_kernel_mode_time>(
                _ts, _kern / units::sec);
        }

        auto _end_ts = _thread_info->get_stop();
        write_perfetto_counter_track<category::process_page>(_end_ts, 0.0);
        write_perfetto_counter_track<category::process_virt>(_end_ts, 0.0);
        write_perfetto_counter_track<category::process_peak>(_end_ts, 0.0);
        write_perfetto_counter_track<category::process_context_switch>(_end_ts, 0);
        write_perfetto_counter_track<category::process_page_fault>(_end_ts, 0);
        write_perfetto_counter_track<category::process_user_mode_time>(_end_ts, 0.0);
        write_perfetto_counter_track<category::process_kernel_mode_time>(_end_ts, 0.0);
    };

    _process_cpu_rusage();

    //
    // FIXME: start  SQLite hack test
    //          - SQL create +  metrics insertion (will be deprecated)
    //  

    // SQLite connection, 
    sqlite3* conn = nullptr;
    auto output_file = std::string(tim::settings::instance()->get_output_path()) + "/cpu_results.db";

    // Delete database file if it exists
    FILE* check_file = fopen(output_file.c_str(), "r");
    if(check_file) {
        fclose(check_file);
        if(std::remove(output_file.c_str()) != 0) {
            ROCPROFSYS_VERBOSE(1, "Warning: Failed to delete existing database file: %s\n", output_file.c_str());
        } else {
            ROCPROFSYS_VERBOSE(1, "Removed existing database file: %s\n", output_file.c_str());
        }
    }

    // Open db connection
    int rc = sqlite3_open(output_file.c_str(), &conn);
    if(rc != SQLITE_OK) {
        ROCPROFSYS_VERBOSE(1, "Failed to open database: %s, error: %s\n", 
                          output_file.c_str(), sqlite3_errmsg(conn));
        sqlite3_close(conn);
        return;
    }
    
    sqlite3_busy_handler(conn, &sql_busy_handler, nullptr);
    ROCPROFSYS_VERBOSE(1, "Opened result file: %s\n", output_file.c_str());
    
    // Create schema
    execute_raw_sql_statements(conn, table_schema);

    uint64_t node_id = 1; // Fixed only one node
    uint64_t agent_id = 1; // Fixed only one cpu
    
    uint64_t track_id = 1;  
    uint64_t event_id = 1;
    uint64_t sample_id = 1;
   
    // Insert node + agent (CPU)
    std::stringstream sql_agent;
    sql_agent << "INSERT INTO _rocpd_node (id) VALUES (" << node_id << "); ";
    sql_agent << "INSERT INTO rocpd_agent (id, node_id, type, absolute_index) VALUES (" << agent_id << ", " << node_id << ", 'CPU', " << 0 << "); ";
    execute_raw_sql_statements(conn, sql_agent.str());

    // Insert default metrics into rocpd_string
    std::map<std::string, int> metric_names = {    
        {"Memory Usage", 1},    
        {"Virtual Memory Usage", 2},    
        {"Peak Memory", 3},    
        {"Context Switches", 4},    
        {"Page Faults", 5},    
        {"User Time", 6},    
        {"Kernel Time", 7}    
    };    
    uint64_t metric_id = 8; // Start from 8 for CPU frequency metrics

    std::stringstream sql_metric_names;
    for(const auto& pair : metric_names) {  
        sql_metric_names << "INSERT INTO rocpd_string (id, string) VALUES ("  
            << pair.second << ", '" << pair.first << "'); ";  
    }  
    execute_raw_sql_statements(conn, sql_metric_names.str());

    for(const auto& itr : data)
    {   
        uint64_t ts = std::get<0>(itr);

        double rss = std::get<1>(itr) / units::megabyte;
        double virt = std::get<2>(itr) / units::megabyte;
        double peak = std::get<3>(itr) / units::megabyte;
        uint64_t ctx = std::get<4>(itr);
        uint64_t faults = std::get<5>(itr);
        double user_time = std::get<6>(itr) / units::sec;
        double kernel_time = std::get<7>(itr) / units::sec;
       
        // Access CPU frequencies using cpu_freq component
        const auto& freq_data = std::get<8>(itr);
        const auto& enabled_cpus = component::cpu_freq::get_enabled_cpus();
        
        std::stringstream sql;

        // Create json metrics (will be replaced with rocpd_metric)
        std::stringstream json_metrics;
        json_metrics << "{";
        json_metrics << "\"rss_mb\": " << rss << ",";
        json_metrics << "\"virt_mb\": " << virt << ",";
        json_metrics << "\"peak_mb\": " << peak << ",";
        json_metrics << "\"context_switches\": " << ctx << ",";
        json_metrics << "\"page_faults\": " << faults << ",";
        json_metrics << "\"user_time_sec\": " << user_time << ",";
        json_metrics << "\"kernel_time_sec\": " << kernel_time ;
        for(const auto& cpu : enabled_cpus)
        {
            json_metrics << "," << "\"cpu_" << cpu << "_freq_mhz\": " << freq_data.at(cpu);
        }
        json_metrics << "}";

        // Insert event, track and sample
        sql << "INSERT INTO rocpd_event (id, metrics) VALUES (" << event_id << ", '" << json_metrics.str() << "'); ";
        
        sql << "INSERT INTO _rocpd_track (id, node_id) VALUES (" << track_id << ", " << node_id << "); ";
       
        sql << "INSERT INTO _rocpd_sample (id, track_id, timestamp, event_id) VALUES ("
        << sample_id << ", " << track_id << ", " << ts << ", " << event_id << "); ";

        // Insert metrics 
        sql << "INSERT INTO rocpd_metric (event_id, name_id, value) VALUES ("  
        << event_id << ", " << metric_names.find("Memory Usage")->second << ", "<< rss << "); "; // Memory Usage  
            
        sql << "INSERT INTO rocpd_metric (event_id, name_id, value) VALUES ("  
            << event_id << ", " << metric_names.find("Virtual Memory Usage")->second  << ", " << virt << "); "; // Virtual Memory Usage  
        
        sql << "INSERT INTO rocpd_metric (event_id, name_id, value) VALUES ("  
            << event_id << ", " << metric_names.find("Peak Memory")->second  << ", " << peak << "); "; // Peak Memory  
        
        sql << "INSERT INTO rocpd_metric (event_id, name_id, value) VALUES ("  
            << event_id << ", " << metric_names.find("Context Switches")->second  << ", " << ctx << "); "; // Context Switches  
        
        sql << "INSERT INTO rocpd_metric (event_id, name_id, value) VALUES ("  
            << event_id << ", " << metric_names.find("Page Faults")->second  << ", " << faults << "); "; // Page Faults  
        
        sql << "INSERT INTO rocpd_metric (event_id, name_id, value) VALUES ("  
            << event_id << ", " << metric_names.find("User Time")->second  << ", " << user_time << "); "; // User Time  
        
        sql << "INSERT INTO rocpd_metric (event_id, name_id, value) VALUES ("  
            << event_id << ", " << metric_names.find("Kernel Time")->second  << ", " << kernel_time << "); "; // Kernel Time  
            
        for(const auto& cpu : enabled_cpus)
        {
            std::string cpu_freq_name = "CPU Frequency [" + std::to_string(cpu) + "]";

            // Check if metric name already exists
            auto metric_it = metric_names.find(cpu_freq_name);
            int current_metric_id;
            
            if(metric_it == metric_names.end()) {
                sql << "INSERT INTO rocpd_string (id, string) VALUES ("  
                    << metric_id << ", '" << cpu_freq_name << "'); ";
                
                metric_names[cpu_freq_name] = metric_id;
                current_metric_id = metric_id;
                metric_id++;
            }
            else {
                current_metric_id = metric_it->second;
            }
          
            sql << "INSERT INTO rocpd_metric (event_id, name_id, value) VALUES ("
                << event_id << ", " << current_metric_id << ", " << freq_data.at(cpu) << "); ";
        }
    
        execute_raw_sql_statements(conn, sql.str());

        track_id++;
        event_id++;
        sample_id++;

    }

    // Make sure we clean up all resources
    sqlite3_close(conn);
    ROCPROFSYS_VERBOSE(1, "Database connection closed\n");
    
    //
    // FIXME : end SQLite hack test
    //

    auto& enabled_cpu_freqs = component::cpu_freq::get_enabled_cpus();
    for(auto itr = enabled_cpu_freqs.begin(); itr != enabled_cpu_freqs.end(); ++itr)
    {
        auto _idx    = *itr;
        auto _offset = std::distance(enabled_cpu_freqs.begin(), itr);
        _process_frequencies(_idx, _offset);
    }
    enabled_cpu_freqs.clear();
}
}  // namespace cpu_freq
}  // namespace rocprofsys

