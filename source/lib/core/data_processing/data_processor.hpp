#pragma once 

#include <string>
#include <cstdint>
#include <thread>
#include <unordered_map>
#include <functional>
#include <any>

#include "utils.hpp"
#include "core/data_storage/database.hpp"
#include "core/data_storage/queries/table_insert_query.hpp"

#include "core/categories.hpp"      // For tim::trait::perfetto_category
#include <timemory/mpl/types.hpp>   // For tim::type_list


namespace rocprofsys {
namespace {
    using add_event_stmt = std::function<void(int, uint32_t, uint32_t, uint32_t, uint32_t)>;
}

struct data_processor {
    enum class category_id {
        smi_device_busy,
        smi_device_temperature,
        smi_device_power,
        smi_device_memory_usage,
    };

    enum class correlation_id {
        smi_unused
    };

    enum class agent_type {
        cpu,
        gpu,
        unknown
    };

    struct agent_descriptor {
        uint64_t id;
        uint32_t node_id;
        agent_type type;
        int32_t absolute_index;
        int32_t logical_index;     
        int32_t type_index;      
        int64_t uuid;
        const char* name;
        const char* model_name;
        const char* vendor_name;
        const char* product_name;
        const char* user_name;
        const char* extdata;
    };

    struct event_descriptor {
       int category_id;
       int correlation_id;
       int stack_id;
       int parent_stack_id;
       const char* args;
       const char* metrics;
       const char* call_stack;
       const char* line_info;
       const char* extdata;
    };

    struct track_descriptor {
        std::string name;       
        int node_id;      
        int pid;              
        int tid;    
        std::string extdata;
    };

    struct sample_descriptor {
        uint32_t track_id;     
        uint64_t timestamp;     
        uint32_t event_id;     
        const char* extdata;    
    };

    struct pmc_descriptor {
        std::string name;
        std::string symbol;   
        std::string target_arch;
        uint32_t agent_id;
        int event_code;
        int instance_id;
        std::string description;
        std::string long_description;
        std::string component; 
        std::string units;
        std::string value_type;
        std::string block;         
        std::string expression;     
        int is_constant;
        int is_derived;
        std::string extdata;
    };


    static data_processor& get_instance();

    int create_string(std::string_view str);

    void create_agent(const agent_descriptor& agent);
    
    uint32_t add_track(const track_descriptor& track);
    
    uint32_t add_event(const event_descriptor& event);

    uint32_t add_sample(const sample_descriptor& sample);
    
    uint32_t add_pmc(const pmc_descriptor& pmc);
    
    void add_pmc_event(uint32_t pmc_id, double value, uint32_t event_id = 0);
    
    uint32_t find_pmc_id(const std::string& name);
    uint32_t find_string_id(const std::string& str);

    template <typename... Types>
    void init_db_counter_cpu_tracks(tim::type_list<Types...>, 
                                    std::array<const char*, sizeof...(Types)> units = {},
                                    std::array<const char*, sizeof...(Types)> custom_names = {},
                                    uint32_t agent_id = 0)
    {
        auto insert_cpu_counter = [this, &units, &custom_names, agent_id](auto _t, size_t idx) {
            using type = std::decay_t<decltype(_t)>;
            
            // Use custom name if provided, otherwise use the type trait name
            const char* name = (idx < custom_names.size() && custom_names[idx]) 
                ? custom_names[idx] 
                : tim::trait::perfetto_category<type>::value;
            const char* desc = tim::trait::perfetto_category<type>::description;
            
            pmc_descriptor pmc;
            pmc.name = name;
            pmc.description = desc;
            // Set units if provided
            if (idx < units.size() && units[idx]) {
                pmc.units = units[idx];
            }        
            pmc.target_arch = "CPU"; 
            pmc.agent_id = agent_id; //0
            pmc.value_type = "ABS";   
            pmc.is_constant = 0;
            pmc.is_derived = 0;
            pmc.event_code = 0;
            pmc.instance_id = 0;
            
            add_pmc(pmc);
        };
        
        // Apply the lambda to each type in the type list with index
        size_t idx = 0;
        (insert_cpu_counter(Types{}, idx++), ...);
    }

private:
    data_processor();

    data_processor(data_processor&) = delete;

    data_processor& operator=(const data_processor&) = delete;

private:
    std::unordered_map<std::string_view, uint32_t> _track_name_map;
    std::unordered_map<category_id, int> _category_map;
    std::unordered_map<std::string, uint32_t> _pmc_name_map; // TODO 
    std::unordered_map<std::string, uint32_t> _strings_map; // TODO 

    uint32_t _track_id{1};
    uint32_t _string_id{1};
    uint32_t _sample_id{1};
    uint32_t _event_id{1};
    uint32_t _pmc_id = {1};
};

} // namespace rocprofsys

