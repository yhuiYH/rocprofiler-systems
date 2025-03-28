#pragma once
#include "queries/table_insert_query.hpp"
#include "utils.hpp"
#include <sqlite3.h>
#include <memory>
#include <functional>

namespace data_storage {

class database {
public:
    static database& get_instance();

    database(database&) = delete;
    database& operator=(database&) = delete;
    
    ~database();

private:
    database();

    template<typename ... Args>
    static inline void validate_sqlite3_result(int sqlite3_error_code, Args&& ... args) {
        if (SQLITE_OK != sqlite3_error_code && SQLITE_DONE != sqlite3_error_code) {
            std::stringstream ss;
            ((ss << args << " "),...);
            ss << " [Sqlite3 error: " << sqlite3_errstr(sqlite3_error_code) << "]";
            throw std::runtime_error(ss.str());
        }
    }

public:  
    
    void initialize_schema();

    void execute_query(const std::string& query);

    /**
     * This function prepares an SQLite statement based on the provided SQL query and returns a lambda   
     * that can execute the prepared statement, binding the provided values to the respective placeholders   
     * in the query.
    */
    template<typename ... Values>
    auto create_statment_executor(const std::string& query) {
        sqlite3_stmt* p_stmt;
        validate_sqlite3_result(sqlite3_prepare_v2(_sqlite3_db, query.c_str(), -1, &p_stmt, nullptr), "Failed to create statement!", query);
        std::unique_ptr<
                sqlite3_stmt, 
                std::function<int(sqlite3_stmt*)>> u_stmt{p_stmt, sqlite3_finalize};

        return [stmt = std::move(u_stmt)](Values ... value) mutable {
            int position = 1;
            auto bind_value = [&](auto value) {
                using T = decltype(value);
                if constexpr (std::is_integral_v<T>){
                    database::validate_sqlite3_result(sqlite3_bind_int(stmt.get(), position, value), "Failed to bind int!");
                } else if constexpr (std::is_floating_point_v<T>) {
                    database::validate_sqlite3_result(sqlite3_bind_double(stmt.get(), position, value), "Failed to bind double!");
                } else if constexpr (utils::is_string_literal_v<T>) {
                    std::cout << "Binding text " << value << std::endl;
                    database::validate_sqlite3_result(sqlite3_bind_text(stmt.get(), position, value, -1, SQLITE_STATIC), "Failed to bind text!");
                    
                } else if constexpr (std::is_same_v<std::decay_t<T>, std::string>) {
                    std::cout << "Binding std::string " << value << std::endl;
                    database::validate_sqlite3_result(sqlite3_bind_text(stmt.get(), position, value.c_str(), -1, SQLITE_TRANSIENT), "Failed to bind text!");
                } else {
                    std::cout << "SSS " << value << std::endl;
                }
                position++;
            };

            (bind_value(value),...);
            validate_sqlite3_result(sqlite3_step(stmt.get()), "Failed to execute step!");
            sqlite3_reset(stmt.get());
        };
    }

private:
    sqlite3* _sqlite3_db;
};

} // namespace data_storage

    