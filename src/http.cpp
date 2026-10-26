#include "pulse/http.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>
namespace pulse {
namespace {
constexpr size_t max_header=8192,max_body=16384;
struct Socket { int fd; explicit Socket(int n):fd(n){} ~Socket(){if(fd>=0) ::close(fd);} Socket(const Socket&)=delete; };
std::string lower(std::string s) { for(char& c:s) if(c>='A' && c<='Z') c+=32; return s; }
std::string trim(std::string s) { auto a=s.find_first_not_of(" \t"), b=s.find_last_not_of(" \t"); return a==std::string::npos ? "" : s.substr(a,b-a+1); }
void send_all(int fd,const std::string& bytes) {
    size_t sent=0; while(sent<bytes.size()) {
#ifdef MSG_NOSIGNAL
        auto n=::send(fd,bytes.data()+sent,bytes.size()-sent,MSG_NOSIGNAL);
#else
        auto n=::send(fd,bytes.data()+sent,bytes.size()-sent,0);
#endif
        if(n<0 && errno==EINTR) continue;
        if(n<=0) throw Error(503,"socket send failed"); sent+=static_cast<size_t>(n);
    }
}
void timeout(int fd) { timeval t{2,0}; setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&t,sizeof(t)); setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&t,sizeof(t));
#ifdef SO_NOSIGPIPE
    int one=1; setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof(one));
#endif
}
std::string wire(const Response& r) {
    return "HTTP/1.1 "+std::to_string(r.status)+" Response\r\nContent-Type: "+r.content_type+"\r\nContent-Length: "+std::to_string(r.body.size())+"\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\nCache-Control: no-store\r\nContent-Security-Policy: default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'\r\n\r\n"+r.body;
}
Request receive_request(int fd) {
    std::string bytes; size_t end=std::string::npos, length=0; Request req;
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    for(;;) {
        if(end!=std::string::npos && bytes.size()>=end+4+length) { req.body=bytes.substr(end+4,length); return req; }
        if(std::chrono::steady_clock::now()>deadline) throw Error(408,"request deadline exceeded");
        char buffer[4096]; auto n=::recv(fd,buffer,sizeof(buffer),0);
        if(n<0 && errno==EINTR) continue;
        if(n<=0) throw Error(400,"incomplete request"); bytes.append(buffer,static_cast<size_t>(n));
        if(end==std::string::npos) {
            end=bytes.find("\r\n\r\n");
            if(end==std::string::npos && bytes.size()>max_header) throw Error(431,"headers too large");
            if(end!=std::string::npos) { if(end>max_header) throw Error(431,"headers too large"); req=parse_http_head(bytes.substr(0,end),length); }
        }
    }
}
}
Request parse_http_head(const std::string& head,size_t& length) {
    if(head.size()>max_header) throw Error(431,"headers too large");
    if(head.find('\0')!=std::string::npos) throw Error(400,"invalid header");
    auto end=head.find("\r\n"); auto first=head.substr(0,end); std::istringstream line(first); Request req; std::string version,extra;
    if(!(line>>req.method>>req.path>>version) || line>>extra || version!="HTTP/1.1" || req.path.empty() || req.path[0]!='/' || req.path.size()>256) throw Error(400,"invalid request line");
    if(req.method!="GET" && req.method!="POST") throw Error(405,"method unsupported");
    size_t pos=end==std::string::npos ? head.size() : end+2;
    while(pos<head.size()) {
        end=head.find("\r\n",pos); auto h=head.substr(pos,end==std::string::npos ? std::string::npos : end-pos); auto colon=h.find(':');
        if(colon==std::string::npos || colon==0) throw Error(400,"invalid header");
        auto key=lower(h.substr(0,colon)); if(key.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-")!=std::string::npos) throw Error(400,"invalid header name");
        auto value=trim(h.substr(colon+1)); for(unsigned char c:value) if(c<32 || c==127) throw Error(400,"invalid header value");
        if(!req.headers.emplace(key,value).second) throw Error(400,"duplicate header");
        pos=end==std::string::npos ? head.size() : end+2;
    }
    if(!req.headers.contains("host") || req.headers.at("host").empty()) throw Error(400,"Host required");
    if(req.headers.contains("transfer-encoding") || req.headers.contains("expect")) throw Error(400,"unsupported framing");
    length=0;
    if(auto it=req.headers.find("content-length"); it!=req.headers.end()) {
        auto& s=it->second; auto [p,ec]=std::from_chars(s.data(),s.data()+s.size(),length);
        if(ec!=std::errc{} || p!=s.data()+s.size()) throw Error(400,"invalid content length");
    } else if(req.method=="POST") throw Error(411,"Content-Length required");
    if(length>max_body) throw Error(413,"body too large");
    if(req.method=="GET" && length) throw Error(400,"GET body unsupported");
    if(req.method=="POST" && length && (!req.headers.contains("content-type") || req.headers.at("content-type")!="application/json")) throw Error(415,"application/json required");
    return req;
}
void HttpServer::run(const std::function<bool()>& stopping) {
    Socket listener(::socket(AF_INET,SOCK_STREAM,0)); if(listener.fd<0) throw std::runtime_error("socket failed");
    int one=1; setsockopt(listener.fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
    sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_port=htons(config_.port);
    if(inet_pton(AF_INET,config_.bind.c_str(),&addr.sin_addr)!=1 || ::bind(listener.fd,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))<0 || ::listen(listener.fd,256)<0) throw std::runtime_error("bind/listen failed");
    fcntl(listener.fd,F_SETFL,O_NONBLOCK);
    std::mutex mutex; std::condition_variable ready; std::deque<int> pending; bool done=false;
    std::vector<std::thread> threads;
    for(int i=0;i<config_.http_threads;++i) threads.emplace_back([&] {
        for(;;) {
            int fd; { std::unique_lock lock(mutex); ready.wait(lock,[&]{return done || !pending.empty();}); if(done && pending.empty()) return; fd=pending.front(); pending.pop_front(); }
            Socket client(fd); timeout(fd); Response response;
            try { response=handler_(receive_request(fd)); }
            catch(const Error& e) { response={e.status,Json({{"error",e.what()}}).dump(),"application/json"}; }
            catch(...) { response={500,"{\"error\":\"internal failure\"}","application/json"}; }
            try { send_all(fd,wire(response)); } catch(...) {}
        }
    });
    while(!stopping()) {
        pollfd pfd{listener.fd,POLLIN,0}; auto n=::poll(&pfd,1,100); if(n<=0) continue;
        int fd=::accept(listener.fd,nullptr,nullptr); if(fd<0) continue;
        // Darwin may inherit O_NONBLOCK from the listener; request readers use
        // bounded blocking IO, including separately arriving header/body bytes.
        fcntl(fd,F_SETFL,fcntl(fd,F_GETFL,0)&~O_NONBLOCK);
        std::lock_guard lock(mutex);
        if(pending.size()>=config_.max_connections) { auto bytes=wire({503,"{\"error\":\"connection capacity reached\"}","application/json"}); fcntl(fd,F_SETFL,O_NONBLOCK); try { send_all(fd,bytes); } catch(...) {} ::close(fd); }
        else { pending.push_back(fd); ready.notify_one(); }
    }
    { std::lock_guard lock(mutex); done=true; for(int fd:pending) ::close(fd); pending.clear(); }
    ready.notify_all(); for(auto& thread:threads) thread.join();
}
Response http_call(const std::string& host,int port,const Request& r) {
    addrinfo hints{},*raw=nullptr; hints.ai_socktype=SOCK_STREAM; hints.ai_family=AF_INET;
    if(getaddrinfo(host.c_str(),std::to_string(port).c_str(),&hints,&raw)!=0) throw Error(503,"host resolution failed");
    std::unique_ptr<addrinfo,decltype(&freeaddrinfo)> info(raw,freeaddrinfo); Socket socket(::socket(raw->ai_family,raw->ai_socktype,0)); timeout(socket.fd);
    if(::connect(socket.fd,raw->ai_addr,raw->ai_addrlen)<0) throw Error(503,"connection failed");
    std::string bytes=r.method+" "+r.path+" HTTP/1.1\r\nHost: "+host+"\r\nContent-Type: application/json\r\nContent-Length: "+std::to_string(r.body.size())+"\r\nConnection: close\r\n";
    for(auto& [key,value]:r.headers) bytes+=key+": "+value+"\r\n";
    send_all(socket.fd,bytes+"\r\n"+r.body); bytes.clear(); char buffer[4096];
    for(;;) { auto n=::recv(socket.fd,buffer,sizeof(buffer),0); if(n<0 && errno==EINTR) continue; if(n<0) throw Error(503,"response timeout"); if(!n) break; bytes.append(buffer,static_cast<size_t>(n)); if(bytes.size()>1024*1024) throw Error(503,"response too large"); }
    auto end=bytes.find("\r\n\r\n"); if(end==std::string::npos) throw Error(503,"invalid response");
    std::istringstream status(bytes); std::string version; Response out; if(!(status>>version>>out.status)) throw Error(503,"invalid response status"); out.body=bytes.substr(end+4); return out;
}
}
