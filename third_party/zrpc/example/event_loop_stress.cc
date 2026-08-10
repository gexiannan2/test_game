#include <iostream>
#include <thread>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <string>
#include <cassert>
#include <atomic>
#include <vector>
#include <chrono>
#include <random>
#include <algorithm>
#include <numeric>
#include <unordered_map>

#include "zrpc/net/event_loop.h"
#include "zrpc/base/group.h"
#include <fstream>

using namespace zrpc;
using namespace std::chrono;


// Simple stress test for actor RPC across EventLoop threads.
// Usage: event_loop_stress [nodes] [reqs_per_node] [immediate_pct] [delayed_pct] [noreply_pct] [max_delay_ms]
// Defaults: nodes=4, reqs_per_node=1000, immediate=50, delayed=30, noreply=20, max_delay_ms=10

std::mutex demo_mutex;
std::condition_variable demo_cond;
std::atomic<int> ready_count{0};

class Node {
 public:
  void StartLoop() {
    EventLoop loop_;
    {
      std::unique_lock<std::mutex> lk(demo_mutex);
      loop = &loop_;
      ++ready_count;
      demo_cond.notify_all();
    }

    loop_.RunAfter(0.1, false, std::bind(&Node::Test, this));
    loop_.Run();
    metrics = loop_.GetMetrics();
  }

  void Test() { /* noop for stress */ }

  void DoRpcImmediate(Context& ctx) {
    auto rid_opt = ctx.Get<int64_t>("rid");
    std::string reply = "rid:" + std::to_string(rid_opt ? *rid_opt : -1) + ":immediate";
    if (loop) {
      loop->OnRpcReply(ctx, 0, &reply);
    }
  }

  void DoRpcDelayed(Context& ctx, int max_delay_ms) {
    auto rid_opt = ctx.Get<int64_t>("rid");
    int64_t rid = rid_opt ? *rid_opt : -1;
    if (loop) {
      // simulate async work on the node loop
      std::string pref = "rid:" + std::to_string(rid) + ":delayed";
      thread_local std::mt19937_64 rng_local((unsigned)std::hash<std::thread::id>()(std::this_thread::get_id()));
      std::uniform_int_distribution<int> d(0, max_delay_ms);
      int delay = d(rng_local);
      loop->RunAfter(delay / 1000.0, false, [this, ctx, pref]() mutable {
        std::string r = pref; // copy
        if (loop) {
          loop->OnRpcReply(ctx, 0, &r);
        }
      });
    }
  }

  void DoRpcNoReply(Context& /*ctx*/) {
    // intentionally do nothing
  }

  EventLoop* loop{nullptr};
  EventLoop::Metrics metrics;
};

int main(int argc, char* argv[]) {
  int nodes = 4;
  int reqs_per_node = 1000;
  int immediate_pct = 50;
  int delayed_pct = 30;
  int noreply_pct = 20;
  int max_delay_ms = 10;
  bool use_mw = false;
  std::string csv_out_path;

  if (argc > 1) nodes = std::stoi(argv[1]);
  if (argc > 2) reqs_per_node = std::stoi(argv[2]);
  if (argc > 3) immediate_pct = std::stoi(argv[3]);
  if (argc > 4) delayed_pct = std::stoi(argv[4]);
  if (argc > 5) noreply_pct = std::stoi(argv[5]);
  if (argc > 6) max_delay_ms = std::stoi(argv[6]);
  if (argc > 7) use_mw = std::stoi(argv[7]) != 0;
  if (argc > 8) csv_out_path = argv[8];

  if (immediate_pct + delayed_pct + noreply_pct != 100) {
    std::cerr << "percentages must sum to 100" << std::endl;
    return 2;
  }

  int total_requests = nodes * reqs_per_node;
  std::cout << "Stress test: nodes=" << nodes << " reqs_per_node=" << reqs_per_node
            << " total=" << total_requests << " (immediate=" << immediate_pct
            << "% delayed=" << delayed_pct << "% noreply=" << noreply_pct << "%)"
            << std::endl;

  std::vector<std::unique_ptr<Node>> node_vec;
  std::vector<std::unique_ptr<std::thread>> threads;
  node_vec.resize(nodes);
  for (int i = 0; i < nodes; ++i) node_vec[i].reset(new Node());

  for (int i = 0; i < nodes; ++i) {
    threads.emplace_back(std::unique_ptr<std::thread>(new std::thread(
        std::bind(&Node::StartLoop, node_vec[i].get()))));
  }

  // wait for all node loops ready
  {
    std::unique_lock<std::mutex> lk(demo_mutex);
    demo_cond.wait(lk, [&]() { return ready_count.load() >= nodes; });
  }

  EventLoop main_loop;
  EventLoop* main_lp = &main_loop;

  // Inject Engines to avoid nullptr calls. Keep references so we can register
  // optional middleware chains per node when requested.
  main_lp->SetEngine(std::make_shared<Engine>());
  std::vector<std::shared_ptr<Engine>> node_engines(nodes);
  for (int i = 0; i < nodes; ++i) {
    node_engines[i] = std::make_shared<Engine>();
    node_vec[i]->loop->SetEngine(node_engines[i]);
  }

  // optional middleware counters/registration
  std::vector<std::shared_ptr<std::atomic<int>>> mw_counters;
  if (use_mw) {
    mw_counters.resize(nodes);
    for (int i = 0; i < nodes; ++i) {
      mw_counters[i].reset(new std::atomic<int>(0));
      auto counter = mw_counters[i];
      HandleAction log_mw = [counter](Context& ctx) {
        counter->fetch_add(1, std::memory_order_relaxed);
        ctx.Next();
      };
      HandleAction mark_mw = [](Context& ctx) {
        ctx.Set("via_mw", (int64_t)1);
        ctx.Next();
      };
      std::vector<HandleAction> handlers;
      handlers.push_back(log_mw);
      handlers.push_back(mark_mw);
      handlers.emplace_back(); // placeholder for actual handler
      node_engines[i]->RegisterInternal(1, std::move(handlers));
    }
  }

  std::atomic<int> pending{total_requests};
  std::atomic<int> successes{0};
  std::atomic<int> timeouts{0};
  std::mutex lat_mtx;
  std::vector<long long> latencies; latencies.reserve(total_requests);

  // per-node and per-behavior stats
  std::vector<std::vector<long long>> lat_per_node(nodes);
  std::vector<int> successes_per_node(nodes, 0);
  std::vector<int> timeouts_per_node(nodes, 0);
  std::vector<int> successes_by_behavior(3, 0);
  std::vector<int> timeouts_by_behavior(3, 0);
  struct FailEntry { int rid; int node; int behavior; int code; std::string reply; int64_t lat; };
  std::vector<FailEntry> failed_entries;
  const size_t max_failed_records = 1000;

  // store send time per rid in vector
  std::vector<int64_t> send_time(total_requests + 1, 0);

  // optional CSV output (written on main loop thread from resp_cb)
  std::shared_ptr<std::ofstream> csv_out;
  if (!csv_out_path.empty()) {
    csv_out.reset(new std::ofstream(csv_out_path, std::ofstream::out));
    if (csv_out->is_open()) {
      (*csv_out) << "rid,node,behavior,code,lat_us,reply\n";
    } else {
      csv_out.reset();
    }
  }

  std::mt19937_64 rng((unsigned)time(nullptr));
  std::uniform_int_distribution<int> dist(1, 100);

  // Send requests shortly after main loop starts
  main_lp->RunAfter(0.05, false, [&, nodes, reqs_per_node]() mutable {
    int rid_counter = 0;
    for (int ni = 0; ni < nodes; ++ni) {
      for (int j = 0; j < reqs_per_node; ++j) {
        ++rid_counter;
        int rid = rid_counter;
        Values vals;
        vals["rid"] = (int64_t)rid;
        vals["node"] = (int64_t)ni;

        int r = dist(rng);
        int behavior = 0; // 0 immediate, 1 delayed, 2 noreply
        if (r <= immediate_pct) behavior = 0;
        else if (r <= immediate_pct + delayed_pct) behavior = 1;
        else behavior = 2;

        // record send time
        send_time[rid] = static_cast<int64_t>(zrpc::NowMicros());

        // prepare req_cb
        RpcReqFunctor req_cb;
        if (behavior == 0) {
          req_cb = std::bind(&Node::DoRpcImmediate, node_vec[ni].get(), std::placeholders::_1);
        } else if (behavior == 1) {
          // wrap delayed with max_delay_ms
          req_cb = [ni, &node_vec, max_delay_ms](Context& ctx) {
            node_vec[ni]->DoRpcDelayed(ctx, max_delay_ms);
          };
        } else {
          req_cb = std::bind(&Node::DoRpcNoReply, node_vec[ni].get(), std::placeholders::_1);
        }

        // response callback runs on main loop (we pass main_lp as caller loop)
        int node_id = ni;
        int beh = behavior;
        RpcRespFunctor resp_cb = [rid, node_id, beh, &send_time, &latencies, &lat_mtx, &successes, &timeouts, &pending,
            &successes_per_node, &timeouts_per_node, &successes_by_behavior, &timeouts_by_behavior,
            &failed_entries, &lat_per_node, main_lp, &csv_out]
            (int code, std::string* reply) {
          // capture csv_out by reference from outer scope
          std::shared_ptr<std::ofstream> csv_local = csv_out;
          int64_t now = static_cast<int64_t>(zrpc::NowMicros());
          int64_t sent = send_time[rid];
          if (sent == 0) sent = now;
          long long lat = now - sent;
          if (code == 0 && reply) {
            {
              std::lock_guard<std::mutex> g(lat_mtx);
              latencies.push_back(lat);
              lat_per_node[node_id].push_back(lat);
            }
            ++successes;
            ++successes_per_node[node_id];
            ++successes_by_behavior[beh];
          } else {
            ++timeouts;
            ++timeouts_per_node[node_id];
            ++timeouts_by_behavior[beh];
            if (failed_entries.size() < max_failed_records) {
              FailEntry fe{rid, node_id, beh, code, reply ? *reply : std::string(), lat};
              failed_entries.push_back(std::move(fe));
            }
          }

          int left = --pending;
          if (left == 0) {
            // quit main loop and request nodes to quit
            main_lp->RunInLoop(std::bind(&EventLoop::Quit, main_lp));
          }
          // write csv (on main loop thread)
          if (csv_local) {
            (*csv_local) << rid << "," << node_id << "," << beh << "," << code << "," << lat << ",";
            if (reply) {
              std::string r = *reply;
              // replace newlines
              std::replace(r.begin(), r.end(), '\n', ' ');
              (*csv_local) << '"' << r << '"';
            }
            (*csv_local) << '\n';
          }
        };

        // send with 5s expire by default
        node_vec[ni]->loop->RunRpcInLoop(5000000 /*5s*/, 1 /*pid*/, vals, std::move(req_cb), std::move(resp_cb), main_lp);
      }
    }
  });

  auto start_time = static_cast<int64_t>(zrpc::NowMicros());
  main_lp->Run();
  auto end_time = static_cast<int64_t>(zrpc::NowMicros());

  // stop node loops
  for (int i = 0; i < nodes; ++i) {
    if (node_vec[i]->loop) node_vec[i]->loop->RunInLoop(std::bind(&EventLoop::Quit, node_vec[i]->loop));
  }

  for (auto &t : threads) if (t && t->joinable()) t->join();

  // compute stats
  int succ = successes.load();
  int to = timeouts.load();
  int total_done = succ + to;
  std::sort(latencies.begin(), latencies.end());
  double avg = 0;
  if (!latencies.empty()) avg = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();
  long long p50 = latencies.empty() ? 0 : latencies[latencies.size() / 2];
  long long p95 = latencies.empty() ? 0 : latencies[std::min((size_t)((latencies.size()*95)/100), latencies.size()-1)];
  long long mx = latencies.empty() ? 0 : latencies.back();

  std::cout << "Stress finished. total_sent=" << total_requests
            << " total_done=" << total_done << " successes=" << succ << " timeouts=" << to << "\n";
  std::cout << "Elapsed ms=" << (end_time - start_time) / 1000.0 << " avg_us=" << avg
            << " p50_us=" << p50 << " p95_us=" << p95 << " max_us=" << mx << std::endl;

  // print per-node and per-behavior stats
  std::cout << "Per-node stats:\n";
  for (int i = 0; i < nodes; ++i) {
    long long pavg = 0;
    if (!lat_per_node[i].empty()) pavg = std::accumulate(lat_per_node[i].begin(), lat_per_node[i].end(), 0LL) / (long long)lat_per_node[i].size();
    std::cout << " node=" << i << " succ=" << successes_per_node[i] << " to=" << timeouts_per_node[i]
              << " avg_us=" << pavg << " samples=" << lat_per_node[i].size() << "\n";
  }

  std::cout << "Per-behavior stats (0=immediate,1=delayed,2=noreply):\n";
  for (int b = 0; b < 3; ++b) {
    std::cout << " behavior=" << b << " succ=" << successes_by_behavior[b] << " to=" << timeouts_by_behavior[b] << "\n";
  }

  std::cout << "Recorded failed entries (up to " << max_failed_records << "): " << failed_entries.size() << "\n";
  for (size_t i = 0; i < failed_entries.size(); ++i) {
    auto &fe = failed_entries[i];
    std::cout << "  rid=" << fe.rid << " node=" << fe.node << " beh=" << fe.behavior << " code=" << fe.code << " lat_us=" << fe.lat << " reply='" << fe.reply << "'\n";
  }

  // print EventLoop metrics per node
  std::cout << "EventLoop metrics (per-loop):\n";
  auto main_m = main_lp->GetMetrics();
  std::cout << " main: wakeups=" << main_m.wakeup_count << " run_inloop=" << main_m.run_inloop_count << " queue_in=" << main_m.queue_inloop_count << " queue_rpc=" << main_m.queue_rpc_inloop_count << " max_pending=" << main_m.max_pending_functors << " max_rpc_pending=" << main_m.max_rpc_pending << " max_rpc_callbacks=" << main_m.max_rpc_callbacks << " max_rpc_heap=" << main_m.max_rpc_heap << "\n";
  for (int i = 0; i < nodes; ++i) {
    const auto &m = node_vec[i]->metrics;
    std::cout << " node=" << i << " wakeups=" << m.wakeup_count << " run_inloop=" << m.run_inloop_count << " queue_in=" << m.queue_inloop_count << " queue_rpc=" << m.queue_rpc_inloop_count << " max_pending=" << m.max_pending_functors << " max_rpc_pending=" << m.max_rpc_pending << " max_rpc_callbacks=" << m.max_rpc_callbacks << " max_rpc_heap=" << m.max_rpc_heap << "\n";
  }

  if (csv_out && csv_out->is_open()) csv_out->close();
  if (use_mw) {
    std::cout << "Middleware counts per node:\n";
    for (int i = 0; i < nodes; ++i) {
      int cnt = 0;
      if (i < (int)mw_counters.size() && mw_counters[i]) cnt = mw_counters[i]->load(std::memory_order_relaxed);
      std::cout << " node=" << i << " mw_calls=" << cnt << "\n";
    }
  }

  return 0;
}
