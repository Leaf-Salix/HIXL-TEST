#include "control.h"
#include "pattern.h"

#include <acl/acl.h>
#include <adxl/adxl_engine.h>
#include <hixl/hixl.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr size_t kBytes = 2UL * 1024 * 1024;
constexpr size_t kSourceOffset = 256;
constexpr size_t kTargetOffset = 512;
constexpr int kTimeoutMs = 15000;
int gDevice = 0;
int gWorld = 1;
int gNuma = -1;
bool gRawVmm = true;
bool gDomainHandles = false;
std::string gEngine;
std::string gKernelLibrary;
control::Group *gGroup = nullptr;

void Check(unsigned code, const char *what) {
  if (code != 0) {
    const char *detail = aclGetRecentErrMsg();
    throw std::runtime_error(std::string(what) + " rc=" + std::to_string(code) +
                             " detail=" + (detail ? detail : "none"));
  }
}

void Barrier(int64_t sequence) { gGroup->Barrier(sequence); }

struct Peer {
  uintptr_t address[2]{};
  char engine[64]{};
  int32_t physical = 0;
  char hostname[256]{};
};

class Endpoint {
 public:
  explicit Endpoint(int rank) : rank_(rank) {
    if (gethostname(peer_.hostname, sizeof(peer_.hostname) - 1) != 0) throw std::runtime_error("gethostname failed");
    Check(aclInit(nullptr), "aclInit");
    acl_ready_ = true;
    uint32_t count = 0;
    Check(aclrtGetDeviceCount(&count), "aclrtGetDeviceCount");
    if (count <= static_cast<uint32_t>(gDevice)) throw std::runtime_error("not enough visible devices");
    Check(aclrtSetDevice(gDevice), "aclrtSetDevice");
    device_ready_ = true;
    Check(aclrtGetPhyDevIdByLogicDevId(gDevice, &peer_.physical), "aclrtGetPhyDevIdByLogicDevId");
    std::printf("DEVICE rank=%d logical=%d physical=%d count=%u visible=%s\n", rank, gDevice,
                peer_.physical, count, std::getenv("ASCEND_RT_VISIBLE_DEVICES") ?: "unset");
  }

  void Allocate() {
    for (int kind = 0; kind < 2; ++kind) {
      auto type = kind == 0 ? adxl::MEM_HOST : adxl::MEM_DEVICE;
      std::printf("ALLOC_BEGIN rank=%d kind=%s bytes=%zu\n", rank_, Name(kind), kBytes);
      // ADXL's legacy allocator supports Host memory only; HBM always uses ACL VMM.
      if (gRawVmm || kind == 1) {
        aclrtPhysicalMemProp prop{};
        prop.handleType = ACL_MEM_HANDLE_TYPE_NONE;
        prop.allocationType = ACL_MEM_ALLOCATION_TYPE_PINNED;
        prop.memAttr = kind == 0 ? ACL_MEM_P2P_HUGE : ACL_HBM_MEM_HUGE;
        prop.location.type = kind == 0 ? ACL_MEM_LOCATION_TYPE_HOST_NUMA : ACL_MEM_LOCATION_TYPE_DEVICE;
        prop.location.id = kind == 0 ? (gNuma >= 0 ? gNuma : (peer_.physical / 2) * 2) : gDevice;
        std::printf("RAW_VMM rank=%d kind=%s location=%d id=%d attr=%d\n", rank_, Name(kind),
                    static_cast<int>(prop.location.type), prop.location.id, static_cast<int>(prop.memAttr));
        Check(aclrtMallocPhysical(&raw_handles_[kind], kBytes, &prop, 0), "raw aclrtMallocPhysical");
        Check(aclrtReserveMemAddress(&buffers_[kind], kBytes, 0, nullptr, 1), "raw aclrtReserveMemAddress");
        Check(aclrtMapMem(buffers_[kind], kBytes, 0, raw_handles_[kind], 0), "raw aclrtMapMem");
        raw_mapped_[kind] = true;
        if (kind == 0) {
          int driver_id = gDevice;
          using GetVersion = int32_t (*)(const char *, int32_t *);
          auto version = reinterpret_cast<GetVersion>(dlsym(RTLD_DEFAULT, "aclsysGetVersionNum"));
          int32_t runtime_version = 0;
          if (version) Check(version("runtime", &runtime_version), "runtime version query");
          if (runtime_version < 90200000) {
            Check(aclrtGetLogicDevIdByUserDevId(gDevice, &driver_id), "raw user to driver device id");
          }
          std::printf("ACCESS_DEVICE rank=%d runtime_version=%d user=%d access=%d\n",
                      rank_, runtime_version, gDevice, driver_id);
          aclrtMemAccessDesc access{};
          access.flags = ACL_RT_MEM_ACCESS_FLAGS_READWRITE;
          access.location.type = ACL_MEM_LOCATION_TYPE_DEVICE;
          access.location.id = driver_id;
          Check(aclrtMemSetAccess(buffers_[kind], kBytes, &access, 1), "raw DRAM device access");
        }
      } else {
        Check(adxl::AdxlEngine::MallocMem(type, kBytes, &buffers_[kind]), "AdxlEngine::MallocMem");
      }
      peer_.address[kind] = reinterpret_cast<uintptr_t>(buffers_[kind]);
      std::printf("ALLOC_OK rank=%d kind=%s address=%p\n", rank_, Name(kind), buffers_[kind]);
    }
  }

  void InitializeEngine() {
    if (gEngine.size() >= sizeof(peer_.engine)) throw std::runtime_error("engine endpoint too long");
    std::snprintf(peer_.engine, sizeof(peer_.engine), "%s", gEngine.c_str());
    std::map<hixl::AscendString, hixl::AscendString> options;
    options[hixl::OPTION_ENABLE_USE_FABRIC_MEM] = "1";
    options[hixl::OPTION_BUFFER_POOL] = "0:0";
    std::printf("ENGINE_BEGIN rank=%d engine=%s FabricMem=1 BufferPool=0:0\n", rank_, peer_.engine);
    Check(engine_.Initialize(peer_.engine, options), "Hixl::Initialize");
    engine_ready_ = true;
    for (int kind = 0; kind < 2; ++kind) {
      hixl::MemDesc desc{};
      desc.addr = peer_.address[kind];
      desc.len = kBytes;
      Check(engine_.RegisterMem(desc, kind == 0 ? hixl::MEM_HOST : hixl::MEM_DEVICE, handles_[kind]),
            "Hixl::RegisterMem");
    }
    std::printf("ENGINE_OK rank=%d\n", rank_);
  }

  void Fill(int test) {
    std::vector<uint8_t> data(kBytes);
    for (int kind = 0; kind < 2; ++kind) {
      for (size_t i = 0; i < kBytes; ++i) data[i] = Pattern(test, rank_, kind, i);
      if (kind == 0) {
        // A real CPU store into the Fabric DRAM allocation, not an ACL staging copy.
        std::memcpy(buffers_[kind], data.data(), kBytes);
      } else {
        Check(aclrtMemcpy(buffers_[kind], kBytes, data.data(), kBytes, ACL_MEMCPY_HOST_TO_DEVICE), "fill HBM");
      }
    }
  }

  void Verify(int test, int destination_rank, int destination_kind, int source_rank,
              int source_kind, size_t length) {
    std::vector<uint8_t> staging(kBytes);
    for (int kind = 0; kind < 2; ++kind) {
      const uint8_t *data = static_cast<uint8_t *>(buffers_[kind]);
      if (kind == 1) {
        Check(aclrtMemcpy(staging.data(), kBytes, buffers_[kind], kBytes, ACL_MEMCPY_DEVICE_TO_HOST), "verify HBM");
        data = staging.data();
      }
      for (size_t i = 0; i < kBytes; ++i) {
        uint8_t expected = Pattern(test, rank_, kind, i);
        if (rank_ == destination_rank && kind == destination_kind && i >= kTargetOffset &&
            i < kTargetOffset + length) {
          expected = Pattern(test, source_rank, source_kind, i - kTargetOffset + kSourceOffset);
        }
        if (data[i] != expected) {
          throw std::runtime_error("data mismatch test=" + std::to_string(test) + " rank=" +
                                   std::to_string(rank_) + " kind=" + Name(kind) + " offset=" +
                                   std::to_string(i) + " expected=" + std::to_string(expected) +
                                   " actual=" + std::to_string(data[i]));
        }
      }
    }
  }

  void Transfer(const Peer &remote, int local_kind, int remote_kind, bool read, bool async, size_t length,
                size_t source_offset = kSourceOffset, size_t target_offset = kTargetOffset) {
    hixl::TransferOpDesc desc{};
    desc.local_addr = peer_.address[local_kind] + (read ? target_offset : source_offset);
    desc.remote_addr = remote.address[remote_kind] + (read ? source_offset : target_offset);
    desc.len = length;
    auto op = read ? hixl::READ : hixl::WRITE;
    if (!async) {
      Check(engine_.TransferSync(remote.engine, op, {desc}, kTimeoutMs), "Hixl::TransferSync");
      return;
    }
    hixl::TransferReq req{};
    hixl::TransferArgs args{};
    Check(engine_.TransferAsync(remote.engine, op, {desc}, args, req), "Hixl::TransferAsync");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kTimeoutMs);
    while (true) {
      hixl::TransferStatus status{};
      Check(engine_.GetTransferStatus(req, status), "Hixl::GetTransferStatus");
      if (status == hixl::TransferStatus::COMPLETED) return;
      if (status != hixl::TransferStatus::WAITING) throw std::runtime_error("HIXL async failed or timed out");
      if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("async poll deadline");
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
  }

  void CloseEngine() {
    for (const auto &remote : connected_engines_) {
      Check(engine_.Disconnect(remote.c_str(), kTimeoutMs), "Hixl::Disconnect");
    }
    connected_engines_.clear();
    for (auto &handle : handles_) {
      if (handle) { Check(engine_.DeregisterMem(handle), "Hixl::DeregisterMem"); handle = nullptr; }
    }
    if (engine_ready_) { engine_.Finalize(); engine_ready_ = false; }
  }

  void Close() {
    CloseEngine();
    for (int kind = 0; kind < 2; ++kind) {
      if (gRawVmm || kind == 1) {
        if (raw_mapped_[kind]) { Check(aclrtUnmapMem(buffers_[kind]), "raw unmap"); raw_mapped_[kind] = false; }
        if (buffers_[kind]) { Check(aclrtReleaseMemAddress(buffers_[kind]), "raw release VA"); buffers_[kind] = nullptr; }
        if (raw_handles_[kind]) { Check(aclrtFreePhysical(raw_handles_[kind]), "raw free PA"); raw_handles_[kind] = nullptr; }
      } else if (buffers_[kind]) {
        Check(adxl::AdxlEngine::FreeMem(buffers_[kind]), "AdxlEngine::FreeMem"); buffers_[kind] = nullptr;
      }
    }
    if (device_ready_) { Check(aclrtResetDevice(gDevice), "aclrtResetDevice"); device_ready_ = false; }
    if (acl_ready_) { Check(aclFinalize(), "aclFinalize"); acl_ready_ = false; }
  }

  ~Endpoint() {
    try { Close(); } catch (const std::exception &e) { std::fprintf(stderr, "CLEANUP_FAIL %s\n", e.what()); }
  }

  static const char *Name(int kind) { return kind == 0 ? "DRAM" : "HBM"; }
  Peer peer_{};
  hixl::Hixl engine_;
  std::vector<std::string> connected_engines_;
  void *buffers_[2]{};
 private:
  int rank_;
  hixl::MemHandle handles_[2]{};
  bool acl_ready_ = false;
  bool device_ready_ = false;
  bool engine_ready_ = false;
  aclrtDrvMemHandle raw_handles_[2]{};
  bool raw_mapped_[2]{};
};

struct FabricDescriptors {
  aclrtMemFabricHandle handles[2]{};
};

class KernelViews {
 public:
  KernelViews() = default;
  void Initialize(Endpoint &ep, int rank) {
    rank_ = rank;
    addresses_.resize(gWorld * 2);
    imported_.resize(gWorld * 2);
    remote_va_.resize(gWorld * 2);
    mapped_.resize(gWorld * 2);
    if (gDomainHandles) {
      acquire_ = reinterpret_cast<Acquire>(dlsym(RTLD_DEFAULT, "HixlDomainWindowAcquire"));
      release_ = reinterpret_cast<Release>(dlsym(RTLD_DEFAULT, "HixlDomainWindowRelease"));
      if (!acquire_ || !release_) throw std::runtime_error("domain window bridge API is missing");
    }
    FabricDescriptors local{};
    for (int kind = 0; kind < 2; ++kind) {
      addresses_[rank * 2 + kind] = ep.buffers_[kind];
      if (gDomainHandles) {
        Check(acquire_(ep.buffers_[kind], kBytes, &local.handles[kind]), "domain window acquire");
        acquired_[kind] = ep.buffers_[kind];
        aclrtMemFabricHandle duplicate{};
        Check(acquire_(ep.buffers_[kind], kBytes, &duplicate), "domain window acquire again");
        if (std::memcmp(&duplicate, &local.handles[kind], sizeof(duplicate)) != 0) {
          throw std::runtime_error("idempotent acquire returned a different Fabric handle");
        }
        std::printf("LIFECYCLE_ACQUIRE_PASS rank=%d kind=%s identical_handle=true\n", rank, Endpoint::Name(kind));
      } else {
        Check(aclrtMemRetainAllocationHandle(ep.buffers_[kind], &retained_[kind]), "retain local Fabric PA");
        Check(aclrtMemExportToShareableHandleV2(retained_[kind], ACL_RT_VMM_EXPORT_FLAG_DISABLE_PID_VALIDATION,
                                               ACL_MEM_SHARE_HANDLE_TYPE_FABRIC, &local.handles[kind]),
              "export kernel Fabric handle");
      }
    }
    auto descriptors = gGroup->AllGather(local);
    for (int peer = 0; peer < gWorld; ++peer) {
      if (peer == rank) continue;
      for (int kind = 0; kind < 2; ++kind) {
        const int index = peer * 2 + kind;
        Check(aclrtMemImportFromShareableHandleV2(&descriptors[peer].handles[kind],
                ACL_MEM_SHARE_HANDLE_TYPE_FABRIC, 0, &imported_[index]), "import kernel Fabric handle");
        Check(aclrtReserveMemAddress(&remote_va_[index], kBytes, 0, nullptr, 1), "reserve kernel peer VA");
        Check(aclrtMapMem(remote_va_[index], kBytes, 0, imported_[index], 0), "map kernel peer VA");
        mapped_[index] = true;
        addresses_[index] = remote_va_[index];
      }
    }
    library_ = dlopen(gKernelLibrary.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!library_) throw std::runtime_error(std::string("dlopen kernel: ") + dlerror());
    launch_ = reinterpret_cast<Launch>(dlsym(library_, "launch_fabric_copy"));
    if (!launch_) throw std::runtime_error("kernel entry missing");
    Check(aclrtCreateStream(&stream_), "kernel create stream");
    std::printf("KERNEL_MAPS_READY rank=%d windows=%d auxiliary_acl_mapping=true\n", rank, gWorld * 2);
  }

  void Copy(int source, int destination, size_t bytes, size_t source_offset = kSourceOffset,
            size_t target_offset = kTargetOffset) {
    void *dst = static_cast<char *>(addresses_[destination]) + target_offset;
    const void *src = static_cast<char *>(addresses_[source]) + source_offset;
    if (launch_(dst, src, bytes, stream_) != 0) throw std::runtime_error("kernel launch wrapper failed");
    Check(aclrtSynchronizeStreamWithTimeout(stream_, kTimeoutMs), "kernel stream complete");
  }

  void CheckRegisteredReleaseGuard() {
    if (!gDomainHandles) return;
    for (void *address : acquired_) {
      if (release_(address) != hixl::RESOURCE_EXHAUSTED) {
        throw std::runtime_error("window release was not rejected while HIXL registered it");
      }
    }
    std::printf("LIFECYCLE_BUSY_PASS rank=%d registered_window_release_rejected=true\n", rank_);
  }

  void CloseMappings() {
    if (stream_) { Check(aclrtDestroyStream(stream_), "destroy kernel stream"); stream_ = nullptr; }
    if (library_) { dlclose(library_); library_ = nullptr; }
    for (size_t index = 0; index < remote_va_.size(); ++index) {
      if (mapped_[index]) { Check(aclrtUnmapMem(remote_va_[index]), "unmap kernel peer"); mapped_[index] = false; }
      if (remote_va_[index]) {
        Check(aclrtReleaseMemAddress(remote_va_[index]), "release kernel peer VA"); remote_va_[index] = nullptr;
      }
      if (imported_[index]) {
        Check(aclrtFreePhysical(imported_[index]), "release imported PA"); imported_[index] = nullptr;
      }
    }
  }

  void ReleaseLocal() {
    for (int kind = 0; kind < 2; ++kind) {
      if (retained_[kind]) { Check(aclrtFreePhysical(retained_[kind]), "release retained PA"); retained_[kind] = nullptr; }
      if (acquired_[kind]) {
        void *address = acquired_[kind];
        Check(release_(address), "domain window release"); acquired_[kind] = nullptr;
        if (release_(address) != hixl::PARAM_INVALID) throw std::runtime_error("duplicate release accepted");
        std::printf("LIFECYCLE_RELEASE_PASS rank=%d kind=%s double_release_rejected=true\n", rank_, Endpoint::Name(kind));
      }
    }
  }

  void Close() { CloseMappings(); ReleaseLocal(); }

  ~KernelViews() {
    try { Close(); } catch (const std::exception &e) { std::fprintf(stderr, "KERNEL_CLEANUP_FAIL %s\n", e.what()); }
  }

 private:
  using Launch = int (*)(void *, const void *, size_t, void *);
  using Acquire = unsigned (*)(void *, size_t, aclrtMemFabricHandle *);
  using Release = unsigned (*)(void *);
  int rank_ = -1;
  std::vector<void *> addresses_;
  aclrtDrvMemHandle retained_[2]{};
  std::vector<aclrtDrvMemHandle> imported_;
  std::vector<void *> remote_va_;
  std::vector<bool> mapped_;
  void *library_ = nullptr;
  Launch launch_ = nullptr;
  aclrtStream stream_ = nullptr;
  Acquire acquire_ = nullptr;
  Release release_ = nullptr;
  void *acquired_[2]{};
};

struct Counts { int hixl = 0; int kernel = 0; int handoff = 0; };

void Baseline(Endpoint &ep, int rank) {
  for (bool to_hbm : {true, false}) {
    int source = to_hbm ? 0 : 1;
    int target = 1 - source;
    ep.Fill(source);
    ep.Verify(source, -1, -1, -1, -1, 0);
    Check(aclrtMemcpy(static_cast<char *>(ep.buffers_[target]) + kTargetOffset, kBytes - kTargetOffset,
                      static_cast<char *>(ep.buffers_[source]) + kSourceOffset, 4096,
                      to_hbm ? ACL_MEMCPY_HOST_TO_DEVICE : ACL_MEMCPY_DEVICE_TO_HOST), "baseline copy");
    ep.Verify(source, rank, target, rank, source, 4096);
    std::printf("BASELINE_PASS rank=%d direction=%s guards=PASS\n", rank, to_hbm ? "DRAM_TO_HBM" : "HBM_TO_DRAM");
  }
  Barrier(-10);
}

void RunHixl(Endpoint &ep, const std::vector<Peer> &peers, int rank, Counts &counts) {
  for (size_t length : {4096UL, 65536UL}) {
    for (int actor = 0; actor < gWorld; ++actor) {
      for (int peer = 0; peer < gWorld; ++peer) {
        if (peer == actor) continue;
        for (int local_kind = 0; local_kind < 2; ++local_kind) {
          for (int remote_kind = 0; remote_kind < 2; ++remote_kind) {
            for (bool read : {false, true}) {
              for (bool async : {false, true}) {
                int test = ++counts.hixl;
                ep.Fill(test);
                Barrier(test * 3);
                if (rank == actor) ep.Transfer(peers[peer], local_kind, remote_kind, read, async, length);
                Barrier(test * 3 + 1);
                ep.Verify(test, read ? actor : peer, read ? local_kind : remote_kind,
                          read ? peer : actor, read ? remote_kind : local_kind, length);
                Barrier(test * 3 + 2);
                if (rank == actor) {
                  std::printf("CASE_PASS id=%d actor=%d peer=%d local=%s remote=%s op=%s completion=%s bytes=%zu guards=PASS\n",
                              test, actor, peer, Endpoint::Name(local_kind), Endpoint::Name(remote_kind),
                              read ? "READ" : "WRITE", async ? "ASYNC_POLLED" : "SYNC", length);
                }
              }
            }
          }
        }
      }
    }
  }
}

void RunKernel(Endpoint &ep, KernelViews &views, int rank, Counts &counts) {
  for (size_t length : {4096UL, 65536UL}) {
    for (int actor = 0; actor < gWorld; ++actor) {
      for (int source = 0; source < gWorld * 2; ++source) {
        for (int destination = 0; destination < gWorld * 2; ++destination) {
          if (source == destination) continue;
          int test = ++counts.kernel;
          ep.Fill(test);
          Barrier(test * 3);
          if (rank == actor) views.Copy(source, destination, length);
          Barrier(test * 3 + 1);
          ep.Verify(test, destination / 2, destination % 2, source / 2, source % 2, length);
          Barrier(test * 3 + 2);
          if (rank == actor) {
            std::printf("KERNEL_CASE_PASS id=%d actor=%d source=%s%d destination=%s%d bytes=%zu completion=STREAM_SYNC guards=PASS\n",
                        test, actor, Endpoint::Name(source % 2), source / 2,
                        Endpoint::Name(destination % 2), destination / 2, length);
          }
        }
      }
    }
  }
}

void RunHandoffs(Endpoint &ep, KernelViews &views, const std::vector<Peer> &peers, int rank, Counts &counts) {
  for (size_t length : {4096UL, 65536UL}) {
    for (int producer = 0; producer < gWorld; ++producer) {
      for (int consumer = 0; consumer < gWorld; ++consumer) {
        if (producer == consumer) continue;
        for (bool kernel_first : {false, true}) {
          int test = ++counts.handoff;
          ep.Fill(test);
          Barrier(test * 8);
          if (kernel_first) {
            if (rank == consumer) views.Copy(producer * 2, consumer * 2 + 1, length);
          } else if (rank == producer) {
            ep.Transfer(peers[consumer], 0, 1, false, false, length);
          }
          Barrier(test * 8 + 1);
          // No ACL data readback between producer and consumer: completion and control only.
          if (kernel_first) {
            if (rank == producer) ep.Transfer(peers[consumer], 0, 1, true, true, length, kTargetOffset, kTargetOffset);
          } else if (rank == consumer) {
            views.Copy(consumer * 2 + 1, producer * 2, length, kTargetOffset, kTargetOffset);
          }
          Barrier(test * 8 + 2);
          if (rank == producer || rank == consumer) {
            ep.Verify(test, rank, rank == producer ? 0 : 1, producer, 0, length);
          } else {
            ep.Verify(test, -1, -1, -1, -1, 0);
          }
          Barrier(test * 8 + 3);
          if (rank == producer) {
            std::printf("HANDOFF_PASS id=%d producer=%d consumer=%d mode=%s bytes=%zu same_windows=true guards=PASS\n",
                        test, producer, consumer,
                        kernel_first ? "KERNEL_THEN_HIXL_ASYNC_CPU_READ" : "HIXL_SYNC_THEN_KERNEL_CPU_READ", length);
          }
        }
      }
    }
  }
}

void Run(int rank, const std::string &mode) {
  // Failure is fail-stop: main exits without unsafe distributed cleanup. Normal cleanup is explicit.
  auto *owner = new Endpoint(rank);
  auto *views_owner = new KernelViews;
  Endpoint &ep = *owner;
  KernelViews &views = *views_owner;
  ep.Allocate();
  Baseline(ep, rank);
  bool hixl = mode == "hixl" || mode == "combined";
  bool kernel = mode == "kernel" || mode == "combined";
  auto identities = gGroup->AllGather(ep.peer_);
  for (int peer = 0; peer < rank; ++peer) {
    if (identities[peer].physical == ep.peer_.physical &&
        std::strcmp(identities[peer].hostname, ep.peer_.hostname) == 0) {
      throw std::runtime_error("ranks on the same host use the same physical NPU");
    }
  }
  if (kernel) views.Initialize(ep, rank);
  if (hixl) ep.InitializeEngine();
  if (mode == "combined") views.CheckRegisteredReleaseGuard();
  auto peers = gGroup->AllGather(ep.peer_);
  if (hixl) {
    for (int peer = 0; peer < gWorld; ++peer) {
      if (peer == rank) continue;
      std::printf("CONNECT_BEGIN rank=%d peer=%d\n", rank, peer);
      Check(ep.engine_.Connect(peers[peer].engine, kTimeoutMs), "Hixl::Connect");
      ep.connected_engines_.push_back(peers[peer].engine);
    }
  }
  Barrier(0);
  Counts counts;
  if (hixl) RunHixl(ep, peers, rank, counts);
  if (kernel) RunKernel(ep, views, rank, counts);
  if (mode == "combined") RunHandoffs(ep, views, peers, rank, counts);
  Barrier(-1);  // Every NPU stream and every HIXL operation has completed.
  ep.CloseEngine();
  Barrier(-2);  // No HIXL peer retains imported mappings after this point.
  views.CloseMappings();
  Barrier(-3);  // Every kernel importer is gone before any owner releases its export.
  views.ReleaseLocal();
  ep.Close();
  Barrier(-4);  // Report success only after all owners have cleaned up.
  delete views_owner;
  delete owner;
  std::printf("RANK_PASS rank=%d hixl_cases=%d kernel_cases=%d handoff_cases=%d cleanup=PASS\n",
              rank, counts.hixl, counts.kernel, counts.handoff);
}
}  // namespace

int main(int argc, char **argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  signal(SIGPIPE, SIG_IGN);
  int rank = -1;
  int timeout = 60;
  std::string coordinator, mode, session;
  try {
    for (int arg = 1; arg < argc; ++arg) {
      std::string option = argv[arg];
      if (arg + 1 >= argc) throw std::runtime_error("each option needs a value");
      std::string value = argv[++arg];
      auto number = [&]() {
        size_t used = 0;
        int result = std::stoi(value, &used);
        if (used != value.size()) throw std::runtime_error("invalid integer");
        return result;
      };
      if (option == "--rank") rank = number();
      else if (option == "--world") gWorld = number();
      else if (option == "--device") gDevice = number();
      else if (option == "--numa") gNuma = number();
      else if (option == "--coordinator") coordinator = value;
      else if (option == "--engine") gEngine = value;
      else if (option == "--mode") mode = value;
      else if (option == "--session") session = value;
      else if (option == "--kernel-library") gKernelLibrary = value;
      else if (option == "--control-timeout") timeout = number();
      else if (option == "--allocator") {
        if (value != "vmm" && value != "adxl") throw std::runtime_error("allocator must be vmm or adxl");
        gRawVmm = value == "vmm";
      } else throw std::runtime_error("unknown option: " + option);
    }
    if (gWorld < 1 || gWorld > 64 || rank < 0 || rank >= gWorld || gDevice < 0 || gNuma < -1 ||
        timeout < 1 || timeout > 3600 || session.size() != 64 || coordinator.empty() ||
        (mode != "baseline" && mode != "hixl" && mode != "kernel" && mode != "combined") ||
        (mode != "baseline" && gWorld < 2) ||
        ((mode == "kernel" || mode == "combined") && gKernelLibrary.empty())) {
      throw std::runtime_error("invalid/missing options; use scripts/run.py");
    }
    gDomainHandles = mode == "combined";
    struct Settings { char session[65]{}; char mode[16]{}; uint32_t handle_size; uint32_t pointer_size; uint32_t endian; };
    Settings settings{};
    std::snprintf(settings.session, sizeof(settings.session), "%s", session.c_str());
    std::snprintf(settings.mode, sizeof(settings.mode), "%s", mode.c_str());
    settings.handle_size = sizeof(aclrtMemFabricHandle);
    settings.pointer_size = sizeof(void *);
    settings.endian = 0x01020304;
    control::Group group(rank, gWorld, coordinator, timeout);
    gGroup = &group;
    for (const auto &peer : group.AllGather(settings)) {
      if (std::strcmp(peer.session, settings.session) != 0 || std::strcmp(peer.mode, settings.mode) != 0 ||
          peer.handle_size != settings.handle_size || peer.pointer_size != settings.pointer_size ||
          peer.endian != settings.endian) throw std::runtime_error("ranks disagree on config/mode/ABI");
    }
    struct Nonce { uint32_t words[4]{}; } nonce;
    if (rank == 0) {
      std::random_device random;
      for (auto &word : nonce.words) word = random();
    }
    auto run_id = group.AllGather(nonce)[0];
    std::printf("RUN_ID=%08x%08x%08x%08x\n", run_id.words[0], run_id.words[1], run_id.words[2], run_id.words[3]);
    Run(rank, mode);
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "RANK_FAIL rank=%d error=%s\n", rank, error.what());
    std::fflush(nullptr);
    // Peers get EOF/deadline and fail; never claim coordinated cleanup on a failed run.
    std::_Exit(1);
  }
}
