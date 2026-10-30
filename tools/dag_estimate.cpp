#include "dag/core.hpp"
#include "kernels.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
struct Timing { std::vector<double> samples; };
std::vector<unsigned> parse_workers(const std::string &s) {
    std::vector<unsigned> out; std::stringstream ss(s); std::string x;
    while (std::getline(ss, x, ',')) { auto w = std::stoul(x); if (!w) throw std::runtime_error("workers must be positive"); out.push_back(w); }
    if (out.empty()) throw std::runtime_error("workers is empty"); return out;
}
std::string arg(int argc, char **argv, const std::string &name, const std::string &fallback="") {
    for (int i=1;i<argc;++i) if (argv[i]==name && i+1<argc) return argv[++i];
    return fallback;
}
std::size_t size_arg(int argc,char **argv,const std::string &n,std::size_t d) { auto s=arg(argc,argv,n,""); return s.empty()?d:static_cast<std::size_t>(std::stoull(s)); }
void fill(std::vector<double>& x, double v) { std::fill(x.begin(), x.end(), v); }
Timing measure(const std::string &op, int n, int b, int samples, int warmups) {
    std::vector<double> a(static_cast<std::size_t>(n)*n, 0.0), l(a.size(), 0.0), x(a.size(), 0.0);
    for (int i=0;i<n;++i) { a[static_cast<std::size_t>(i)*n+i]=4.0; if (i+1<n) a[static_cast<std::size_t>(i)*n+i+1]=0.1; }
    Timing t;
    for (int s=0;s<warmups+samples;++s) {
        fill(l, 0.0); fill(x, 0.0);
        for (int i=0;i<n;++i) { l[static_cast<std::size_t>(i)*n+i]=2.0; x[static_cast<std::size_t>(i)*n+i]=1.0; }
        auto start=Clock::now();
        if (op=="cholesky") cholesky(a.data(), l.data(), b, n);
        else if (op=="trsm") trsm(a.data(), l.data(), x.data(), b, n);
        else madd(a.data(), l.data(), x.data(), b, n);
        double sec=std::chrono::duration<double>(Clock::now()-start).count();
        if (s>=warmups) t.samples.push_back(sec);
    }
    return t;
}
struct Sim { double elapsed=0, work=0, critical=0, util=0; std::size_t steals=0, continuations=0; std::vector<double> busy; };
Sim simulate(const dag::Graph &g, const std::vector<double>& d, unsigned workers, const std::string &policy, double overhead) {
    const std::size_t n=g.tasks.size(); Sim out; out.busy.assign(workers,0.0);
    std::vector<std::uint32_t> indeg=g.indegrees; std::vector<std::deque<std::uint32_t>> q(workers);
    for (std::uint32_t i=0;i<n;++i) if (!indeg[i]) q[i%workers].push_back(i);
    std::vector<double> cp(n,0); for (std::size_t ii=n;ii-->0;) { double best=0; for (auto e=g.offsets[ii];e<g.offsets[ii+1];++e) best=std::max(best,cp[g.successors[e]]); cp[ii]=d[ii]+best; }
    out.critical=n?cp[0]:0; for (double x:d) out.work+=x+overhead;
    struct E { double end; unsigned w; std::uint32_t id; bool operator>(const E&o)const{return std::tie(end,w,id)>std::tie(o.end,o.w,o.id);} }; std::priority_queue<E,std::vector<E>,std::greater<E>> running; double now=0;
    auto pop=[&](unsigned w)->std::pair<std::uint32_t,bool>{ if(!q[w].empty()){auto id=policy=="fifo"?q[w].front():q[w].back(); if(policy=="fifo")q[w].pop_front();else q[w].pop_back();return {id,false};} for(unsigned k=0;k<workers;++k){unsigned v=(w+k+1)%workers;if(!q[v].empty()){auto id=q[v].front();q[v].pop_front();return{id,true};}} return {std::numeric_limits<std::uint32_t>::max(),false};};
    auto dispatch=[&](unsigned w){auto [id,st]=pop(w);if(id!=std::numeric_limits<std::uint32_t>::max()){if(st)++out.steals; running.push({now+d[id]+overhead,w,id});}};
    for(unsigned w=0;w<workers;++w) dispatch(w);
    while(!running.empty()){ auto e=running.top(); running.pop(); now=e.end; out.busy[e.w]+=d[e.id]+overhead; std::uint32_t cont=std::numeric_limits<std::uint32_t>::max();
        for(auto k=g.offsets[e.id];k<g.offsets[e.id+1];++k){auto v=g.successors[k]; if(--indeg[v]==0){ if(policy=="current" && cont==std::numeric_limits<std::uint32_t>::max()) cont=v; else q[v%workers].push_back(v); }}
        if(cont!=std::numeric_limits<std::uint32_t>::max()){q[e.w].push_back(cont);++out.continuations;}
        dispatch(e.w); while(running.size()<workers){bool any=false;for(unsigned w=0;w<workers;++w) if(!q[w].empty()){dispatch(w);any=true;break;} if(!any)break;}
    }
    out.elapsed=now; out.util=workers&&now>0?out.work/(now*workers):0; return out;
}
void stats(const std::vector<double>& v, std::ostream &o) { auto x=v; std::sort(x.begin(),x.end()); double med=x[x.size()/2]; if(x.size()%2==0) med=(x[x.size()/2-1]+x[x.size()/2])/2; o<<"{\"samples\":[";for(size_t i=0;i<v.size();++i){if(i)o<<',';o<<std::setprecision(12)<<v[i];}o<<"],\"median\":"<<med<<",\"min\":"<<x.front()<<",\"max\":"<<x.back()<<"}"; }
}
int main(int argc,char**argv){try{
    auto meta=arg(argc,argv,"--metadata"); int n=std::stoi(arg(argc,argv,"--n")), b=std::stoi(arg(argc,argv,"--b")); int samples=std::stoi(arg(argc,argv,"--samples","5")), warmups=std::stoi(arg(argc,argv,"--warmups","0")); auto workers=parse_workers(arg(argc,argv,"--workers","1,2,4,8")); auto policy=arg(argc,argv,"--policy","current"); double overhead=std::stod(arg(argc,argv,"--task-overhead-ns","0"))*1e-9; if(meta.empty()||n<=0||b<=0||n%b||samples<1||warmups<0||(policy!="current"&&policy!="fifo")) throw std::runtime_error("invalid estimate arguments");
    dag::Limits lim; lim.memory_bytes=size_arg(argc,argv,"--graph-memory-mb",1024)*1024*1024; lim.max_tasks=size_arg(argc,argv,"--max-tasks",0); lim.max_steps=size_arg(argc,argv,"--max-steps",0);
    auto program=dag::load_metadata(meta,lim); auto tasks=dag::instantiate(program,{n,b},lim); auto graph=dag::build_graph(std::move(tasks),lim);
    std::vector<double> durations; durations.reserve(graph.tasks.size()); Timing tc=measure("cholesky",n,b,samples,warmups), tt=measure("trsm",n,b,samples,warmups), tm=measure("madd",n,b,samples,warmups); Timing base; std::vector<double> ba(static_cast<std::size_t>(n)*n, 0.0), bl(ba.size(), 0.0); for(int s=0;s<warmups+samples;++s){ for(int i=0;i<n;++i)for(int j=0;j<n;++j){ba[static_cast<std::size_t>(i)*n+j]=(i==j?4.0:0.01); bl[static_cast<std::size_t>(i)*n+j]=ba[static_cast<std::size_t>(i)*n+j];} auto st=Clock::now(); for(int i=0;i<n;i+=b){cholesky(bl.data()+static_cast<std::size_t>(i)*n+i,bl.data()+static_cast<std::size_t>(i)*n+i,b,n);for(int j=i+b;j<n;j+=b)trsm(bl.data()+static_cast<std::size_t>(j)*n+i,bl.data()+static_cast<std::size_t>(i)*n+i,bl.data()+static_cast<std::size_t>(j)*n+i,b,n);for(int j=i+b;j<n;j+=b)for(int k=j;k<n;k+=b)madd(bl.data()+static_cast<std::size_t>(k)*n+i,bl.data()+static_cast<std::size_t>(j)*n+i,bl.data()+static_cast<std::size_t>(k)*n+j,b,n);} double sec=std::chrono::duration<double>(Clock::now()-st).count();if(s>=warmups)base.samples.push_back(sec);} auto median=[](const Timing&t){auto x=t.samples;std::sort(x.begin(),x.end());return x[x.size()/2];}; for(auto&t:graph.tasks) durations.push_back(t.symbol=="cholesky"?median(tc):t.symbol=="trsm"?median(tt):t.symbol=="madd"?median(tm):0.0);
    std::cout<<"{\"model_only\":true,\"performance_eligible\":false,\"correctness_validated\":false,\"n\":"<<n<<",\"b\":"<<b<<",\"q\":"<<n/b<<",\"task_count\":"<<graph.tasks.size()<<",\"edge_count\":"<<graph.successors.size()<<",\"graph_bytes\":"<<graph.memory_bytes<<",\"copy_present\":"<<(std::any_of(graph.tasks.begin(),graph.tasks.end(),[](auto&t){return t.symbol=="__copy";})?"true":"false")<<",\"zero_upper_present\":"<<(std::any_of(graph.tasks.begin(),graph.tasks.end(),[](auto&t){return t.symbol=="__zero_upper";})?"true":"false")<<",\"operator_timing\":{\"cholesky\":";stats(tc.samples,std::cout);std::cout<<",\"trsm\":";stats(tt.samples,std::cout);std::cout<<",\"madd\":";stats(tm.samples,std::cout);std::cout<<"},\"baseline_timing\":";stats(base.samples,std::cout);std::cout<<",\"results\":[";
    for(size_t pi=0;pi<workers.size();++pi){ if(pi)std::cout<<','; auto s=simulate(graph,durations,workers[pi],policy,overhead); double lb=std::max(s.work/workers[pi],s.critical); std::cout<<"{\"policy\":\""<<policy<<"\",\"workers\":"<<workers[pi]<<",\"estimated_dag_seconds\":"<<s.elapsed<<",\"total_modeled_work\":"<<s.work<<",\"critical_path\":"<<s.critical<<",\"lower_bound\":"<<lb<<",\"modeled_utilization\":"<<s.util<<",\"modeled_steals\":"<<s.steals<<",\"modeled_direct_continuations\":"<<s.continuations<<",\"busy_time\":[";for(size_t i=0;i<s.busy.size();++i){if(i)std::cout<<',';std::cout<<s.busy[i];}std::cout<<"]}"; }
    std::cout<<"]}\n";
}catch(const dag::ResourceLimit&e){std::cerr<<"resource_limited: "<<e.what()<<"\n";return 3;}catch(const std::exception&e){std::cerr<<"error: "<<e.what()<<"\n";return 2;}}
