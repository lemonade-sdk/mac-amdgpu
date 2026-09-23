// Exercise the Darwin readiness adapter without creating an HSA runtime.
#include "lse/communication/poller.hpp"
#include "lse/communication/reactor.hpp"
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

struct Ready final : lse::comm::IReady {
  int reads = 0, writes = 0, hangups = 0;
  lse::comm::Poller *removeOnReady = nullptr;
  void on_ready(int fd, lse::comm::Readiness event) override {
    reads += event.readable;
    writes += event.writable;
    hangups += event.hangup;
    if (removeOnReady)
      removeOnReady->remove(fd);
  }
};
int main() {
  using namespace lse::comm;
  auto created = Poller::create();
  assert(created.ok());
  auto poller = created.release();
  assert((fcntl(poller.fd(), F_GETFD) & FD_CLOEXEC) != 0);
  int pair[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  Ready ready;
  assert(poller.add(pair[0], Interest::kRead, &ready).ok());
  assert(!poller.add(pair[0], Interest::kWrite, &ready).ok());
  assert(poller.wait(0).ok() && ready.reads == 0 && ready.writes == 0);
  char byte = 42, received = 0;
  assert(write(pair[1], &byte, 1) == 1);
  assert(poller.wait(100000000).ok() && ready.reads == 1);
  assert(read(pair[0], &received, 1) == 1 && received == byte);
  assert(poller.modify(pair[0], Interest::kWrite).ok());
  assert(poller.wait(100000000).ok() && ready.writes == 1);
  assert(poller.modify(pair[0], Interest::kNone).ok());
  assert(poller.wait(0).ok() && ready.writes == 1);
  assert(poller.modify(pair[0], Interest::kRead).ok());
  close(pair[1]);
  assert(poller.wait(100000000).ok() && ready.hangups == 1);
  poller.remove(pair[0]);
  assert(!poller.modify(pair[0], Interest::kRead).ok());
  const auto callbacks = ready.reads + ready.writes + ready.hangups;
  assert(poller.wait(0).ok());
  assert(ready.reads + ready.writes + ready.hangups == callbacks);
  close(pair[0]);

  // A wake from another thread interrupts a sleeping wait and gets drained.
  std::thread waker([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    poller.wake();
  });
  const auto start = std::chrono::steady_clock::now();
  assert(poller.wait(1000000000).ok());
  waker.join();
  assert(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
  assert(poller.wait(0).ok());

  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  ready.removeOnReady = &poller;
  assert(poller.add(pair[0], Interest::kReadWrite, &ready).ok());
  assert(write(pair[1], &byte, 1) == 1);
  const auto before = ready.reads + ready.writes;
  assert(poller.wait(100000000).ok());
  assert(ready.reads + ready.writes == before + 1);
  close(pair[0]);
  close(pair[1]);
  auto moved = std::move(poller);
  moved.wake();
  assert(moved.wait(0).ok());
  auto serverResult = Reactor::create(), clientResult = Reactor::create();
  assert(serverResult.ok() && clientResult.ok());
  auto server = serverResult.release(), client = clientResult.release();
  auto endpoint = Endpoint::parse("tcp://127.0.0.1:0");
  assert(endpoint.ok());
  auto listener = server.listen(*endpoint);
  if (!listener.ok()) {
    std::fprintf(stderr, "TCP listen: %s\n",
                 listener.status().to_string().c_str());
    return 1;
  }
  auto connection = client.connect(listener->endpoint());
  assert(connection.ok());
  auto channel = connection.release();
  std::uint64_t accepted = 0;
  bool connected = false, receivedData = false;
  std::array<Event, 16> events{};
  auto pump = [&](bool data) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (std::chrono::steady_clock::now() < deadline) {
      auto count = server.poll(events, 100000);
      assert(count.ok());
      for (std::size_t i = 0; i < *count; ++i) {
        if (events[i].kind == EventKind::kAccepted)
          accepted = events[i].channel;
        if (events[i].kind == EventKind::kRecvComplete) {
          assert(events[i].code == lse::StatusCode::kOk);
          receivedData = true;
        }
      }
      count = client.poll(events, 100000);
      assert(count.ok());
      for (std::size_t i = 0; i < *count; ++i)
        if (events[i].kind == EventKind::kConnected)
          connected = true;
      if (data ? receivedData : (connected && accepted))
        return true;
    }
    return false;
  };
  assert(pump(false));
  std::vector<std::byte> payload(1 << 20), landing(payload.size());
  for (std::size_t i = 0; i < payload.size(); ++i)
    payload[i] = static_cast<std::byte>(i * 7 + 11);
  Transfer receive;
  receive.region = host_region(landing.data(), landing.size());
  receive.bytes = landing.size();
  receive.tag = 42;
  auto peer = server.channel(accepted);
  assert(peer.post_recv(receive).ok());
  Transfer send;
  send.region = host_region(payload.data(), payload.size());
  send.bytes = payload.size();
  send.tag = 42;
  assert(channel.post_send(send).ok());
  assert(pump(true));
  assert(payload == landing);
  auto abstractName = Endpoint::parse("unix://@macos-unsupported");
  assert(abstractName.ok());
  auto unsupported = server.listen(*abstractName);
  assert(!unsupported.ok() &&
         unsupported.status().code() == lse::StatusCode::kUnimplemented);
  puts("PASS: Darwin TCP connect/accept and 1 MiB transfer; abstract Unix "
       "declined.");
  puts("PASS: kqueue read/write masks, EOF, removal during callback, move and "
       "cross-thread wake.");
}
