// DRAM_A -> DRAM_B only. The TCP group exchanges handles/status, never payload.
#include <acl/acl.h>
#include <acl/acl_prof.h>
#include <dlfcn.h>
#include <signal.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "control.h"
#include "pattern.h"

namespace {
constexpr size_t kAllocation = 4UL * 1024 * 1024;
constexpr size_t kSource = 256;
constexpr size_t kTarget = 2UL * 1024 * 1024;
constexpr int kTimeoutMs = 120000;

void Check(aclError code, const char *operation) {
  if (code != ACL_SUCCESS) throw std::runtime_error(std::string(operation) + " rc=" + std::to_string(code));
}
uint64_t Now() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct Peer {
  aclrtMemFabricHandle handle{};
  char host[256]{};
  char session[65]{};
  uint64_t bytes = 0;
  uint64_t iterations = 0;
  uint64_t idle_ms = 0;
};

class Memory {
 public:
  void Open(int device, int numa, control::Group &group, Peer &local) {
    aclrtPhysicalMemProp prop{};
    prop.handleType = ACL_MEM_HANDLE_TYPE_NONE;
    prop.allocationType = ACL_MEM_ALLOCATION_TYPE_PINNED;
    prop.memAttr = ACL_MEM_P2P_HUGE;
    prop.location.type = ACL_MEM_LOCATION_TYPE_HOST_NUMA;
    prop.location.id = numa;
    Check(aclrtMallocPhysical(&physical_, kAllocation, &prop, 0), "allocate DRAM");
    Check(aclrtReserveMemAddress(&host_, kAllocation, 0, nullptr, 1), "reserve CPU VA");
    Check(aclrtMapMem(host_, kAllocation, 0, physical_, 0), "map CPU VA");
    int driver_id = device;
    using Version = int32_t (*)(const char *, int32_t *);
    auto version = reinterpret_cast<Version>(dlsym(RTLD_DEFAULT, "aclsysGetVersionNum"));
    int32_t runtime = 0;
    if (version) Check(version("runtime", &runtime), "runtime version");
    if (runtime < 90200000) Check(aclrtGetLogicDevIdByUserDevId(device, &driver_id), "access device ID");
    aclrtMemAccessDesc access{};
    access.flags = ACL_RT_MEM_ACCESS_FLAGS_READWRITE;
    access.location.type = ACL_MEM_LOCATION_TYPE_DEVICE;
    access.location.id = driver_id;
    Check(aclrtMemSetAccess(host_, kAllocation, &access, 1), "DRAM device access");
    Check(aclrtMemRetainAllocationHandle(host_, &retained_), "retain DRAM");
    Check(aclrtMemExportToShareableHandleV2(retained_, ACL_RT_VMM_EXPORT_FLAG_DISABLE_PID_VALIDATION,
        ACL_MEM_SHARE_HANDLE_TYPE_FABRIC, &local.handle), "export DRAM");
    auto peers = group.AllGather(local);
    for (const auto &peer : peers) {
      if (std::strcmp(peer.session, local.session) || peer.bytes != local.bytes ||
          peer.iterations != local.iterations || peer.idle_ms != local.idle_ms) {
        throw std::runtime_error("peer configuration/source/run-ID mismatch");
      }
    }
    std::printf("HOSTS same_host=%d\n", std::strcmp(peers[0].host, peers[1].host) == 0);
    // Same Fabric allocations as CPU mappings. No HBM or staging allocation.
    Check(aclrtReserveMemAddressNoUCMemory(&arena_, 1UL << 30, 0,
        reinterpret_cast<void *>(40UL << 40), 0), "reserve non-UC device VA");
    for (int rank = 0; rank < 2; ++rank) {
      Check(aclrtMemImportFromShareableHandleV2(&peers[rank].handle,
          ACL_MEM_SHARE_HANDLE_TYPE_FABRIC, 0, &imported_[rank]), "import DRAM");
      views_[rank] = static_cast<char *>(arena_) + rank * kAllocation;
      Check(aclrtMapMem(views_[rank], kAllocation, 0, imported_[rank], 0), "map device DRAM VA");
    }
    Check(aclrtCreateStream(&stream_), "create SDMA stream");
  }
  void Fill(int phase, int rank) {
    auto *bytes = static_cast<uint8_t *>(host_);
    for (size_t i = 0; i < kAllocation; ++i) bytes[i] = Pattern(phase, rank, 0, i);
  }
  void Verify(int phase, int rank, size_t bytes, bool copied) {
    auto *data = static_cast<uint8_t *>(host_);
    for (size_t i = 0; i < kAllocation; ++i) {
      auto expected = copied && rank == 1 && i >= kTarget && i < kTarget + bytes
          ? Pattern(phase, 0, 0, i - kTarget + kSource) : Pattern(phase, rank, 0, i);
      if (data[i] != expected) throw std::runtime_error("DRAM data/guard mismatch offset=" + std::to_string(i));
    }
  }
  void Copy(uint64_t iterations, size_t bytes) {
    for (uint64_t i = 0; i < iterations; ++i) {
      Check(aclrtMemcpyAsync(Destination(), bytes, Source(), bytes,
          ACL_MEMCPY_DEVICE_TO_DEVICE, stream_), "DRAM_A -> DRAM_B submit");
      if ((i + 1) % 32 == 0) Check(aclrtSynchronizeStreamWithTimeout(stream_, kTimeoutMs), "batch complete");
    }
    Check(aclrtSynchronizeStreamWithTimeout(stream_, kTimeoutMs), "all copies complete");
  }
  void QueryDriver() {
    // Signature matches CANN runtime NpuDriver::GetTransWayByAddr. Query the exact copy VAs.
    void *library = dlopen("libascend_hal.so", RTLD_NOW | RTLD_LOCAL);
    using Query = int (*)(void *, void *, uint8_t *);
    auto query = reinterpret_cast<Query>(dlsym(library ? library : RTLD_DEFAULT, "drvDeviceGetTransWay"));
    uint8_t way = 255;
    int rc = query ? query(Source(), Destination(), &way) : -1;
    // Installed runtime mem.h enum: 0=INNER, 1=PCIe, 2=HCCs, 3=UB.
    const char *name = rc != 0 ? "UNAVAILABLE" : way == 0 ? "INNER" : way == 1 ? "PCIe" :
        way == 2 ? "HCCS" : way == 3 ? "UB" : "UNKNOWN";
    std::printf("DRIVER_QUERY src=%p dst=%p rc=%d raw=%u classification=%s initiator=0\n",
                Source(), Destination(), rc, way, name);
    if (library && dlclose(library) != 0) throw std::runtime_error("driver dlclose failed");
  }
  void CloseMappings() {
    Check(aclrtDestroyStream(stream_), "destroy stream");
    for (int rank = 0; rank < 2; ++rank) {
      Check(aclrtUnmapMem(views_[rank]), "unmap device VA");
      Check(aclrtFreePhysical(imported_[rank]), "free imported DRAM");
    }
    Check(aclrtReleaseMemAddress(arena_), "release device VA arena");
  }
  void CloseOwner() {
    Check(aclrtFreePhysical(retained_), "release retained export");
    Check(aclrtUnmapMem(host_), "unmap CPU VA");
    Check(aclrtReleaseMemAddress(host_), "release CPU VA");
    Check(aclrtFreePhysical(physical_), "free owner DRAM");
  }
 private:
  void *Source() { return static_cast<char *>(views_[0]) + kSource; }
  void *Destination() { return static_cast<char *>(views_[1]) + kTarget; }
  void *host_ = nullptr;
  void *arena_ = nullptr;
  void *views_[2]{};
  aclrtDrvMemHandle physical_ = nullptr;
  aclrtDrvMemHandle retained_ = nullptr;
  aclrtDrvMemHandle imported_[2]{};
  aclrtStream stream_ = nullptr;
};

void Sample(Memory &memory, control::Group &group, int rank, int device, int phase,
            const std::string &output, uint64_t iterations, size_t bytes, int idle_ms) {
  const char *name = phase == 0 ? "idle" : phase == 1 ? "n" : "2n";
  // All CPU writes and verification occur while profiling is OFF.
  memory.Fill(phase, rank);
  group.Barrier(100 + phase);
  std::string path = output + "/" + name;
  std::filesystem::create_directories(path);
  Check(aclprofInit(path.c_str(), path.size()), "profiler init");
  uint32_t id = device;
  auto *config = aclprofCreateConfig(&id, 1, ACL_AICORE_NONE, nullptr, ACL_PROF_TASK_TIME);
  if (!config) throw std::runtime_error("profiler config failed");
  Check(aclprofSetConfig(ACL_PROF_SYS_INTERCONNECTION_FREQ, "50", 2), "enable PCIe/HCCS 50Hz");
  group.Barrier(200 + phase);
  Check(aclprofStart(config), "profiler start");
  uint64_t start = Now();
  group.Barrier(300 + phase);
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  uint64_t begin = Now();
  uint64_t count = phase == 0 ? 0 : iterations * (phase == 2 ? 2 : 1);
  if (rank == 0) {
    if (count) memory.Copy(count, bytes);
    else std::this_thread::sleep_for(std::chrono::milliseconds(idle_ms));
  }
  group.Barrier(400 + phase);  // Rank A has already synchronized the SDMA stream.
  uint64_t end = Now();
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  Check(aclprofStop(config), "profiler stop");
  uint64_t stop = Now();
  Check(aclprofDestroyConfig(config), "profiler destroy");
  Check(aclprofFinalize(), "profiler finalize");
  group.Barrier(500 + phase);  // Both profilers stopped before either CPU touches DRAM.
  memory.Verify(phase, rank, bytes, count != 0);
  group.Barrier(600 + phase);
  std::printf("PHASE_PASS phase=%s rank=%d copies=%llu payload_bytes=%llu sample_start_ns=%llu "
              "payload_start_ns=%llu payload_end_ns=%llu sample_stop_ns=%llu guards=PASS\n",
              name, rank, static_cast<unsigned long long>(count),
              static_cast<unsigned long long>(count * bytes), static_cast<unsigned long long>(start),
              static_cast<unsigned long long>(begin), static_cast<unsigned long long>(end),
              static_cast<unsigned long long>(stop));
}
}  // namespace

int main(int argc, char **argv) {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  signal(SIGPIPE, SIG_IGN);
  try {
    if (argc != 10) throw std::runtime_error("internal usage: rank device numa coordinator output bytes iterations idle_ms session");
    int rank = std::stoi(argv[1]), device = std::stoi(argv[2]), numa = std::stoi(argv[3]);
    Peer local{};
    local.bytes = std::stoull(argv[6]);
    local.iterations = std::stoull(argv[7]);
    local.idle_ms = std::stoull(argv[8]);
    if (rank < 0 || rank > 1 || device < 0 || numa < 0 || local.bytes < 4096 ||
        local.bytes > 1024 * 1024 || local.iterations < 1 || local.iterations > 1000000 ||
        local.idle_ms < 100 || local.idle_ms > 120000 || std::strlen(argv[9]) != 64) {
      throw std::runtime_error("invalid route parameters");
    }
    std::strcpy(local.session, argv[9]);
    if (gethostname(local.host, sizeof(local.host) - 1) != 0) throw std::runtime_error("hostname failed");
    control::Group group(rank, 2, argv[4], 300);
    Check(aclInit(nullptr), "aclInit");
    Check(aclrtSetDevice(device), "set device");
    int physical = -1;
    Check(aclrtGetPhyDevIdByLogicDevId(device, &physical), "physical device ID");
    std::printf("DEVICE rank=%d logical=%d physical=%d numa=%d dram_bytes=%zu hbm_allocations=0\n",
                rank, device, physical, numa, kAllocation);
    Memory memory;
    memory.Open(device, numa, group, local);
    if (rank == 0) memory.QueryDriver();
    memory.Fill(7, rank);
    group.Barrier(1);
    if (rank == 0) memory.Copy(32, local.bytes);
    group.Barrier(2);
    memory.Verify(7, rank, local.bytes, true);
    group.Barrier(3);
    std::printf("WARMUP_PASS rank=%d copies=32\n", rank);
    for (int phase = 0; phase < 3; ++phase) {
      Sample(memory, group, rank, device, phase, argv[5], local.iterations, local.bytes, local.idle_ms);
    }
    group.Barrier(10);
    memory.CloseMappings();
    group.Barrier(11);
    memory.CloseOwner();
    Check(aclrtResetDevice(device), "reset device");
    Check(aclFinalize(), "aclFinalize");
    group.Barrier(12);
    std::printf("ROUTE_RANK_PASS rank=%d cleanup=PASS\n", rank);
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "ROUTE_RANK_FAIL %s\n", error.what());
    return 1;
  }
}
