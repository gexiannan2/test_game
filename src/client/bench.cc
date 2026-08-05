// bench.cc - EventLoop pipe benchmark (adapted from muduo bench.cc)
// Usage: svc_game_3d_bench [-n pipes] [-a active] [-w writes]

#include "zrpc/base/logger.h"
#include "zrpc/net/channel.h"
#include "zrpc/net/event_loop.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <chrono>
#include <vector>
#include <memory>

using namespace zrpc;

std::vector<int> g_pipes;
int              g_numPipes;
int              g_numActive;
int              g_numWrites;
EventLoop*       g_loop;
std::vector<std::unique_ptr<Channel>> g_channels;

int g_reads, g_writes, g_fired;

void ReadCallback(int fd, int idx) {
  char ch;
  g_reads += static_cast<int>(::recv(fd, &ch, sizeof(ch), 0));
  if (g_writes > 0) {
    int widx = idx + 1;
    if (widx >= g_numPipes) widx -= g_numPipes;
    ::send(g_pipes[2 * widx + 1], "m", 1, 0);
    g_writes--;
    g_fired++;
  }
  if (g_fired == g_reads) {
    g_loop->Quit();
  }
}

using Clock = std::chrono::steady_clock;
using Ms    = std::chrono::milliseconds;

int64_t NowMs() {
  return std::chrono::duration_cast<Ms>(Clock::now().time_since_epoch()).count();
}

std::pair<int, int> RunOnce() {
  int64_t beforeInit = NowMs();
  for (int i = 0; i < g_numPipes; ++i) {
    Channel& channel = *g_channels[i];
    channel.SetReadCallback([fd = channel.Getfd(), i]() { ReadCallback(fd, i); });
    channel.EnableReading();
  }

  int space = g_numPipes / g_numActive;
  space *= 2;
  for (int i = 0; i < g_numActive; ++i) {
    ::send(g_pipes[i * space + 1], "m", 1, 0);
  }

  g_fired  = g_numActive;
  g_reads  = 0;
  g_writes = g_numWrites;
  int64_t beforeLoop = NowMs();
  g_loop->Run();

  int64_t end       = NowMs();
  int iterTime = static_cast<int>(end - beforeInit);
  int loopTime = static_cast<int>(end - beforeLoop);
  return std::make_pair(iterTime, loopTime);
}

int main(int argc, char* argv[]) {
  g_numPipes  = 100;
  g_numActive = 1;
  g_numWrites = 100;

  int c;
  while ((c = getopt(argc, argv, "n:a:w:")) != -1) {
    switch (c) {
      case 'n': g_numPipes  = atoi(optarg); break;
      case 'a': g_numActive = atoi(optarg); break;
      case 'w': g_numWrites = atoi(optarg); break;
      default:  fprintf(stderr, "usage: %s [-n pipes] [-a active] [-w writes]\n", argv[0]); return 1;
    }
  }

  struct rlimit rl;
  rl.rlim_cur = rl.rlim_max = g_numPipes * 2 + 50;
  ::setrlimit(RLIMIT_NOFILE, &rl);

  g_pipes.resize(2 * g_numPipes);
  for (int i = 0; i < g_numPipes; ++i) {
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, &g_pipes[i * 2]) == -1) {
      perror("socketpair");
      return 1;
    }
  }

  EventLoop loop;
  g_loop = &loop;

  for (int i = 0; i < g_numPipes; ++i) {
    g_channels.emplace_back(std::make_unique<Channel>(&loop, g_pipes[i * 2]));
  }

  for (int i = 0; i < 25; ++i) {
    auto t = RunOnce();
    printf("%8d %8d\n", t.first, t.second);
  }

  for (auto& channel : g_channels) {
    channel->DisableAll();
    channel->Remove();
  }
  g_channels.clear();

  return 0;
}
