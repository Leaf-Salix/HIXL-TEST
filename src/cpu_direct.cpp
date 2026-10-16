// Capability probe: CPU accesses remote DRAM only after explicit HOST access succeeds.
// There are no stream, memcpy, kernel or DMA submission APIs in this executable.
#include <acl/acl.h>
#include <signal.h>

#include <atomic>
#include <cstdio>
#include <string>

#include "control.h"
#include "pattern.h"

namespace {
constexpr size_t kAllocation = 2UL * 1024 * 1024;
constexpr size_t kSource = 256;
constexpr size_t kTarget = 1024 * 1024;
constexpr size_t kBytes = 64 * 1024;

void Check(aclError code, const char *operation) {
  if (code != ACL_SUCCESS) throw std::runtime_error(std::string(operation) + " rc=" + std::to_string(code));
}
void AccessFault(int signal_number) {
  constexpr char text[] = "CPU_DIRECT_ACCESS_FAULT: CPU load/store or runtime fault; not a PASS\n";
  (void)write(STDERR_FILENO, text, sizeof(text) - 1);
  _exit(128 + signal_number);
}
void Fence() {
  std::atomic_thread_fence(std::memory_order_seq_cst);
#if defined(__aarch64__)
  asm volatile("dsb sy" ::: "memory");
#endif
}
struct Peer {
  aclrtMemFabricHandle handle{};
  char hostname[256]{};
  char session[65]{};
};
struct Access {
  int set_rc = -1;
  int get_rc = -1;
  uint64_t flags = 0;
  int ready = 0;
};

class Memory {
 public:
  void Open(int numa, int rank, control::Group &group, Peer &local) {
    aclrtPhysicalMemProp prop{};
    prop.handleType = ACL_MEM_HANDLE_TYPE_NONE;
    prop.allocationType = ACL_MEM_ALLOCATION_TYPE_PINNED;
    prop.memAttr = ACL_MEM_P2P_HUGE;
    prop.location.type = ACL_MEM_LOCATION_TYPE_HOST_NUMA;
    prop.location.id = numa;
    Check(aclrtMallocPhysical(&physical_, kAllocation, &prop, 0), "allocate Host DRAM");
    Check(aclrtReserveMemAddress(&owner_, kAllocation, 0, nullptr, 1), "reserve owner VA");
    Check(aclrtMapMem(owner_, kAllocation, 0, physical_, 0), "map owner DRAM");
    Check(aclrtMemExportToShareableHandleV2(physical_, ACL_RT_VMM_EXPORT_FLAG_DISABLE_PID_VALIDATION,
        ACL_MEM_SHARE_HANDLE_TYPE_FABRIC, &local.handle), "export Host DRAM");
    auto peers = group.AllGather(local);
    for (const auto &peer : peers) {
      if (std::strcmp(peer.session, local.session)) throw std::runtime_error("source/config/run-ID mismatch");
    }
    std::printf("HOSTS same_host=%d\n", std::strcmp(peers[0].hostname, peers[1].hostname) == 0);
    Check(aclrtMemImportFromShareableHandleV2(&peers[1 - rank].handle,
        ACL_MEM_SHARE_HANDLE_TYPE_FABRIC, 0, &imported_), "import remote Host DRAM");
    // Ordinary VMM reservation, never reinterpret a NoUCMemory device arena as a CPU pointer.
    Check(aclrtReserveMemAddress(&remote_, kAllocation, 0, nullptr, 1), "reserve peer VA");
    Check(aclrtMapMem(remote_, kAllocation, 0, imported_, 0), "map imported DRAM");
  }
  Access RequestHostAccess() {
    Access result;
    aclrtMemAccessDesc desc{};
    desc.location.type = ACL_MEM_LOCATION_TYPE_HOST;
    desc.location.id = 0;
    desc.flags = ACL_RT_MEM_ACCESS_FLAGS_READWRITE;
    result.set_rc = aclrtMemSetAccess(remote_, kAllocation, &desc, 1);
    if (result.set_rc == ACL_ERROR_RT_FEATURE_NOT_SUPPORT) return result;
    Check(result.set_rc, "set HOST access");
    result.get_rc = aclrtMemGetAccess(remote_, &desc.location, &result.flags);
    if (result.get_rc == ACL_ERROR_RT_FEATURE_NOT_SUPPORT) return result;
    Check(result.get_rc, "query HOST access");
    if (result.flags != ACL_RT_MEM_ACCESS_FLAGS_READWRITE) throw std::runtime_error("HOST access is not READWRITE");
    result.ready = 1;
    return result;
  }
  void Fill(int phase, int rank) {
    auto *data = static_cast<uint8_t *>(owner_);
    for (size_t i = 0; i < kAllocation; ++i) data[i] = Pattern(phase, rank, 0, i);
    Fence();
  }
  void Copy(bool write) {
    auto *src = static_cast<volatile uint8_t *>(write ? owner_ : remote_) + kSource;
    auto *dst = static_cast<volatile uint8_t *>(write ? remote_ : owner_) + kTarget;
    // Volatile scalar accesses cannot be replaced by an ACL memcpy or a kernel launch.
    for (size_t i = 0; i < kBytes; ++i) dst[i] = src[i];
    Fence();
  }
  void Verify(int phase, int rank, int actor, bool write) {
    const int target = write ? 1 - actor : actor;
    const int source = write ? actor : 1 - actor;
    auto *data = static_cast<volatile uint8_t *>(owner_);
    Fence();
    for (size_t i = 0; i < kAllocation; ++i) {
      uint8_t expected = rank == target && i >= kTarget && i < kTarget + kBytes
          ? Pattern(phase, source, 0, i - kTarget + kSource) : Pattern(phase, rank, 0, i);
      if (data[i] != expected) throw std::runtime_error("CPU data/guard mismatch at " + std::to_string(i));
    }
  }
  void CloseImporter() {
    Check(aclrtUnmapMem(remote_), "unmap peer");
    Check(aclrtReleaseMemAddress(remote_), "release peer VA");
    Check(aclrtFreePhysical(imported_), "release imported handle");
  }
  void CloseOwner() {
    Check(aclrtUnmapMem(owner_), "unmap owner");
    Check(aclrtReleaseMemAddress(owner_), "release owner VA");
    Check(aclrtFreePhysical(physical_), "release owner physical memory");
  }
 private:
  void *owner_ = nullptr;
  void *remote_ = nullptr;
  aclrtDrvMemHandle physical_ = nullptr;
  aclrtDrvMemHandle imported_ = nullptr;
};
}  // namespace

int main(int argc, char **argv) {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  signal(SIGPIPE, SIG_IGN);
  try {
    if (argc != 6) throw std::runtime_error("internal usage: rank device numa coordinator session");
    int rank = std::stoi(argv[1]), device = std::stoi(argv[2]), numa = std::stoi(argv[3]);
    if ((rank != 0 && rank != 1) || device < 0 || numa < 0 || std::strlen(argv[5]) != 64) {
      throw std::runtime_error("invalid arguments");
    }
    Peer local{};
    std::strcpy(local.session, argv[5]);
    if (gethostname(local.hostname, sizeof(local.hostname) - 1) != 0) throw std::runtime_error("hostname failed");
    control::Group group(rank, 2, argv[4], 120);
    Check(aclInit(nullptr), "aclInit");
    Check(aclrtSetDevice(device), "device context for Fabric metadata");
    int physical = -1;
    Check(aclrtGetPhyDevIdByLogicDevId(device, &physical), "device ID");
    std::printf("DEVICE rank=%d logical=%d physical=%d numa=%d\n", rank, device, physical, numa);
    Memory memory;
    memory.Open(numa, rank, group, local);
    Access access = memory.RequestHostAccess();
    std::printf("HOST_ACCESS rank=%d set_rc=%d get_rc=%d flags=%llu ready=%d\n", rank,
                access.set_rc, access.get_rc, static_cast<unsigned long long>(access.flags), access.ready);
    bool supported = true;
    for (const auto &peer : group.AllGather(access)) supported = supported && peer.ready == 1;
    if (supported) {
      // Launcher isolates this process; no post-ACL fork with potentially non-inherited VMAs.
      struct sigaction action{};
      action.sa_handler = AccessFault;
      sigemptyset(&action.sa_mask);
      if (sigaction(SIGBUS, &action, nullptr) || sigaction(SIGSEGV, &action, nullptr)) {
        throw std::runtime_error("install CPU access fault handler");
      }
      std::puts("CPU_IO_BEGIN streams=0 memcpy_calls=0 kernel_launches=0 fallback=NONE");
      for (int actor = 0; actor < 2; ++actor) {
        for (int write = 0; write < 2; ++write) {
          int phase = actor * 2 + write;
          memory.Fill(phase, rank);
          group.Barrier(phase * 3);
          if (rank == actor) memory.Copy(write != 0);
          group.Barrier(phase * 3 + 1);
          memory.Verify(phase, rank, actor, write != 0);
          group.Barrier(phase * 3 + 2);
          std::printf("CPU_CASE_PASS actor=%d operation=%s bytes=%zu guards=PASS\n",
                      actor, write ? "WRITE" : "READ", kBytes);
        }
      }
    }
    group.Barrier(100);
    memory.CloseImporter();
    group.Barrier(101);
    memory.CloseOwner();
    Check(aclrtResetDevice(device), "reset context");
    Check(aclFinalize(), "aclFinalize");
    group.Barrier(102);
    std::printf("CPU_DIRECT_RESULT rank=%d status=%s cleanup=PASS fallback=NONE\n",
                rank, supported ? "PASS" : "UNSUPPORTED");
    return supported ? 0 : 3;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "CPU_DIRECT_FAIL %s\n", error.what());
    return 1;
  }
}
