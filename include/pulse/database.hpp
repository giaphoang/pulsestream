#pragma once
#include "pulse/model.hpp"
#include <libpq-fe.h>
#include <memory>
#include <vector>
namespace pulse {
struct Result {
    std::unique_ptr<PGresult,decltype(&PQclear)> result;
    explicit Result(PGresult* p):result(p,PQclear){}
    int size() const { return PQntuples(result.get()); }
    std::string get(int row,int column) const { return PQgetvalue(result.get(),row,column); }
    Json json_rows() const;
};
class Database {
    PGconn* connection_;
public:
    explicit Database(const std::string& dsn);
    ~Database();
    Database(const Database&)=delete;
    static Database& for_thread(const std::string& dsn);
    Result exec(const std::string& sql, const std::vector<std::string>& args={});
    void migrate(const std::string& sql);
    void persist(const Json& envelope);
    void audit(const std::string& clinic,const std::string& action,const std::string& subject);
};
}
