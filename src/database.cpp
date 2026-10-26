#include "pulse/database.hpp"
namespace pulse {
Json Result::json_rows() const {
    Json rows=Json::array();
    for(int i=0;i<size();++i) {
        Json row=Json::object();
        for(int k=0;k<PQnfields(result.get());++k) {
            auto name=PQfname(result.get(),k);
            if(PQgetisnull(result.get(),i,k)) row[name]=nullptr;
            else if(PQftype(result.get(),k)==20 || PQftype(result.get(),k)==23) row[name]=std::stoll(get(i,k));
            else if(PQftype(result.get(),k)==3802) row[name]=Json::parse(get(i,k));
            else row[name]=get(i,k);
        } rows.push_back(row);
    } return rows;
}
Database::Database(const std::string& dsn) {
    const char* keys[]={"dbname","connect_timeout",nullptr}; const char* values[]={dsn.c_str(),"3",nullptr};
    connection_=PQconnectdbParams(keys,values,1);
    if(!connection_ || PQstatus(connection_)!=CONNECTION_OK) { if(connection_) PQfinish(connection_); throw Error(503,"database unavailable"); }
    try { exec("SET statement_timeout='5s'"); exec("SET lock_timeout='3s'"); exec("SET synchronous_commit=on"); exec("SELECT set_config('application_name','pulsestream:' || current_schema(),false)"); }
    catch(...) { PQfinish(connection_); throw; }
}
Database::~Database() { PQfinish(connection_); }
Database& Database::for_thread(const std::string& dsn) {
    // Each bounded HTTP worker owns one connection. Reconnecting after a broken
    // session avoids connection-per-event churn and ephemeral-port exhaustion.
    thread_local std::unique_ptr<Database> cached;
    thread_local std::string cached_dsn;
    if(!cached || cached_dsn!=dsn || PQstatus(cached->connection_)!=CONNECTION_OK) {
        cached=std::make_unique<Database>(dsn); cached_dsn=dsn;
    }
    return *cached;
}
Result Database::exec(const std::string& sql,const std::vector<std::string>& args) {
    std::vector<const char*> p; for(auto& a:args) p.push_back(a.c_str());
    Result r(args.empty() ? PQexec(connection_,sql.c_str()) : PQexecParams(connection_,sql.c_str(),static_cast<int>(p.size()),nullptr,p.data(),nullptr,nullptr,0));
    auto s=PQresultStatus(r.result.get());
    if(s!=PGRES_COMMAND_OK && s!=PGRES_TUPLES_OK) {
        const char* state=PQresultErrorField(r.result.get(),PG_DIAG_SQLSTATE);
        if(state && std::string(state)=="23505") throw Error(409,"identity or sequence conflict");
        throw Error(503,"database operation failed");
    } return r;
}
void Database::migrate(const std::string& sql) { exec(sql); }
void Database::audit(const std::string& clinic,const std::string& action,const std::string& subject) {
    exec("INSERT INTO audit(clinic_id,action,subject,occurred_ms) VALUES(NULLIF($1,''),$2,$3,$4::bigint)",{clinic,action,subject,std::to_string(now_ms())});
}
void Database::persist(const Json& envelope) {
    const auto& e=envelope.at("event"); auto device=e.at("device_id").get<std::string>(); auto event=e.at("event_id").get<std::string>(); auto clinic=envelope.at("clinic_id").get<std::string>();
    auto type=e.at("type").get<std::string>(); auto receipt=envelope.at("received_ms").dump();
    exec("BEGIN");
    try {
        auto r=exec("INSERT INTO events(device_id,event_id,clinic_id,sequence,timestamp_ms,received_ms,type,payload) VALUES($1,$2,$3,$4::bigint,$5::bigint,$6::bigint,$7,$8::jsonb) ON CONFLICT(device_id,event_id) DO NOTHING RETURNING event_id",{device,event,clinic,e.at("sequence").dump(),e.at("timestamp_ms").dump(),receipt,type,e.dump()});
        if(r.size()) {
            exec("UPDATE devices SET battery_pct=CASE WHEN NULLIF($5,'') IS NOT NULL AND $4::bigint >= COALESCE(battery_seen_ms,0) THEN $5::integer ELSE battery_pct END, battery_seen_ms=CASE WHEN NULLIF($5,'') IS NOT NULL THEN GREATEST(COALESCE(battery_seen_ms,0),$4::bigint) ELSE battery_seen_ms END, last_seen_ms=GREATEST(COALESCE(last_seen_ms,0),$4::bigint), max_sequence=GREATEST(max_sequence,$3::bigint) WHERE device_id=$1 AND clinic_id=$2",{device,clinic,e.at("sequence").dump(),receipt,type=="heartbeat" ? e.at("battery_pct").dump() : ""});
            if(type=="diagnostic_event") exec("INSERT INTO notifications(clinic_id,device_id,event_id,flag,created_ms) VALUES($1,$2,$3,$4,$5::bigint) ON CONFLICT DO NOTHING",{clinic,device,event,e.at("diagnostic_flag").get<std::string>(),receipt});
            if(type=="scheduled_transmission") exec("INSERT INTO transmission_parts(device_id,transmission_id,part_index,event_id,received_ms) VALUES($1,$2,$3::integer,$4,$5::bigint) ON CONFLICT DO NOTHING",{device,e.at("transmission_id").get<std::string>(),e.at("part_index").dump(),event,receipt});
            audit(clinic,"event_persisted",device+"/"+event);
        }
        exec("COMMIT");
    } catch(...) { try { exec("ROLLBACK"); } catch(...) {} throw; }
}
}
