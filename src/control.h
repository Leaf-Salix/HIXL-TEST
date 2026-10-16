// Metadata and completion notifications only; no tested memory contents use this socket.
#pragma once

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace control {
class Socket {
 public:
  explicit Socket(int fd = -1) : fd_(fd) {}
  ~Socket() { if (fd_ >= 0) close(fd_); }
  Socket(const Socket &) = delete;
  Socket &operator=(const Socket &) = delete;
  int Get() const { return fd_; }
  int Release() { int fd = fd_; fd_ = -1; return fd; }
 private:
  int fd_;
};

inline void Io(int fd, void *buffer, size_t bytes, bool write) {
  auto *data = static_cast<char *>(buffer);
  while (bytes != 0) {
    // The executables ignore SIGPIPE; flags=0 also permits host-only tests on macOS.
    ssize_t n = write ? send(fd, data, bytes, 0) : recv(fd, data, bytes, 0);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) throw std::runtime_error(std::string("control I/O failed/peer exited: ") +
                                       (n == 0 ? "EOF" : strerror(errno)));
    data += n;
    bytes -= static_cast<size_t>(n);
  }
}

inline sockaddr_in Address(const std::string &endpoint) {
  size_t colon = endpoint.rfind(':');
  if (colon == std::string::npos) throw std::runtime_error("expected IPv4-or-host:port");
  std::string service = endpoint.substr(colon + 1);
  if (service.empty() || service.find_first_not_of("0123456789") != std::string::npos ||
      std::stoul(service) == 0 || std::stoul(service) > 65535) {
    throw std::runtime_error("invalid TCP port");
  }
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *result = nullptr;
  int rc = getaddrinfo(endpoint.substr(0, colon).c_str(), service.c_str(), &hints, &result);
  if (rc != 0) throw std::runtime_error(std::string("resolve: ") + gai_strerror(rc));
  sockaddr_in addr = *reinterpret_cast<sockaddr_in *>(result->ai_addr);
  freeaddrinfo(result);
  return addr;
}

class Group {
 public:
  Group(int rank, int world, const std::string &endpoint, int timeout_seconds = 60)
      : rank_(rank), world_(world), peers_(world, -1) {
    if (world < 1 || world > 64 || rank < 0 || rank >= world || timeout_seconds < 1) {
      throw std::runtime_error("invalid control group dimensions/timeout");
    }
    try {
      const auto address = Address(endpoint);
      auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
      if (rank == 0) {
        Socket listener(socket(AF_INET, SOCK_STREAM, 0));
        if (listener.Get() < 0) throw std::runtime_error("control socket failed");
        int yes = 1;
        setsockopt(listener.Get(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        if (bind(listener.Get(), reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0 ||
            listen(listener.Get(), world) != 0) throw std::runtime_error("coordinator bind/listen failed");
        for (int count = 1; count < world; ++count) {
          auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
              deadline - std::chrono::steady_clock::now()).count();
          pollfd ready{listener.Get(), POLLIN, 0};
          if (remaining <= 0 || poll(&ready, 1, static_cast<int>(remaining)) <= 0) {
            throw std::runtime_error("coordinator accept timeout");
          }
          Socket peer(accept(listener.Get(), nullptr, nullptr));
          Configure(peer.Get(), timeout_seconds);
          int32_t hello[3]{};
          Io(peer.Get(), hello, sizeof(hello), false);
          if (hello[0] != 0x48585431 || hello[2] != world || hello[1] <= 0 ||
              hello[1] >= world || peers_[hello[1]] >= 0) throw std::runtime_error("bad/duplicate rank handshake");
          peers_[hello[1]] = peer.Release();
        }
      } else {
        while (true) {
          Socket peer(socket(AF_INET, SOCK_STREAM, 0));
          Configure(peer.Get(), timeout_seconds);
          // A nonblocking connect keeps unreachable peers within the startup deadline.
          int flags = fcntl(peer.Get(), F_GETFL, 0);
          if (flags < 0 || fcntl(peer.Get(), F_SETFL, flags | O_NONBLOCK) < 0) {
            throw std::runtime_error("control nonblocking setup failed");
          }
          int rc = connect(peer.Get(), reinterpret_cast<const sockaddr *>(&address), sizeof(address));
          if (rc != 0 && errno == EINPROGRESS) {
            pollfd ready{peer.Get(), POLLOUT, 0};
            if (poll(&ready, 1, 200) > 0) {
              int error = 0;
              socklen_t length = sizeof(error);
              if (getsockopt(peer.Get(), SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0) rc = 0;
            }
          }
          if (rc == 0) {
            if (fcntl(peer.Get(), F_SETFL, flags) < 0) throw std::runtime_error("restore blocking socket failed");
            int32_t hello[3]{0x48585431, rank, world};
            Io(peer.Get(), hello, sizeof(hello), true);
            peers_[0] = peer.Release();
            break;
          }
          if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("coordinator connect timeout");
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
      }
    } catch (...) { Close(); throw; }
  }
  ~Group() { Close(); }
  Group(const Group &) = delete;
  Group &operator=(const Group &) = delete;

  template <typename T> std::vector<T> AllGather(const T &local) {
    static_assert(std::is_trivially_copyable<T>::value, "metadata must be a fixed record");
    std::vector<T> result(world_);
    result[rank_] = local;
    uint64_t header[2]{++sequence_, sizeof(T)};
    if (rank_ == 0) {
      for (int rank = 1; rank < world_; ++rank) {
        uint64_t incoming[2]{};
        Io(peers_[rank], incoming, sizeof(incoming), false);
        if (std::memcmp(incoming, header, sizeof(header)) != 0) throw std::runtime_error("collective sequence mismatch");
        Io(peers_[rank], &result[rank], sizeof(T), false);
      }
      for (int rank = 1; rank < world_; ++rank) Io(peers_[rank], result.data(), sizeof(T) * world_, true);
    } else {
      T copy = local;
      Io(peers_[0], header, sizeof(header), true);
      Io(peers_[0], &copy, sizeof(copy), true);
      Io(peers_[0], result.data(), sizeof(T) * world_, false);
    }
    return result;
  }

  void Barrier(int64_t stage) {
    for (int64_t value : AllGather(stage)) {
      if (value != stage) throw std::runtime_error("barrier stage mismatch");
    }
  }

 private:
  static void Configure(int fd, int timeout_seconds) {
    if (fd < 0) throw std::runtime_error("control socket failed");
    timeval timeout{timeout_seconds, 0};
    int yes = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0 ||
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) != 0) {
      throw std::runtime_error("control socket options failed");
    }
  }
  void Close() { for (int &fd : peers_) { if (fd >= 0) close(fd); fd = -1; } }
  int rank_;
  int world_;
  uint64_t sequence_ = 0;
  std::vector<int> peers_;
};
}  // namespace control
