// Copyright (c) 2018-2025 Advanced Micro Devices, Inc. All Rights Reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// with the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
// sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// * Redistributions of source code must retain the above copyright notice,
// this list of conditions and the following disclaimers.
//
// * Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimers in the
// documentation and/or other materials provided with the distribution.
//
// * Neither the names of Advanced Micro Devices, Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this Software without specific prior written permission.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS WITH
// THE SOFTWARE.

#if defined(NDEBUG)
#    undef NDEBUG
#endif

#include "library/rocm_smi.hpp"
#include "core/common.hpp"
#include "core/components/fwd.hpp"
#include "core/config.hpp"
#include "core/debug.hpp"
#include "core/gpu.hpp"
#include "core/perfetto.hpp"
#include "core/state.hpp"
#include "library/runtime.hpp"
#include "library/thread_info.hpp"

#include <timemory/backends/threading.hpp>
#include <timemory/components/timing/backends.hpp>
#include <timemory/mpl/type_traits.hpp>
#include <timemory/units.hpp>
#include <timemory/utility/delimit.hpp>
#include <timemory/utility/locking.hpp>

#include <rocm_smi/rocm_smi.h>

#include <cassert>
#include <cstdio> 
#include <chrono>
#include <ios>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <thread>

#include <sqlite3.h>


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

        CREATE TABLE IF NOT EXISTS
        "rocpd_gpu_metrics" (
            "id" INTEGER PRIMARY KEY AUTOINCREMENT,
            "timestamp" BIGINT,
            "node_id" INTEGER,
            "agent_id" INTEGER, 
            "device_id" INTEGER,
            "utilization" REAL,
            "temperature" REAL,
            "power" REAL,
            "memory_usage" REAL,
            "vcn_activity" TEXT, -- Stored as JSON array
            "jpeg_activity" TEXT, -- Stored as JSON array
            FOREIGN KEY (node_id) REFERENCES _rocpd_node (id),
            FOREIGN KEY (agent_id) REFERENCES rocpd_agent (id)
        );
    
    -- View for GPU metrics
        CREATE VIEW IF NOT EXISTS gpu_metrics AS
        SELECT 
            n.id as node_id,
            n.machine_id,
            a.id as agent_id,
            a.type as agent_type,
            a.absolute_index as gpu_index,
            t.id as track_id,
            t.pid,
            t.tid,
            s.id as sample_id,
            s.timestamp,
            e.id as event_id,
            e.metrics as metrics
        FROM _rocpd_node n
        JOIN rocpd_agent a ON a.node_id = n.id
        JOIN _rocpd_track t ON t.node_id = n.id
        JOIN _rocpd_sample s ON s.track_id = t.id
        JOIN rocpd_event e ON e.id = s.event_id
        WHERE a.type = 'GPU'
        ORDER BY s.timestamp;
    
    INSERT INTO
        "rocpd_metadata" (tag, value)
    VALUES
        ("schema_version", "3");
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

    
#define ROCPROFSYS_ROCM_SMI_CALL(...)                                                    \
    ::rocprofsys::rocm_smi::check_error(__FILE__, __LINE__, __VA_ARGS__)

namespace rocprofsys
{
namespace rocm_smi
{
using bundle_t          = std::deque<data>;
using sampler_instances = thread_data<bundle_t, category::rocm_smi>;

namespace
{
auto&
get_settings(uint32_t _dev_id)
{
    static auto _v = std::unordered_map<uint32_t, rocm_smi::settings>{};
    return _v[_dev_id];
}

bool&
is_initialized()
{
    static bool _v = false;
    return _v;
}

void
check_error(const char* _file, int _line, rsmi_status_t _code, bool* _option = nullptr)
{
    if(_code == RSMI_STATUS_SUCCESS)
        return;
    else if(_code == RSMI_STATUS_NOT_SUPPORTED && _option)
    {
        *_option = false;
        return;
    }

    const char* _msg = nullptr;
    auto        _err = rsmi_status_string(_code, &_msg);
    if(_err != RSMI_STATUS_SUCCESS)
        ROCPROFSYS_THROW("rsmi_status_string failed. No error message available. "
                         "Error code %i originated at %s:%i\n",
                         static_cast<int>(_code), _file, _line);
    ROCPROFSYS_THROW("[%s:%i] Error code %i :: %s", _file, _line, static_cast<int>(_code),
                     _msg);
}

std::atomic<State>&
get_state()
{
    static std::atomic<State> _v{ State::PreInit };
    return _v;
}
}  // namespace

//--------------------------------------------------------------------------------------//

size_t                           data::device_count     = 0;
std::set<uint32_t>               data::device_list      = {};
std::unique_ptr<data::promise_t> data::polling_finished = {};

data::data(uint32_t _dev_id) { sample(_dev_id); }

void
data::sample(uint32_t _dev_id)
{
    auto _ts = tim::get_clock_real_now<size_t, std::nano>();
    assert(_ts < std::numeric_limits<int64_t>::max());
    rsmi_gpu_metrics_t _gpu_metrics;

    auto _state = get_state().load();

    if(_state != State::Active) return;

    m_dev_id = _dev_id;
    m_ts     = _ts;

#define ROCPROFSYS_RSMI_GET(OPTION, FUNCTION, ...)                                       \
    if(OPTION)                                                                           \
    {                                                                                    \
        try                                                                              \
        {                                                                                \
            ROCPROFSYS_ROCM_SMI_CALL(FUNCTION(__VA_ARGS__), &OPTION);                    \
        } catch(std::runtime_error & _e)                                                 \
        {                                                                                \
            ROCPROFSYS_VERBOSE_F(                                                        \
                0, "[%s] Exception: %s. Disabling future samples from rocm-smi...\n",    \
                #FUNCTION, _e.what());                                                   \
            get_state().store(State::Disabled);                                          \
        }                                                                                \
    }

    ROCPROFSYS_RSMI_GET(get_settings(m_dev_id).busy, rsmi_dev_busy_percent_get, _dev_id,
                        &m_busy_perc);
    ROCPROFSYS_RSMI_GET(get_settings(m_dev_id).temp, rsmi_dev_temp_metric_get, _dev_id,
                        RSMI_TEMP_TYPE_JUNCTION, RSMI_TEMP_CURRENT, &m_temp);
    RSMI_POWER_TYPE power_type = RSMI_CURRENT_POWER;
    ROCPROFSYS_RSMI_GET(get_settings(m_dev_id).power, rsmi_dev_power_get, _dev_id,
                        &m_power, &power_type)
    ROCPROFSYS_RSMI_GET(get_settings(m_dev_id).mem_usage, rsmi_dev_memory_usage_get,
                        _dev_id, RSMI_MEM_TYPE_VRAM, &m_mem_usage);
    ROCPROFSYS_ROCM_SMI_CALL(rsmi_dev_gpu_metrics_info_get(_dev_id, &_gpu_metrics));

    for(const auto& v_activity : _gpu_metrics.vcn_activity)
    {
        if(v_activity != UINT16_MAX) m_vcn_metrics[_dev_id].push_back(v_activity);
    }
    for(const auto& j_activity : _gpu_metrics.jpeg_activity)
    {
        if(j_activity != UINT16_MAX) m_jpeg_metrics[_dev_id].push_back(j_activity);
    }

#undef ROCPROFSYS_RSMI_GET
}

void
data::print(std::ostream& _os) const
{
    std::stringstream _ss{};
    _ss << "device: " << m_dev_id << ", busy = " << m_busy_perc << "%, temp = " << m_temp
        << ", power = " << m_power << ", memory usage = " << m_mem_usage;
    _os << _ss.str();
}

namespace
{
std::vector<unique_ptr_t<bundle_t>*> _bundle_data{};
}

void
config()
{
    _bundle_data.resize(data::device_count, nullptr);
    for(size_t i = 0; i < data::device_count; ++i)
    {
        if(data::device_list.count(i) > 0)
        {
            _bundle_data.at(i) = &sampler_instances::get()->at(i);
            if(!*_bundle_data.at(i))
                *_bundle_data.at(i) = unique_ptr_t<bundle_t>{ new bundle_t{} };
        }
    }

    data::get_initial().resize(data::device_count);
    for(auto itr : data::device_list)
        data::get_initial().at(itr).sample(itr);
}

void
sample()
{
    for(auto itr : data::device_list)
    {
        if(rocm_smi::get_state() != State::Active) continue;
        ROCPROFSYS_DEBUG_F("Polling rocm-smi for device %u...\n", itr);
        auto& _data = *_bundle_data.at(itr);
        if(!_data) continue;
        _data->emplace_back(data{ itr });
        ROCPROFSYS_DEBUG_F("    %s\n", TIMEMORY_JOIN("", _data->back()).c_str());
    }
}

void
set_state(State _v)
{
    rocm_smi::get_state().store(_v);
}

std::vector<data>&
data::get_initial()
{
    static std::vector<data> _v{};
    return _v;
}

bool
data::setup()
{
    perfetto_counter_track<data>::init();
    rocm_smi::set_state(State::PreInit);
    return true;
}

bool
data::shutdown()
{
    ROCPROFSYS_DEBUG("Shutting down rocm-smi...\n");
    rocm_smi::set_state(State::Finalized);
    return true;
}

#define GPU_METRIC(COMPONENT, ...)                                                       \
    if constexpr(tim::trait::is_available<COMPONENT>::value)                             \
    {                                                                                    \
        auto* _val = _v.get<COMPONENT>();                                                \
        if(_val)                                                                         \
        {                                                                                \
            _val->set_value(itr.__VA_ARGS__);                                            \
            _val->set_accum(itr.__VA_ARGS__);                                            \
        }                                                                                \
    }

void
data::post_process(uint32_t _dev_id)
{
    using component::sampling_gpu_busy;
    using component::sampling_gpu_jpeg;
    using component::sampling_gpu_memory;
    using component::sampling_gpu_power;
    using component::sampling_gpu_temp;
    using component::sampling_gpu_vcn;

    if(device_count < _dev_id) return;

    auto&       _rocm_smi_v = sampler_instances::get()->at(_dev_id);
    auto        _rocm_smi   = (_rocm_smi_v) ? *_rocm_smi_v : std::deque<rocm_smi::data>{};
    const auto& _thread_info = thread_info::get(0, InternalTID);

    ROCPROFSYS_VERBOSE(1, "Post-processing %zu rocm-smi samples from device %u\n",
                       _rocm_smi.size(), _dev_id);
    ROCPROFSYS_CI_THROW(!_thread_info, "Missing thread info for thread 0");
    if(!_thread_info) return;
                        
    //Open SQLite connection, output to same location as csv
    // Delete existing database file if it exists to ensure fresh data on each run

    sqlite3* conn        = nullptr;
    auto output_file = std::string(tim::settings::instance()->get_output_path()) + "/gpu_metrics.db";

    // FILE* check_file = fopen(output_file.c_str(), "r");
    // if(check_file) {
    //     fclose(check_file);
    //     std::remove(output_file.c_str());
    //     ROCPROFSYS_VERBOSE(1, "Removed existing database file: %s\n", output_file.c_str());
    // }

    //open db connection
    sqlite3_open(output_file.c_str(), &conn);
    sqlite3_busy_handler(conn, &sql_busy_handler, nullptr);

    ROCPROFSYS_VERBOSE(1, "Opened result file: %s\n", output_file.c_str());
    execute_raw_sql_statements(conn, table_schema);

    // Get the highest existing IDs from the database
    uint64_t node_id = 1;
    uint64_t agent_id = 1;
    uint64_t track_id = 1;
    uint64_t event_id = 1;
    uint64_t sample_id = 1;
    
    // Query for the max IDs to avoid conflicts
    sqlite3_stmt* stmt = nullptr;
    
    const char* max_node_sql = "SELECT MAX(id) FROM _rocpd_node";
    if (sqlite3_prepare_v2(conn, max_node_sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
            node_id = sqlite3_column_int64(stmt, 0) + 1;
        }
        sqlite3_finalize(stmt);
    }
    
    const char* max_agent_sql = "SELECT MAX(id) FROM rocpd_agent";
    if (sqlite3_prepare_v2(conn, max_agent_sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
            agent_id = sqlite3_column_int64(stmt, 0) + 1;
        }
        sqlite3_finalize(stmt);
    }
    
    const char* max_track_sql = "SELECT MAX(id) FROM _rocpd_track";
    if (sqlite3_prepare_v2(conn, max_track_sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
            track_id = sqlite3_column_int64(stmt, 0) + 1;
        }
        sqlite3_finalize(stmt);
    }
    
    const char* max_event_sql = "SELECT MAX(id) FROM rocpd_event";
    if (sqlite3_prepare_v2(conn, max_event_sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
            event_id = sqlite3_column_int64(stmt, 0) + 1;
        }
        sqlite3_finalize(stmt);
    }
    
    const char* max_sample_sql = "SELECT MAX(id) FROM _rocpd_sample";
    if (sqlite3_prepare_v2(conn, max_sample_sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
            sample_id = sqlite3_column_int64(stmt, 0) + 1;
        }
        sqlite3_finalize(stmt);
    }
    
    ROCPROFSYS_VERBOSE(1, "Starting IDs for device %u: node=%lu, agent=%lu, track=%lu, event=%lu, sample=%lu\n", 
                        _dev_id, node_id, agent_id, track_id, event_id, sample_id);
   
    std::map<std::string, int> gpu_node_agent; 
    std::string gpu_id  = std::to_string(_dev_id);
    
    for(const auto& itr : _rocm_smi)
    {
        if(itr.m_dev_id != _dev_id) continue;
        
       // Get VCN activities as array
        std::stringstream vcn_activity;
        vcn_activity << "[";
        if(!itr.m_vcn_metrics.empty()) {
            bool first = true;
            for(const auto& vcn : itr.m_vcn_metrics.at(_dev_id)) {
                if (!first) vcn_activity << ",";
                vcn_activity << vcn;
                first = false;
            }
        } else {
            vcn_activity << "0";
        }
        vcn_activity << "]";

        
        // Get JPEG activities as array 
        std::stringstream jpeg_metric;
        jpeg_metric << "[";
        if(!itr.m_jpeg_metrics.empty()) {
            bool first = true;
            for(const auto& jpeg : itr.m_jpeg_metrics.at(_dev_id)) {
                if (!first) jpeg_metric << ",";
                jpeg_metric << jpeg;
                first = false;
            }
        } else {
            jpeg_metric << "0";
        }
        jpeg_metric << "]";

        std::stringstream json_metrics;
        json_metrics << "{";
        json_metrics << "\"utilization\": " << itr.m_busy_perc << ",";
        json_metrics << "\"temperature\": " << itr.m_temp / 1.0e3 << ",";
        json_metrics << "\"power\": " << itr.m_power / 1.0e6 << ",";
        json_metrics << "\"memoryUsage\": " << itr.m_mem_usage / static_cast<double>(units::megabyte)  << ",";
        json_metrics << "\"vcnActivity\": " << vcn_activity.str() << ",";
        json_metrics << "\"JPEGActivity\": " << jpeg_metric.str()  << ",";
        json_metrics << "}";

        if (gpu_node_agent.find(gpu_id) == gpu_node_agent.end()) {  
            // Insert node
            std::stringstream sql;
            sql << "INSERT INTO _rocpd_node (id, hash, machine_id) VALUES ("
                << node_id << ", " << node_id << ", 'local'); ";
                
            sql << "INSERT INTO rocpd_agent (id, node_id, type, absolute_index) VALUES ("
                << agent_id << ", " << node_id << ", 'GPU', " << gpu_id << "); ";
            
            execute_raw_sql_statements(conn, sql.str());
            // Insert into map  
            gpu_node_agent[gpu_id] = node_id;  
        } else {  
            node_id = gpu_node_agent[gpu_id];  
        }  


        std::stringstream sql;
        sql << "INSERT INTO _rocpd_track (id, node_id) VALUES ("
            << track_id << ", " << node_id << "); ";

        sql << "INSERT INTO rocpd_event (id, metrics) VALUES ("
            << event_id << ", '" << json_metrics.str() << "'); ";

        sql << "INSERT INTO _rocpd_sample (id, track_id, timestamp, event_id) VALUES ("
            << sample_id << ", " << track_id << ", " << itr.m_ts << ", " << event_id << "); ";

        sql << "INSERT INTO rocpd_gpu_metrics (timestamp, node_id, agent_id, device_id, "
                    << "utilization, temperature, power, memory_usage, vcn_activity, jpeg_activity) VALUES ("
                    << itr.m_ts << ", "
                    << node_id << ", "
                    << agent_id << ", "
                    << itr.m_dev_id << ", "
                    << itr.m_busy_perc << ", "
                    << itr.m_temp / 1.0e3 << ", "
                    << itr.m_power / 1.0e6 << ", "
                    << itr.m_mem_usage / static_cast<double>(units::megabyte) << ", "
                    << "'" << vcn_activity.str() << "', "
                    << "'" << jpeg_metric.str() << "');";

        execute_raw_sql_statements(conn, sql.str());

        node_id++;
        agent_id++;
        
        track_id++;
        event_id++;
        sample_id++;
    }

    //close db connection
    sqlite3_close(conn);
    
    //
    //end SQLite hack test
    //


     // Write metrics to CSV file
     auto output_path = std::string(tim::settings::instance()->get_output_path()) 
                       + "/gpu_metrics_" + std::to_string(_dev_id) + ".csv";
     std::ofstream outfile(output_path);
 
     // Write CSV header
     outfile << "Timestamp,DeviceID,Utilization(%),Temperature(C),Power(W),"
             << "MemoryUsage(MB),VCN_Activity(%),JPEG_Activity(%)\n";
 
     // Write samples
     for(const auto& itr : _rocm_smi)
     {
         if(itr.m_dev_id != _dev_id) continue;
 
         // Basic metrics
         outfile << itr.m_ts << ","
                 << itr.m_dev_id << ","
                 << itr.m_busy_perc << ","
                 << itr.m_temp / 1.0e3 << "," // Convert to Celsius
                 << itr.m_power / 1.0e6 << "," // Convert to Watts
                 << itr.m_mem_usage / static_cast<double>(units::megabyte) << ",";
 
         // VCN metrics
        if(!itr.m_vcn_metrics.empty()) {
            bool first = true;
            for(const auto& vcn : itr.m_vcn_metrics.at(_dev_id)) {
                if (!first) outfile << "|";  // Use pipe as separator between multiple VCN values
                outfile << vcn;
                first = false;
            }
            outfile << ",";
        } else {
            outfile << "0,";
        }

        // JPEG metrics
        if(!itr.m_jpeg_metrics.empty()) {
            bool first = true;
            for(const auto& jpeg : itr.m_jpeg_metrics.at(_dev_id)) {
                if (!first) outfile << "|";  // Use pipe as separator between multiple JPEG values
                outfile << jpeg;
                first = false;
            }
            outfile << "\n";
        } else {
            outfile << "0\n";
        }
     }
 
     outfile.close();
     ROCPROFSYS_VERBOSE(1, "GPU metrics written to: %s\n", output_path.c_str());
 
// ----- END HACK
    auto _settings = get_settings(_dev_id);

    auto _process_perfetto = [&]() {
        auto _idx = std::array<uint64_t, 6>{};
        {
            _idx.fill(_idx.size());
            uint64_t nidx = 0;
            if(_settings.busy) _idx.at(0) = nidx++;
            if(_settings.temp) _idx.at(1) = nidx++;
            if(_settings.power) _idx.at(2) = nidx++;
            if(_settings.mem_usage) _idx.at(3) = nidx++;
            if(_settings.vcn_activity) _idx.at(4) = nidx++;
            if(_settings.jpeg_activity) _idx.at(5) = nidx++;
        }

        for(auto& itr : _rocm_smi)
        {
            using counter_track = perfetto_counter_track<data>;
            if(itr.m_dev_id != _dev_id) continue;
            if(!counter_track::exists(_dev_id))
            {
                auto addendum = [&](const char* _v) {
                    return JOIN(" ", "GPU", _v, JOIN("", '[', _dev_id, ']'), "(S)");
                };
                auto addendum_blk = [&](std::size_t _i, const char* _metric) {
                    if(_i < 10)
                    {
                        return JOIN(" ", "GPU", JOIN("", '[', _dev_id, ']'), _metric,
                                    JOIN("", "[0", _i, ']'), "(S)");
                    }
                    else
                    {
                        return JOIN(" ", "GPU", JOIN("", '[', _dev_id, ']'), _metric,
                                    JOIN("", '[', _i, ']'), "(S)");
                    }
                };

                if(_settings.busy) counter_track::emplace(_dev_id, addendum("Busy"), "%");
                if(_settings.temp)
                    counter_track::emplace(_dev_id, addendum("Temperature"), "deg C");
                if(_settings.power)
                    counter_track::emplace(_dev_id, addendum("Power"), "watts");
                if(_settings.mem_usage)
                    counter_track::emplace(_dev_id, addendum("Memory Usage"),
                                           "megabytes");
                if(_settings.vcn_activity)
                {
                    for(const auto& [dev_id, metrics] : itr.m_vcn_metrics)
                    {
                        for(std::size_t i = 0; i < std::size(metrics); ++i)
                        {
                            counter_track::emplace(
                                _dev_id, addendum_blk(i, "  VCN Activity"), "%");
                        }
                    }
                }
                if(_settings.jpeg_activity)
                {
                    for(const auto& [dev_id, metrics] : itr.m_jpeg_metrics)
                    {
                        for(std::size_t i = 0; i < std::size(metrics); ++i)
                        {
                            counter_track::emplace(_dev_id,
                                                   addendum_blk(i, "JPEG Activity"), "%");
                        }
                    }
                }
            }
            uint64_t _ts = itr.m_ts;
            if(!_thread_info->is_valid_time(_ts)) continue;

            double _busy  = itr.m_busy_perc;
            double _temp  = itr.m_temp / 1.0e3;
            double _power = itr.m_power / 1.0e6;
            double _usage = itr.m_mem_usage / static_cast<double>(units::megabyte);

            if(_settings.busy)
                TRACE_COUNTER("device_busy", counter_track::at(_dev_id, _idx.at(0)), _ts,
                              _busy);
            if(_settings.temp)
                TRACE_COUNTER("device_temp", counter_track::at(_dev_id, _idx.at(1)), _ts,
                              _temp);
            if(_settings.power)
                TRACE_COUNTER("device_power", counter_track::at(_dev_id, _idx.at(2)), _ts,
                              _power);
            if(_settings.mem_usage)
                TRACE_COUNTER("device_memory_usage",
                              counter_track::at(_dev_id, _idx.at(3)), _ts, _usage);
            if(_settings.vcn_activity)
            {
                for(const auto& [dev_id, metrics] : itr.m_vcn_metrics)
                {
                    for(std::size_t i = 0; i < std::size(metrics); ++i)
                    {
                        double _vcn_activity = metrics[i];
                        TRACE_COUNTER("device_vcn_activity",
                                      counter_track::at(_dev_id, _idx.at(4) + i), _ts,
                                      _vcn_activity);
                    }
                }
            }
            if(_settings.jpeg_activity)
            {
                for(const auto& [dev_id, metrics] : itr.m_jpeg_metrics)
                {
                    for(std::size_t i = 0; i < std::size(metrics); ++i)
                    {
                        double _jpeg_activity = metrics[i];
                        TRACE_COUNTER("device_jpeg_activity",
                                      counter_track::at(_dev_id, _idx.at(5) + i), _ts,
                                      _jpeg_activity);
                    }
                }
            }
        }
    };

    if(get_use_perfetto()) _process_perfetto();
}

//--------------------------------------------------------------------------------------//

void
setup()
{
    auto_lock_t _lk{ type_mutex<category::rocm_smi>() };

    if(is_initialized() || !get_use_rocm_smi()) return;

    ROCPROFSYS_SCOPED_SAMPLING_ON_CHILD_THREADS(false);

    // assign the data value to determined by rocm-smi
    data::device_count = device_count();

    auto _devices_v = get_sampling_gpus();
    for(auto& itr : _devices_v)
        itr = tolower(itr);
    if(_devices_v == "off")
        _devices_v = "none";
    else if(_devices_v == "on")
        _devices_v = "all";
    bool _all_devices = _devices_v.find("all") != std::string::npos || _devices_v.empty();
    bool _no_devices  = _devices_v.find("none") != std::string::npos;

    std::set<uint32_t> _devices = {};
    auto               _emplace = [&_devices](auto idx) {
        if(idx < data::device_count) _devices.emplace(idx);
    };

    if(_all_devices)
    {
        for(uint32_t i = 0; i < data::device_count; ++i)
            _emplace(i);
    }
    else if(!_no_devices)
    {
        auto _enabled = tim::delimit(_devices_v, ",; \t");
        for(auto&& itr : _enabled)
        {
            if(itr.find_first_not_of("0123456789-") != std::string::npos)
            {
                ROCPROFSYS_THROW("Invalid GPU specification: '%s'. Only numerical values "
                                 "(e.g., 0) or ranges (e.g., 0-7) are permitted.",
                                 itr.c_str());
            }

            if(itr.find('-') != std::string::npos)
            {
                auto _v = tim::delimit(itr, "-");
                ROCPROFSYS_CONDITIONAL_THROW(_v.size() != 2,
                                             "Invalid GPU range specification: '%s'. "
                                             "Required format N-M, e.g. 0-4",
                                             itr.c_str());
                for(auto i = std::stoul(_v.at(0)); i < std::stoul(_v.at(1)); ++i)
                    _emplace(i);
            }
            else
            {
                _emplace(std::stoul(itr));
            }
        }
    }

    data::device_list = _devices;

    auto _metrics = get_setting_value<std::string>("ROCPROFSYS_ROCM_SMI_METRICS");

    try
    {
        for(auto itr : _devices)
        {
            uint16_t dev_id = 0;
            ROCPROFSYS_ROCM_SMI_CALL(rsmi_dev_id_get(itr, &dev_id));
            // dev_id holds the device ID of device i, upon a successful call

            if(_metrics && !_metrics->empty())
            {
                using key_pair_t     = std::pair<std::string_view, bool&>;
                const auto supported = std::unordered_map<std::string_view, bool&>{
                    key_pair_t{ "busy", get_settings(dev_id).busy },
                    key_pair_t{ "temp", get_settings(dev_id).temp },
                    key_pair_t{ "power", get_settings(dev_id).power },
                    key_pair_t{ "mem_usage", get_settings(dev_id).mem_usage },
                    key_pair_t{ "vcn_activity", get_settings(dev_id).vcn_activity },
                    key_pair_t{ "jpeg_activity", get_settings(dev_id).jpeg_activity },
                };

                get_settings(dev_id) = { false, false, false, false, false, false };
                for(const auto& metric : tim::delimit(*_metrics, ",;:\t\n "))
                {
                    auto iitr = supported.find(metric);
                    if(iitr == supported.end())
                        ROCPROFSYS_FAIL_F("unsupported rocm-smi metric: %s\n",
                                          metric.c_str());

                    ROCPROFSYS_VERBOSE_F(1, "Enabling rocm-smi metric '%s'\n",
                                         metric.c_str());
                    iitr->second = true;
                }
            }
        }

        is_initialized() = true;

        data::setup();
    } catch(std::runtime_error& _e)
    {
        ROCPROFSYS_VERBOSE(0, "Exception thrown when initializing rocm-smi: %s\n",
                           _e.what());
        data::device_list = {};
    }
}

void
shutdown()
{
    auto_lock_t _lk{ type_mutex<category::rocm_smi>() };

    if(!is_initialized()) return;

    try
    {
        if(data::shutdown())
        {
            ROCPROFSYS_ROCM_SMI_CALL(rsmi_shut_down());
        }
    } catch(std::runtime_error& _e)
    {
        ROCPROFSYS_VERBOSE(0, "Exception thrown when shutting down rocm-smi: %s\n",
                           _e.what());
    }

    is_initialized() = false;
}

void
post_process()
{
    for(auto itr : data::device_list)
        data::post_process(itr);
}

uint32_t
device_count()
{
    return gpu::rsmi_device_count();
}
}  // namespace rocm_smi
}  // namespace rocprofsys

ROCPROFSYS_INSTANTIATE_EXTERN_COMPONENT(
    TIMEMORY_ESC(data_tracker<double, rocprofsys::component::backtrace_gpu_busy>), true,
    double)

ROCPROFSYS_INSTANTIATE_EXTERN_COMPONENT(
    TIMEMORY_ESC(data_tracker<double, rocprofsys::component::backtrace_gpu_temp>), true,
    double)

ROCPROFSYS_INSTANTIATE_EXTERN_COMPONENT(
    TIMEMORY_ESC(data_tracker<double, rocprofsys::component::backtrace_gpu_power>), true,
    double)

ROCPROFSYS_INSTANTIATE_EXTERN_COMPONENT(
    TIMEMORY_ESC(data_tracker<double, rocprofsys::component::backtrace_gpu_memory>), true,
    double)

ROCPROFSYS_INSTANTIATE_EXTERN_COMPONENT(
    TIMEMORY_ESC(data_tracker<double, rocprofsys::component::backtrace_gpu_vcn>), true,
    double)

ROCPROFSYS_INSTANTIATE_EXTERN_COMPONENT(
    TIMEMORY_ESC(data_tracker<double, rocprofsys::component::backtrace_gpu_jpeg>), true,
    double)
