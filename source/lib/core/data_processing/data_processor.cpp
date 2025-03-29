#include "data_processor.hpp"

namespace rocprofsys{
namespace {
    
static inline constexpr const char* 
get_agent_type(data_processor::agent_type agent_type) {
    switch (agent_type)
    {
        case data_processor::agent_type::cpu:
            return "CPU";
        
        case data_processor::agent_type::gpu:
            return "GPU";
        
        default: 
            return "UNKNOWN";
    }
}

}

constexpr char* CATEHORY_NAME_SMI_DEVICE_BUSY = "Device Busy";
constexpr char* CATEHORY_NAME_SMI_DEVICE_MEMORY_USAGE = "Device Memory Usage";
constexpr char* CATEHORY_NAME_SMI_DEVICE_POWER = "Device Power";
constexpr char* CATEHORY_NAME_SMI_DEVICE_TEMPERATURE = "Device Temperature";

data_processor::data_processor() {
    data_storage::database::get_instance().initialize_schema();

    auto position = create_string(CATEHORY_NAME_SMI_DEVICE_BUSY);
    _category_map[category_id::smi_device_busy] = position;

    position = create_string(CATEHORY_NAME_SMI_DEVICE_MEMORY_USAGE);
    _category_map[category_id::smi_device_memory_usage] = position;

    position = create_string(CATEHORY_NAME_SMI_DEVICE_POWER);
    _category_map[category_id::smi_device_power] = position;

    position = create_string(CATEHORY_NAME_SMI_DEVICE_TEMPERATURE);
    _category_map[category_id::smi_device_temperature] = position;
}

data_processor& data_processor::get_instance(){
    static data_processor _instance;
    return _instance;
}

int data_processor::create_string(std::string_view str){
    data_storage::queries::table_insert_query query;
    data_storage::database::get_instance()
                            .execute_query(
                                query.set_table_name("rocpd_string")
                                    .set_columns("id", "string")
                                    .set_values(_string_id, str)
                                    .get_query_string());
    
    uint32_t id = _string_id++;
    std::string str_copy(str);
    _strings_map[str_copy]= id;
    return id;
}

void 
data_processor::create_agent(const data_processor::agent_descriptor& agent) {
    data_storage::queries::table_insert_query query;
    data_storage::database::get_instance()
                            .execute_query(
                                query.set_table_name("rocpd_agent")
                                    .set_columns("id", "node_id", "type", "absolute_index", "logical_index", "type_index", "uuid", "name", 
                                                    "model_name", "vendor_name", "product_name", "user_name", "extdata")
                                    .set_values(agent.id, agent.node_id, get_agent_type(agent.type), agent.absolute_index, agent.logical_index, agent.type_index, 
                                                agent.uuid, agent.name, agent.model_name, agent.vendor_name, agent.product_name, agent.user_name, agent.extdata)
                                    .get_query_string());

    if (std::string(get_agent_type(agent.type)) == "GPU") {
        this->_gpu_agents[agent.type_index] = agent.id;  
    }
}


uint32_t 
data_processor::add_track(const data_processor::track_descriptor& track) {

    uint32_t name_id = find_string_id(track.name);
    
    static auto _add_track_stmt = []() {
        data_storage::queries::table_insert_query query_builder;
        auto query = query_builder.set_table_name("_rocpd_track")
                                  .set_columns(
                                      "id", "node_id", "pid", "tid", "name_id", "extdata")
                                  .set_values('?', '?', '?', '?', '?', '?')
                                  .get_query_string();
        return data_storage::database::get_instance().create_statment_executor<
                                uint32_t,      // id
                                int,           // node_id
                                int,           // pid
                                int,           // tid
                                int,           // name_id
                                const char*    // extdata
                            >(query);
    }();
    
    uint32_t id = _track_id++;
    _add_track_stmt(id, track.node_id, track.pid, track.tid, name_id, track.extdata.empty() ? "{}" : track.extdata.c_str());
    
    return id;
}

uint32_t data_processor::add_event(const data_processor::event_descriptor& event) {

    static auto _add_event_stmt = []{
        data_storage::queries::table_insert_query query_builder;
        auto query = query_builder.set_table_name("rocpd_event")
                                    .set_columns(
                                        "id", "category_id", "correlation_id", "stack_id", "parent_stack_id", 
                                        "args", "metrics", "call_stack", "line_info", "extdata")
                                    .set_values('?', '?', '?', '?', '?', '?', '?', '?', '?', '?')
                                    .get_query_string();
        return data_storage::database::get_instance().create_statment_executor<
                                    uint32_t,      // id
                                    int,           // category_id
                                    int,           // correlation_id
                                    int,           // stack_id
                                    int,           // parent_stack_id
                                    const char*,   // args
                                    const char*,   // metrics
                                    const char*,   // call_stack
                                    const char*,   // line_info
                                    const char*    // extdata
                                >(query);                                           
    }();

    // static std::string metrics;
    // metrics = event.metrics.serialize();
    // std::cout << "Add event. Metrics " << event.metrics << std::endl;

    uint32_t id = _event_id++;
    _add_event_stmt(id, event.category_id, event.correlation_id, event.stack_id, event.parent_stack_id, 
                    event.args, event.metrics, event.call_stack, event.line_info, event.extdata);

    return id;
}

uint32_t data_processor::add_sample(const data_processor::sample_descriptor& sample){
    static auto _add_sample_stmt = []{
        data_storage::queries::table_insert_query query_builder;
        auto query = query_builder.set_table_name("_rocpd_sample")
                                  .set_columns(
                                      "id", "track_id", "timestamp", "event_id", "extdata")
                                  .set_values('?', '?', '?', '?', '?')
                                  .get_query_string();
        return data_storage::database::get_instance().create_statment_executor<
                                    uint32_t,      // id
                                    uint32_t,      // track_id
                                    uint64_t,      // timestamp
                                    uint32_t,      // event_id
                                    const char*    // extdata
                                >(query);
    }();
    
    uint32_t id = _sample_id++;
    _add_sample_stmt(id, sample.track_id,  sample.timestamp, sample.event_id, sample.extdata);
    return id;
}


uint32_t data_processor::add_pmc(const pmc_descriptor& pmc) {  
    static auto _add_pmc_stmt = []() {  
        data_storage::queries::table_insert_query query_builder;  
        auto query = query_builder.set_table_name("rocpd_pmc")  
                                  .set_columns(  
                                      "id", "target_arch", "agent_id", "name", "symbol",   
                                      "event_code", "instance_id", "description", "long_description",  
                                      "component", "units", "value_type", "block", "expression",  
                                      "is_constant", "is_derived", "extdata")  
                                  .set_values('?', '?', '?', '?', '?',   
                                              '?', '?', '?', '?', '?',   
                                              '?', '?', '?', '?', '?',  
                                              '?', '?')  
                                  .get_query_string();  
          
        return data_storage::database::get_instance().create_statment_executor<  
            uint32_t, const char*, uint32_t, const char*, const char*,  
            int, int, const char*, const char*, const char*,  
            const char*, const char*, const char*, const char*,  
            int, int, const char* 
        >(query);  
    }(); 

    uint32_t id = _pmc_id++;    
    _add_pmc_stmt(id, pmc.target_arch.c_str(), pmc.agent_id,   
        pmc.name.c_str(), pmc.symbol.c_str(), pmc.event_code, pmc.instance_id,  
        pmc.description.c_str(), pmc.long_description.c_str(), pmc.component.c_str(),  
        pmc.units.c_str(), pmc.value_type.c_str(), pmc.block.c_str(), pmc.expression.c_str(),  
        pmc.is_constant, pmc.is_derived, pmc.extdata.c_str());

    _pmc_name_map[pmc.name] = id;  
    return id;  
}  

void data_processor::add_pmc_event(uint32_t pmc_id, double value, uint32_t event_id) {
    static auto _add_pmc_event_stmt = []() {
        data_storage::queries::table_insert_query query_builder;
        auto query = query_builder.set_table_name("rocpd_pmc_event")
                                .set_columns(
                                    "id", "event_id", "pmc_id", "value")
                                .set_values('?', '?', '?', '?')
                                .get_query_string();
        
        return data_storage::database::get_instance().create_statment_executor<
            uint32_t,      // id
            uint32_t,      // event_id
            uint32_t,      // pmc_id
            double         // value
        >(query);
    }();

    static uint32_t _pmc_event_id = 1;
    _add_pmc_event_stmt(_pmc_event_id++, event_id, pmc_id, value);
}

uint32_t data_processor::find_pmc_id(const std::string& name) {
    auto it = _pmc_name_map.find(name);
    if (it != _pmc_name_map.end()) {
        return it->second;
    }
    return 0; 
}


uint32_t data_processor::find_string_id(const std::string& str) {  
    auto it = _strings_map.find(str);
    if (it != _strings_map.end()) {
        return it->second;
    }
    return create_string(str); 
}


uint32_t data_processor::find_gpu_agent_id(const int& type_id) {  
    auto it = _gpu_agents.find(type_id);
    if (it != _gpu_agents.end()) {
        return it->second;
    }
    return 0;
}

} // namespace rocprofsys
