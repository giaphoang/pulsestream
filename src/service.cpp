#include "pulse/service.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
#include <set>
namespace pulse {
namespace {
Response json(int status, const Json& j) { return {status,j.dump(),"application/json"}; }
std::vector<std::string> segments(const std::string& path) {
    std::vector<std::string> parts; std::istringstream stream(path); std::string p;
    while(std::getline(stream,p,'/')) if(!p.empty()) parts.push_back(p);
    return parts;
}
std::string file(const std::string& path) { std::ifstream f(path); if(!f) throw Error(503,"resource unavailable"); return {std::istreambuf_iterator<char>(f),{}}; }
}
Service::Service(Config c):config_(std::move(c)),queue_(config_.wal_directory,config_.queue_items,config_.lease_ms){}
Service::~Service() { stop(); }
void Service::initialize() {
    Database db(config_.database); db.migrate(file(std::string(PULSE_SOURCE_DIR)+"/db/schema.sql"));
    auto clinics=Json::parse(config_.clinics); if(!clinics.is_array() || clinics.empty()) throw std::runtime_error("PULSE_CLINICS must contain clinic credentials");
    std::set<std::string> hashes{digest(config_.operator_token)};
    for(auto& c:clinics) {
        auto id=require_id(c,"clinic_id"); auto token=c.at("token").get<std::string>();
        if(token.size()<16 || !hashes.insert(digest(token)).second) throw std::runtime_error("clinic tokens must be distinct and 16+ characters");
        db.exec("INSERT INTO clinics(clinic_id,token_hash) VALUES($1,$2) ON CONFLICT(clinic_id) DO UPDATE SET token_hash=EXCLUDED.token_hash",{id,digest(token)});
    }
}
void Service::start() { for(int i=0;i<config_.workers;++i) workers_.emplace_back([this]{worker();}); }
void Service::stop() { stopping_=true; for(auto& t:workers_) if(t.joinable()) t.join(); workers_.clear(); }
void Service::worker() {
    std::unique_ptr<Database> db;
    while(!stopping_) {
        std::optional<std::pair<uint64_t,Json>> item;
        try {
            if(queue_.failed()) break;
            if(!db) db=std::make_unique<Database>(config_.database);
            item=queue_.claim();
            if(!item) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
            crash_point(config_,"after_claim"); db->persist(item->second); crash_point(config_,"after_commit");
            queue_.ack(item->first); ++processed_; crash_point(config_,"after_ack");
        } catch(const std::exception&) {
            ++retries_; db.reset();
            if(item && !queue_.failed()) { try { queue_.retry(item->first,1000); } catch(...) {} }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}
std::string Service::bearer(const Request& r) const {
    auto it=r.headers.find("authorization");
    if(it==r.headers.end() || !it->second.starts_with("Bearer ") || it->second.size()>512) throw Error(401,"bearer credential required");
    return it->second.substr(7);
}
std::string Service::clinic(Database& db,const Request& r) const {
    auto token=bearer(r); auto rows=db.exec("SELECT clinic_id FROM clinics WHERE token_hash=$1",{digest(token)});
    if(!rows.size()) throw Error(403,"clinic credential required"); return rows.get(0,0);
}
Response Service::handle(const Request& r) {
    try { return route(r); }
    catch(const Error& e) { ++rejected_; return json(e.status,{{"error",e.what()}}); }
    catch(const Json::exception&) { ++rejected_; return json(422,{{"error","invalid JSON or field type"}}); }
    catch(const std::exception& e) { ++rejected_; std::cerr<<"request failed: "<<e.what()<<'\n'; return json(503,{{"error","service unavailable"}}); }
}
Response Service::route(const Request& r) {
    if(r.method=="GET" && r.path=="/") return {200,file(std::string(PULSE_SOURCE_DIR)+"/dashboard/index.html"),"text/html; charset=utf-8"};
    if(r.method=="GET" && r.path=="/healthz") { auto& db=Database::for_thread(config_.database); db.exec("SELECT 1"); if(queue_.failed()) throw Error(503,"queue requires restart"); return json(200,{{"status","ready"},{"synthetic_only",true}}); }
    if(r.method=="GET" && r.path=="/metrics") {
        if(!secret_equal(bearer(r),config_.operator_token)) throw Error(403,"operator credential required");
        auto q=queue_.stats(); std::ostringstream out;
        out<<"# TYPE pulse_accepted_total counter\npulse_accepted_total "<<accepted_<<"\npulse_duplicate_total "<<duplicates_<<"\npulse_rejected_total "<<rejected_<<"\npulse_processed_total "<<processed_<<"\npulse_retry_total "<<retries_<<"\npulse_queue_pending "<<q.available_items+q.claimed_items<<"\npulse_queue_quarantined "<<q.quarantined_items<<"\npulse_wal_bytes "<<q.stored_bytes<<"\npulse_queue_failed "<<queue_.failed()<<"\n";
        return {200,out.str(),"text/plain; version=0.0.4"};
    }
    if(r.path.find('?')!=std::string::npos || r.path.find('%')!=std::string::npos) throw Error(400,"query strings and encoded paths unsupported");
    auto p=segments(r.path);
    auto& db=Database::for_thread(config_.database);
    if(r.method=="POST" && r.path=="/v1/devices") {
        if(!secret_equal(bearer(r),config_.operator_token)) throw Error(403,"operator credential required");
        auto j=Json::parse(r.body); auto device=require_id(j,"device_id"), c=require_id(j,"clinic_id"); auto token=j.at("token").get<std::string>();
        auto interval=integer(j,"overdue_after_ms",1000,604800000);
        if(token.size()<16 || token.size()>256) throw Error(422,"device token must contain 16 to 256 characters");
        auto schedules=j.value("expected_transmissions",Json::array()); if(!schedules.is_array() || schedules.size()>100) throw Error(422,"invalid schedules");
        for(auto& s:schedules) { require_id(s,"transmission_id"); integer(s,"due_ms",1,INT64_MAX); integer(s,"parts_total",1,1024); }
        if(!db.exec("SELECT 1 FROM clinics WHERE clinic_id=$1",{c}).size()) throw Error(422,"unknown clinic");
        db.exec("BEGIN");
        try {
            db.exec("INSERT INTO devices(device_id,clinic_id,token_hash,registered_ms,overdue_after_ms) VALUES($1,$2,$3,$4::bigint,$5::bigint)",{device,c,digest(token),std::to_string(now_ms()),std::to_string(interval)});
            for(auto& s:schedules) db.exec("INSERT INTO expected_transmissions VALUES($1,$2,$3::bigint,$4::integer)",{device,s.at("transmission_id").get<std::string>(),s.at("due_ms").dump(),s.at("parts_total").dump()});
            db.audit(c,"device_registered",device); db.exec("COMMIT");
        } catch(...) { try{ db.exec("ROLLBACK"); }catch(...){} throw; }
        return json(201,{{"device_id",device},{"clinic_id",c}});
    }
    if(r.method=="POST" && r.path=="/v1/events") {
        auto raw=Json::parse(r.body); auto device=require_id(raw,"device_id");
        auto token=bearer(r); auto rows=db.exec("SELECT clinic_id,token_hash FROM devices WHERE device_id=$1",{device});
        if(!rows.size() || !secret_equal(digest(token),rows.get(0,1))) { db.audit("","ingestion_auth_rejected",device); throw Error(401,"invalid device credential"); }
        auto c=rows.get(0,0); Json event;
        try { event=validate_event(raw,now_ms()); }
        catch(...) { db.audit(c,"event_validation_rejected",device); throw; }
        auto event_id=event.at("event_id").get<std::string>();
        std::lock_guard lock(admission_);
        try {
            if(event.at("type")=="scheduled_transmission") {
                auto t=db.exec("SELECT parts_total FROM expected_transmissions WHERE device_id=$1 AND transmission_id=$2",{device,event.at("transmission_id").get<std::string>()});
                if(!t.size() || std::stoi(t.get(0,0))!=event.at("parts_total").get<int>()) throw Error(422,"unknown transmission or part count mismatch");
                // A part identity is also immutable, including while events are pending.
                auto part=db.exec("SELECT canonical FROM event_identities WHERE device_id=$1 AND canonical::jsonb->>'transmission_id'=$2 AND (canonical::jsonb->>'part_index')::integer=$3::integer",{device,event.at("transmission_id").get<std::string>(),event.at("part_index").dump()});
                if(part.size() && part.get(0,0)!=event.dump()) throw Error(409,"transmission part conflict");
            }
            db.exec("INSERT INTO event_identities(device_id,event_id,canonical,sequence) VALUES($1,$2,$3,$4::bigint) ON CONFLICT(device_id,event_id) DO NOTHING",{device,event_id,event.dump(),event.at("sequence").dump()});
            auto identity=db.exec("SELECT canonical FROM event_identities WHERE device_id=$1 AND event_id=$2",{device,event_id});
            if(identity.get(0,0)!=event.dump()) throw Error(409,"event identity payload conflict");
            if(db.exec("SELECT 1 FROM events WHERE device_id=$1 AND event_id=$2",{device,event_id}).size()) { ++duplicates_; return json(200,{{"status","persisted_duplicate"},{"event_id",event_id}}); }
            crash_point(config_,"before_wal");
            auto id=queue_.enqueue({{"event",event},{"clinic_id",c},{"received_ms",now_ms()}});
            crash_point(config_,"after_wal"); ++accepted_;
            return json(202,{{"status","durably_accepted"},{"event_id",event_id},{"queue_sequence",id},{"durability","fsync_each_record"}});
        } catch(const Error& e) { if(e.status==409 || e.status==422) db.audit(c,"event_conflict_rejected",device+"/"+event_id); throw; }
    }
    auto c=clinic(db,r);
    if(r.method=="GET" && p.size()==4 && p[0]=="v1" && p[1]=="devices" && (p[3]=="status" || p[3]=="events")) {
        if(!valid_id(p[2])) throw Error(400,"invalid device ID");
        auto d=db.exec("SELECT device_id,clinic_id,registered_ms,last_seen_ms,max_sequence,battery_pct,overdue_after_ms FROM devices WHERE device_id=$1 AND clinic_id=$2",{p[2],c});
        if(!d.size()) throw Error(404,"device not found");
        db.audit(c,p[3]=="status" ? "status_reviewed" : "events_reviewed",p[2]);
        if(p[3]=="status") { auto j=d.json_rows()[0]; auto seen=j["last_seen_ms"].is_null() ? j["registered_ms"].get<int64_t>() : j["last_seen_ms"].get<int64_t>(); j["connectivity"]=now_ms()-seen>j["overdue_after_ms"].get<int64_t>() ? "overdue" : j["last_seen_ms"].is_null() ? "awaiting_first_event" : "connected"; return json(200,j); }
        return json(200,{{"events",db.exec("SELECT event_id,sequence,timestamp_ms,received_ms,type,payload FROM events WHERE device_id=$1 AND clinic_id=$2 ORDER BY sequence DESC LIMIT 100",{p[2],c}).json_rows()},{"limit",100}});
    }
    if(r.method=="GET" && r.path=="/v1/transmissions") {
        auto rows=db.exec("SELECT t.device_id,t.transmission_id,t.due_ms,t.parts_total,count(p.part_index)::integer AS received_parts,COALESCE(max(p.received_ms),0)::bigint AS last_received_ms FROM expected_transmissions t JOIN devices d USING(device_id) LEFT JOIN transmission_parts p USING(device_id,transmission_id) WHERE d.clinic_id=$1 GROUP BY t.device_id,t.transmission_id ORDER BY t.due_ms LIMIT 100",{c}).json_rows();
        auto now=now_ms(); for(auto& t:rows) t["state"]=transmission_state(t["received_parts"],t["parts_total"],t["due_ms"],t["last_received_ms"],now);
        db.audit(c,"transmissions_reviewed",c); return json(200,{{"transmissions",rows},{"limit",100}});
    }
    if(r.method=="GET" && r.path=="/v1/notifications") {
        auto rows=db.exec("UPDATE notifications SET delivered_ms=COALESCE(delivered_ms,$2::bigint) WHERE id IN (SELECT id FROM notifications WHERE clinic_id=$1 ORDER BY id DESC LIMIT 100) AND clinic_id=$1 RETURNING id,device_id,event_id,flag,created_ms,delivered_ms,acknowledged_ms",{c,std::to_string(now_ms())}).json_rows();
        db.audit(c,"notifications_delivered",c); return json(200,{{"notifications",rows},{"limit",100}});
    }
    if(r.method=="POST" && p.size()==4 && p[0]=="v1" && p[1]=="notifications" && p[3]=="ack") {
        if(p[2].empty() || p[2].size()>18 || p[2].find_first_not_of("0123456789")!=std::string::npos) throw Error(400,"invalid notification ID");
        auto n=db.exec("SELECT delivered_ms FROM notifications WHERE id=$1::bigint AND clinic_id=$2",{p[2],c});
        if(!n.size()) throw Error(404,"notification not found");
        if(n.get(0,0).empty()) throw Error(409,"notification must be delivered before acknowledgment");
        db.exec("BEGIN");
        try { auto rows=db.exec("UPDATE notifications SET acknowledged_ms=COALESCE(acknowledged_ms,$3::bigint) WHERE id=$1::bigint AND clinic_id=$2 RETURNING id,delivered_ms,acknowledged_ms",{p[2],c,std::to_string(now_ms())}).json_rows(); db.audit(c,"notification_acknowledged",p[2]); db.exec("COMMIT"); return json(200,rows[0]); }
        catch(...) { try{db.exec("ROLLBACK");}catch(...){} throw; }
    }
    if(r.method=="GET" && r.path=="/v1/audit") return json(200,{{"audit",db.exec("SELECT id,action,subject,occurred_ms FROM audit WHERE clinic_id=$1 ORDER BY id DESC LIMIT 100",{c}).json_rows()},{"limit",100}});
    throw Error(404,"route not found");
}
}
