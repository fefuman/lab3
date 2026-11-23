#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <atomic>
#include <stdexcept>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <process.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/wait.h>
#endif

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
static volatile std::sig_atomic_t stop_requested = 0;
static void request_stop(int) { stop_requested = 1; }

#ifdef _WIN32
using Pid = unsigned long;
static Pid self_pid() { return static_cast<Pid>(_getpid()); }
#else
using Pid = long;
static Pid self_pid() { return static_cast<Pid>(getpid()); }
#endif

struct State {
    std::uint64_t magic = 0x4f534c4142434e54ULL;
    std::int64_t counter = 0;
    std::int64_t leader = 0;
    std::int64_t child_a = 0;
    std::int64_t child_b = 0;
};

class LockedState {
#ifdef _WIN32
    HANDLE h_ = INVALID_HANDLE_VALUE;
    OVERLAPPED ov_{};
#else
    int fd_ = -1;
#endif
public:
    explicit LockedState(const fs::path& p) {
#ifdef _WIN32
        h_ = CreateFileW(p.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open state file");
#else
        fd_ = ::open(p.c_str(), O_RDWR | O_CREAT, 0666);
        if (fd_ < 0) throw std::runtime_error("cannot open state file");
#endif
        lock();
        State s{}; read_raw(s);
        if (s.magic != State{}.magic) { s = State{}; write_raw(s); }
    }
    ~LockedState() {
        unlock();
#ifdef _WIN32
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
#else
        if (fd_ >= 0) ::close(fd_);
#endif
    }
    LockedState(const LockedState&) = delete;
    void lock() {
#ifdef _WIN32
        if (!LockFileEx(h_, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &ov_)) throw std::runtime_error("cannot lock state file");
#else
        while (flock(fd_, LOCK_EX) != 0) { if (errno != EINTR) throw std::runtime_error("cannot lock state file"); }
#endif
    }
    void unlock() {
#ifdef _WIN32
        if (h_ != INVALID_HANDLE_VALUE) UnlockFileEx(h_, 0, MAXDWORD, MAXDWORD, &ov_);
#else
        if (fd_ >= 0) flock(fd_, LOCK_UN);
#endif
    }
    State read() {
        State s{}; read_raw(s);
        if (s.magic != State{}.magic) s = State{};
        return s;
    }
    void write(const State& s) { write_raw(s); }
private:
    void read_raw(State& s) {
#ifdef _WIN32
        LARGE_INTEGER z{}; SetFilePointerEx(h_, z, nullptr, FILE_BEGIN); DWORD n=0; ReadFile(h_, &s, sizeof(s), &n, nullptr); if(n != sizeof(s)) s=State{};
#else
        ssize_t n = pread(fd_, &s, sizeof(s), 0); if (n != (ssize_t)sizeof(s)) s=State{};
#endif
    }
    void write_raw(const State& s) {
#ifdef _WIN32
        LARGE_INTEGER z{}; SetFilePointerEx(h_, z, nullptr, FILE_BEGIN); DWORD n=0; if(!WriteFile(h_, &s, sizeof(s), &n, nullptr) || n != sizeof(s) || !SetEndOfFile(h_)) throw std::runtime_error("cannot write state file"); FlushFileBuffers(h_);
#else
        ssize_t n = pwrite(fd_, &s, sizeof(s), 0); if(n != (ssize_t)sizeof(s)) throw std::runtime_error("cannot write state file"); fsync(fd_);
#endif
    }
};

static bool alive(std::int64_t pid) {
    if (pid <= 0) return false;
#ifdef _WIN32
    HANDLE h=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if(!h) return false; DWORD code=0; bool ok=GetExitCodeProcess(h,&code) && code==STILL_ACTIVE; CloseHandle(h); return ok;
#else
    if (kill(static_cast<pid_t>(pid), 0) == 0) return true;
    return errno == EPERM;
#endif
}
static std::string timestamp() {
    using namespace std::chrono;
    auto now=system_clock::now(); auto ms=duration_cast<milliseconds>(now.time_since_epoch())%1000;
    std::time_t t=system_clock::to_time_t(now); std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm,&t);
#else
    localtime_r(&t,&tm);
#endif
    char b[64]; std::snprintf(b,sizeof(b),"%04d-%02d-%02d %02d:%02d:%02d.%03lld",tm.tm_year+1900,tm.tm_mon+1,tm.tm_mday,tm.tm_hour,tm.tm_min,tm.tm_sec,(long long)ms.count()); return b;
}
static void log_line(const fs::path& log, const std::string& line) {
    LockedState guard(fs::path(log.string()+".lock"));
    std::ofstream out(log, std::ios::app); if(!out) throw std::runtime_error("cannot open log file: "+log.string()); out << line << '\n'; out.flush();
}
static std::string pid_text() { return std::to_string(self_pid()); }
struct Options { fs::path state, log; bool child=false; char kind=0; };
static Options parse(int argc,char**argv) {
    Options o;
#ifdef _WIN32
    auto base=fs::temp_directory_path()/"os_counter_lab";
#else
    auto base=fs::path("/tmp/os_counter_lab_"+std::to_string((long long)getuid()));
#endif
    fs::create_directories(base); o.state=base/"state.bin"; o.log=base/"counter.log";
    for(int i=1;i<argc;++i) { std::string a=argv[i]; if(a=="--state" && i+1<argc)o.state=argv[++i]; else if(a=="--log"&&i+1<argc)o.log=argv[++i]; else if(a=="--child-a") {o.child=true;o.kind='a';} else if(a=="--child-b") {o.child=true;o.kind='b';} else if(a=="--help") {std::cout<<"os_counter [--state PATH] [--log PATH]\n"; std::exit(0);} else throw std::runtime_error("unknown argument: "+a); }
    if(o.state.has_parent_path()) fs::create_directories(o.state.parent_path()); if(o.log.has_parent_path()) fs::create_directories(o.log.parent_path()); return o;
}
static void change(const Options&o, const std::function<void(State&)>& fn) { LockedState f(o.state); State s=f.read(); fn(s); f.write(s); }
static void child_run(const Options&o) {
    auto id=self_pid(); log_line(o.log,"START child-"+std::string(1,o.kind)+" pid="+std::to_string(id)+" time="+timestamp());
    change(o,[&](State&s){ if(o.kind=='a')s.counter+=10; else s.counter*=2; });
    if(o.kind=='b') { std::this_thread::sleep_for(std::chrono::seconds(2)); change(o,[](State&s){s.counter/=2;}); }
    log_line(o.log,"EXIT child-"+std::string(1,o.kind)+" pid="+std::to_string(id)+" time="+timestamp());
}
#ifdef _WIN32
static std::int64_t spawn(const fs::path& exe,const Options&o,char k) {
    std::wstring cmd=L"\""+exe.wstring()+L"\" --state \""+o.state.wstring()+L"\" --log \""+o.log.wstring()+L"\" --child-"+(k=='a'?L"a":L"b");
    STARTUPINFOW si{};si.cb=sizeof(si);PROCESS_INFORMATION pi{};
    if(!CreateProcessW(exe.wstring().c_str(),cmd.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi)) throw std::runtime_error("CreateProcess failed");
    auto pid=pi.dwProcessId; CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return pid;
}
#else
static std::int64_t spawn(const fs::path& exe,const Options&o,char k) {
    pid_t p=fork(); if(p<0)throw std::runtime_error("fork failed"); if(p==0) { std::string st=o.state.string(),lg=o.log.string(),mode=k=='a'?"--child-a":"--child-b"; execl(exe.c_str(),exe.c_str(),"--state",st.c_str(),"--log",lg.c_str(),mode.c_str(),(char*)nullptr); _exit(127); } return p;
}
#endif
int main(int argc,char**argv) {
    try {
        Options o=parse(argc,argv); auto exe=fs::absolute(argv[0]); const auto pid=(std::int64_t)self_pid();
        if(o.child) { child_run(o); return 0; }
        log_line(o.log,"START main pid="+std::to_string(pid)+" time="+timestamp());
        bool leader=false;
        change(o,[&](State&s){if(!alive(s.leader)){s.leader=pid;leader=true;}});
        if(leader) log_line(o.log,"LEADER pid="+std::to_string(pid)+" time="+timestamp());
        std::signal(SIGINT, request_stop); std::signal(SIGTERM, request_stop);
        std::thread input([&]{ std::string line; while(std::getline(std::cin,line)) { try { auto value=std::stoll(line); change(o,[&](State&s){s.counter=value;}); log_line(o.log,"SET pid="+std::to_string(pid)+" time="+timestamp()+" counter="+std::to_string(value)); } catch(...) { if(!line.empty()) std::cerr<<"Введите целое 64-битное число\n"; } } });
        input.detach();
        auto next_tick=Clock::now()+std::chrono::milliseconds(300), next_lead=Clock::now()+std::chrono::seconds(1), next_launch=Clock::now()+std::chrono::seconds(3);
        while(!stop_requested) {
#ifndef _WIN32
            int status=0; while(waitpid(-1,&status,WNOHANG)>0) {}
#endif
            auto now=Clock::now(); if(now>=next_tick){change(o,[](State&s){++s.counter;}); next_tick+=std::chrono::milliseconds(300);}
            if(leader && now>=next_lead){ State snap; {LockedState f(o.state); snap=f.read();} log_line(o.log,"TICK pid="+std::to_string(pid)+" time="+timestamp()+" counter="+std::to_string(snap.counter)); next_lead+=std::chrono::seconds(1); }
            if(leader && now>=next_launch) {
                bool aok=false,bok=false;
                change(o,[&](State&s){aok=!alive(s.child_a);bok=!alive(s.child_b); if(aok)s.child_a=0;if(bok)s.child_b=0;});
                if(aok) { auto cp=spawn(exe,o,'a'); change(o,[&](State&s){s.child_a=cp;}); }
                else log_line(o.log,"SKIP child-a still running pid="+std::to_string(pid)+" time="+timestamp());
                if(bok) { auto cp=spawn(exe,o,'b'); change(o,[&](State&s){s.child_b=cp;}); }
                else log_line(o.log,"SKIP child-b still running pid="+std::to_string(pid)+" time="+timestamp());
                next_launch+=std::chrono::seconds(3);
            }
            if(!leader) { change(o,[&](State&s){if(s.leader==0||!alive(s.leader)){s.leader=pid;leader=true; log_line(o.log,"LEADER pid="+std::to_string(pid)+" time="+timestamp());}}); }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        change(o,[&](State&s){if(s.leader==pid)s.leader=0;});
        log_line(o.log,"STOP main pid="+std::to_string(pid)+" time="+timestamp());
    } catch(const std::exception&e) { std::cerr<<"error: "<<e.what()<<'\n'; return 1; }
}
