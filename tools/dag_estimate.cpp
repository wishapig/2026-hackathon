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
std::vector<std::string> parse_policies(const std::string &s) {
    std::vector<std::string> out; std::stringstream ss(s); std::string x;
    while (std::getline(ss,x,',')) if (!x.empty()) out.push_back(x);
    if (out.empty()) throw std::runtime_error("policy list is empty");
    return out;
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
double median_value(const std::vector<double> &v) {
    if (v.empty()) throw std::runtime_error("calibration has no samples");
    auto x=v; std::sort(x.begin(),x.end());
    return x.size()%2 ? x[x.size()/2] : (x[x.size()/2-1]+x[x.size()/2])/2;
}
void write_calibration(const std::string &path, const std::vector<std::pair<std::string, const std::vector<double>*>> &items) {
    std::ofstream out(path); if (!out) throw std::runtime_error("cannot write calibration");
    out << "dag-estimate-calibration-v1\n";
    out << std::setprecision(17);
    for (const auto &[name, values] : items) { out << name << ' ' << values->size(); for (double x : *values) out << ' ' << x; out << '\n'; }
}
Timing read_timing(std::istringstream &line, const std::string &expected) {
    std::string name; std::size_t count=0; line >> name >> count;
    if (name!=expected || !count) throw std::runtime_error("invalid calibration entry");
    Timing t; t.samples.resize(count); for (double &x:t.samples) if (!(line>>x) || !std::isfinite(x) || x<0) throw std::runtime_error("invalid calibration sample"); return t;
}
void read_calibration(const std::string &path, Timing &tc, Timing &tt, Timing &tm, Timing &base) {
    std::ifstream in(path); if (!in) throw std::runtime_error("cannot read calibration");
    std::string header; std::getline(in,header); if (header!="dag-estimate-calibration-v1") throw std::runtime_error("invalid calibration header");
    std::string line;
    std::vector<Timing*> dst{&tc,&tt,&tm,&base}; std::vector<std::string> names{"cholesky","trsm","madd","baseline"};
    for (std::size_t i=0;i<dst.size();++i) { if (!std::getline(in,line)) throw std::runtime_error("missing calibration entry"); std::istringstream ss(line); *dst[i]=read_timing(ss,names[i]); }
}
struct Sim { double elapsed=0, work=0, critical=0, util=0; std::size_t steals=0, continuations=0; std::vector<double> busy; };
Sim simulate(const dag::Graph &g, const std::vector<double>& d, unsigned workers, const std::string &policy, double overhead) {
    const std::size_t n=g.tasks.size(); Sim out; out.busy.assign(workers,0.0);
    std::vector<std::uint32_t> indeg=g.indegrees; std::vector<std::deque<std::uint32_t>> q(workers);
    for (std::uint32_t i=0;i<n;++i) if (!indeg[i]) q[i%workers].push_back(i);
    std::vector<double> cp(n,0); for (std::size_t ii=n;ii-->0;) { double best=0; for (auto e=g.offsets[ii];e<g.offsets[ii+1];++e) best=std::max(best,cp[g.successors[e]]); cp[ii]=d[ii]+overhead+best; }
    for (std::size_t i=0;i<n;++i) if (!g.indegrees[i]) out.critical=std::max(out.critical,cp[i]); for (double x:d) out.work+=x+overhead;
    struct E { double end; unsigned w; std::uint32_t id; bool operator>(const E&o)const{return std::tie(end,w,id)>std::tie(o.end,o.w,o.id);} }; std::priority_queue<E,std::vector<E>,std::greater<E>> running; double now=0;
    std::vector<bool> busy(workers,false); std::vector<std::uint32_t> continuation(workers,std::numeric_limits<std::uint32_t>::max()); std::vector<unsigned> round_robin(workers); for(unsigned w=0;w<workers;++w) round_robin[w]=w;
    auto pop=[&](unsigned w)->std::tuple<std::uint32_t,bool,bool>{
        const bool oldest=(round_robin[w]++%32)==0;
        if(policy=="current"&&continuation[w]!=std::numeric_limits<std::uint32_t>::max()&&!oldest){auto id=continuation[w];continuation[w]=std::numeric_limits<std::uint32_t>::max();return{id,false,true};}
        if(continuation[w]!=std::numeric_limits<std::uint32_t>::max()){q[w].push_back(continuation[w]);continuation[w]=std::numeric_limits<std::uint32_t>::max();}
        if(!q[w].empty()){auto id=(policy=="fifo"||oldest)?q[w].front():q[w].back();if(policy=="fifo"||oldest)q[w].pop_front();else q[w].pop_back();return{id,false,false};}
        for(unsigned k=1;k<workers;++k){unsigned v=(w+k+(round_robin[w]%workers))%workers;if(v!=w&&!q[v].empty()){auto id=q[v].front();q[v].pop_front();return{id,true,false};}}
        for(unsigned v=0;v<workers;++v)if(v!=w&&!q[v].empty()){auto id=q[v].front();q[v].pop_front();return{id,true,false};}
        return{std::numeric_limits<std::uint32_t>::max(),false,false};
    };
    auto dispatch=[&](unsigned w){if(busy[w])return false;auto [id,st,direct]=pop(w);if(id==std::numeric_limits<std::uint32_t>::max())return false;if(st)++out.steals;if(direct)++out.continuations;busy[w]=true;running.push({now+d[id]+overhead,w,id});return true;};
    auto fill_idle=[&](unsigned first){for(unsigned k=0;k<workers;++k)dispatch((first+k)%workers);}; fill_idle(0);
    while(!running.empty()){auto e=running.top();running.pop();now=e.end;out.busy[e.w]+=d[e.id]+overhead;busy[e.w]=false;
        for(auto k=g.offsets[e.id];k<g.offsets[e.id+1];++k){auto v=g.successors[k];if(--indeg[v]==0){if(policy=="current"){if(continuation[e.w]!=std::numeric_limits<std::uint32_t>::max())q[e.w].push_back(continuation[e.w]);continuation[e.w]=v;}else q[v%workers].push_back(v);}}
        dispatch(e.w);fill_idle((e.w+1)%workers);
    }
    out.elapsed=now; out.util=workers&&now>0?out.work/(now*workers):0; return out;
}
void stats(const std::vector<double>& v, std::ostream &o) { auto x=v; std::sort(x.begin(),x.end()); double med=x[x.size()/2]; if(x.size()%2==0) med=(x[x.size()/2-1]+x[x.size()/2])/2; o<<"{\"samples\":[";for(size_t i=0;i<v.size();++i){if(i)o<<',';o<<std::setprecision(12)<<v[i];}o<<"],\"median\":"<<med<<",\"min\":"<<x.front()<<",\"max\":"<<x.back()<<"}"; }
}
int main(int argc,char**argv){try{
    auto meta=arg(argc,argv,"--metadata"); int n=std::stoi(arg(argc,argv,"--n")), b=std::stoi(arg(argc,argv,"--b")); int samples=std::stoi(arg(argc,argv,"--samples","5")), warmups=std::stoi(arg(argc,argv,"--warmups","0")); auto workers=parse_workers(arg(argc,argv,"--workers","1,2,4,8")); auto policies=parse_policies(arg(argc,argv,"--policies",arg(argc,argv,"--policy","current"))); double overhead=std::stod(arg(argc,argv,"--task-overhead-ns","0"))*1e-9; if(meta.empty()||n<=0||b<=0||n%b||samples<1||warmups<0) throw std::runtime_error("invalid estimate arguments"); for(const auto &policy:policies)if(policy!="current"&&policy!="fifo")throw std::runtime_error("invalid policy");
    dag::Limits lim; lim.memory_bytes=size_arg(argc,argv,"--graph-memory-mb",1024)*1024*1024; lim.max_tasks=size_arg(argc,argv,"--max-tasks",0); lim.max_steps=size_arg(argc,argv,"--max-steps",0);
    lim.loop_chunk=size_arg(argc,argv,"--loop-chunk",1); lim.loop_parts=size_arg(argc,argv,"--loop-parts",0);
    auto program=dag::load_metadata(meta,lim); const auto instantiate_start=Clock::now(); auto tasks=dag::instantiate(program,{n,b},lim); const auto instantiate_seconds=std::chrono::duration<double>(Clock::now()-instantiate_start).count();
    std::size_t phase_tasks=0; bool copy_excluded=false, zero_upper_excluded=false;
    if(!tasks.empty()&&tasks.front().symbol=="__copy"){tasks.erase(tasks.begin());++phase_tasks;copy_excluded=true;}
    if(!tasks.empty()&&tasks.back().symbol=="__zero_upper"){tasks.pop_back();++phase_tasks;zero_upper_excluded=true;}
    const auto graph_start=Clock::now(); auto graph=dag::build_graph(std::move(tasks),lim); const auto build_graph_seconds=std::chrono::duration<double>(Clock::now()-graph_start).count();
    Timing tc, tt, tm, base;
    const auto calibration_in=arg(argc,argv,"--calibration-in");
    if (!calibration_in.empty()) {
        read_calibration(calibration_in,tc,tt,tm,base);
    } else {
        tc=measure("cholesky",n,b,samples,warmups); tt=measure("trsm",n,b,samples,warmups); tm=measure("madd",n,b,samples,warmups);
        std::vector<double> ba(static_cast<std::size_t>(n)*n, 0.0), bl(ba.size(), 0.0);
        for(int s=0;s<warmups+samples;++s){
            for(int i=0;i<n;++i)for(int j=0;j<n;++j){ba[static_cast<std::size_t>(i)*n+j]=(i==j?4.0:0.01);bl[static_cast<std::size_t>(i)*n+j]=ba[static_cast<std::size_t>(i)*n+j];}
            auto st=Clock::now();
            for(int i=0;i<n;i+=b){cholesky(bl.data()+static_cast<std::size_t>(i)*n+i,bl.data()+static_cast<std::size_t>(i)*n+i,b,n);for(int j=i+b;j<n;j+=b)trsm(bl.data()+static_cast<std::size_t>(j)*n+i,bl.data()+static_cast<std::size_t>(i)*n+i,bl.data()+static_cast<std::size_t>(j)*n+i,b,n);for(int j=i+b;j<n;j+=b)for(int k=j;k<n;k+=b)madd(bl.data()+static_cast<std::size_t>(k)*n+i,bl.data()+static_cast<std::size_t>(j)*n+i,bl.data()+static_cast<std::size_t>(k)*n+j,b,n);}
            double sec=std::chrono::duration<double>(Clock::now()-st).count(); if(s>=warmups)base.samples.push_back(sec);
        }
    }
    const auto calibration_out=arg(argc,argv,"--calibration-out");
    if (!calibration_out.empty()) write_calibration(calibration_out,{{"cholesky",&tc.samples},{"trsm",&tt.samples},{"madd",&tm.samples},{"baseline",&base.samples}});
    std::vector<double> durations; durations.reserve(graph.tasks.size());
    const double chol_seconds=median_value(tc.samples), trsm_seconds=median_value(tt.samples), madd_seconds=median_value(tm.samples);
    for(const auto&t:graph.tasks) durations.push_back((t.symbol=="cholesky"?chol_seconds:t.symbol=="trsm"?trsm_seconds:t.symbol=="madd"?madd_seconds:0.0)*static_cast<double>(t.repeat));
    std::size_t logical_task_count=0, logical_kernel_task_count=0;
    for(const auto&t:graph.tasks){const auto repeat=std::max<std::size_t>(t.repeat,1);logical_task_count+=repeat;if(t.symbol!="__copy"&&t.symbol!="__zero_upper")logical_kernel_task_count+=repeat;}
    std::cout<<"{\"model_only\":true,\"performance_eligible\":false,\"correctness_validated\":false,\"n\":"<<n<<",\"b\":"<<b<<",\"q\":"<<n/b<<",\"loop_chunk\":"<<lim.loop_chunk<<",\"loop_parts\":"<<lim.loop_parts<<",\"phase_task_count\":"<<phase_tasks<<",\"copy_excluded\":"<<(copy_excluded?"true":"false")<<",\"zero_upper_excluded\":"<<(zero_upper_excluded?"true":"false")<<",\"instantiate_seconds\":"<<instantiate_seconds<<",\"build_graph_seconds\":"<<build_graph_seconds<<",\"task_count\":"<<graph.tasks.size()<<",\"logical_task_count\":"<<logical_task_count<<",\"logical_kernel_task_count\":"<<logical_kernel_task_count<<",\"edge_count\":"<<graph.successors.size()<<",\"graph_bytes\":"<<graph.memory_bytes<<",\"operator_timing\":{\"cholesky\":";stats(tc.samples,std::cout);std::cout<<",\"trsm\":";stats(tt.samples,std::cout);std::cout<<",\"madd\":";stats(tm.samples,std::cout);std::cout<<"},\"baseline_timing\":";stats(base.samples,std::cout);std::cout<<",\"results\":[";
    bool first=true; for(const auto &policy:policies) for(size_t pi=0;pi<workers.size();++pi){if(!first)std::cout<<',';first=false;auto s=simulate(graph,durations,workers[pi],policy,overhead);double lb=std::max(s.work/workers[pi],s.critical);std::cout<<"{\"policy\":\""<<policy<<"\",\"workers\":"<<workers[pi]<<",\"estimated_dag_seconds\":"<<s.elapsed<<",\"total_modeled_work\":"<<s.work<<",\"critical_path\":"<<s.critical<<",\"lower_bound\":"<<lb<<",\"modeled_utilization\":"<<s.util<<",\"modeled_steals\":"<<s.steals<<",\"modeled_direct_continuations\":"<<s.continuations<<",\"busy_time\":[";for(size_t i=0;i<s.busy.size();++i){if(i)std::cout<<',';std::cout<<s.busy[i];}std::cout<<"]}";}
    std::cout<<"]}\n";
}catch(const dag::ResourceLimit&e){std::cerr<<"resource_limited: "<<e.what()<<"\n";return 3;}catch(const std::exception&e){std::cerr<<"error: "<<e.what()<<"\n";return 2;}}
