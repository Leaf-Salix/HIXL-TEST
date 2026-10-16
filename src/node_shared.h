// Included inside matrix.cpp's anonymous namespace; all windows retain their actual allocation owner.
struct SharedWindow {
  int owner;
  int kind;
  int View() const { return owner * 2 + kind; }
};

std::vector<int> gCpuOwners;
std::vector<SharedWindow> gSharedWindows;
constexpr size_t kSharedSource = 256;
constexpr size_t kSharedTarget = 1024 * 1024;
constexpr size_t kSharedReadback = 1536 * 1024;
constexpr size_t kSharedConcurrentBase = 128 * 1024;
constexpr size_t kSharedConcurrentStride = 64 * 1024;

void PrepareSharedModel(Endpoint &ep, int rank) {
  if (gWorld > 16) throw std::runtime_error("node-shared fixed 2 MiB slot layout supports at most 16 NPU ranks");
  gNodeOwners = gGroup->AllGather(gNodeOwner);
  auto identities = gGroup->AllGather(ep.peer_);
  for (int peer = 0; peer < gWorld; ++peer) {
    int owner = gNodeOwners[peer];
    if (owner < 0 || owner > peer || gNodeOwners[owner] != owner ||
        std::strcmp(identities[peer].hostname, identities[owner].hostname) != 0) {
      throw std::runtime_error("invalid node-owner grouping or MPI rank placement");
    }
    for (int previous = 0; previous < peer; ++previous) {
      if (identities[previous].physical == identities[peer].physical &&
          std::strcmp(identities[previous].hostname, identities[peer].hostname) == 0) {
        throw std::runtime_error("same physical NPU used by multiple ranks");
      }
    }
    if (owner == peer) {
      for (int previous : gCpuOwners) {
        if (std::strcmp(identities[previous].hostname, identities[owner].hostname) == 0) {
          throw std::runtime_error("distinct configured nodes share a hostname");
        }
      }
      gCpuOwners.push_back(owner);
    }
  }
  for (int owner : gCpuOwners) {
    gSharedWindows.push_back({owner, 0});
    for (int peer = 0; peer < gWorld; ++peer) {
      if (gNodeOwners[peer] == owner) gSharedWindows.push_back({peer, 1});
    }
  }
  std::printf("SHARED_MODEL cpu_actors=%zu npu_actors=%d windows=%zu dram_allocations=%zu hbm_allocations=%d\n",
              gCpuOwners.size(), gWorld, gSharedWindows.size(), gCpuOwners.size(), gWorld);
  std::printf("SHARED_ALLOCATION rank=%d dram_owner=%d owns_dram=%s\n",
              rank, gNodeOwner, rank == gNodeOwner ? "true" : "false");
}

void SharedBaseline(Endpoint &ep, int rank) {
  if (rank == gNodeOwner) {
    Baseline(ep, rank);
    return;
  }
  // No second DRAM window is allocated. Non-owner ranks use ordinary temporary CPU staging.
  ep.Fill(0);
  ep.VerifyWrites(0, {});
  std::vector<uint8_t> data(4096);
  for (size_t offset = 0; offset < data.size(); ++offset) data[offset] = Pattern(0, rank, 1, kSharedSource + offset);
  Check(aclrtMemcpy(static_cast<char *>(ep.buffers_[1]) + kSharedTarget, kBytes - kSharedTarget,
                    data.data(), data.size(), ACL_MEMCPY_HOST_TO_DEVICE), "non-owner baseline upload");
  ep.VerifyWrites(0, {{rank, 1, kSharedTarget, rank, 1, kSharedSource, data.size()}});
  std::printf("BASELINE_PASS rank=%d direction=CPU_STAGING_TO_HBM no_extra_dram_window=true guards=PASS\n", rank);
  Check(aclrtMemcpy(data.data(), data.size(), static_cast<char *>(ep.buffers_[1]) + kSharedTarget,
                    data.size(), ACL_MEMCPY_DEVICE_TO_HOST), "non-owner baseline download");
  for (size_t offset = 0; offset < data.size(); ++offset) {
    if (data[offset] != Pattern(0, rank, 1, kSharedSource + offset)) throw std::runtime_error("baseline staging mismatch");
  }
  std::printf("BASELINE_PASS rank=%d direction=HBM_TO_CPU_STAGING no_extra_dram_window=true guards=PASS\n", rank);
  Barrier(-10);
}

const char *CpuWindowCopy(Endpoint &ep, const std::vector<Peer> &peers, int cpu,
                          SharedWindow target, bool read, bool async, size_t bytes,
                          size_t resource_offset = kSharedSource, size_t local_offset = kSharedTarget,
                          KernelViews *acl_views = nullptr) {
  if (acl_views && !(target.owner == cpu && target.kind == 0)) {
    acl_views->HostCopy(read ? target.View() : cpu * 2, read ? cpu * 2 : target.View(), bytes,
                        read ? resource_offset : local_offset, read ? local_offset : resource_offset, async);
    return async ? "ACL_ASYNC" : "ACL_SYNC";
  }
  if (target.owner == cpu) {
    void *dram = static_cast<char *>(ep.buffers_[0]) + local_offset;
    void *resource = static_cast<char *>(ep.buffers_[target.kind]) + resource_offset;
    if (target.kind == 0) {
      if (read) std::memmove(dram, resource, bytes);
      else std::memmove(resource, dram, bytes);
      return "CPU_MEMMOVE";
    }
    Check(aclrtMemcpy(read ? dram : resource, bytes, read ? resource : dram, bytes,
                      read ? ACL_MEMCPY_DEVICE_TO_HOST : ACL_MEMCPY_HOST_TO_DEVICE), "shared CPU local copy");
    return "LOCAL_ACL";
  }
  // Transfer's offsets describe the source and destination, independently of READ/WRITE.
  if (read) ep.Transfer(peers[target.owner], 0, target.kind, true, async, bytes, resource_offset, local_offset);
  else ep.Transfer(peers[target.owner], 0, target.kind, false, async, bytes, local_offset, resource_offset);
  return "HIXL";
}

void RunSharedHost(Endpoint &ep, const std::vector<Peer> &peers, int rank, Counts &counts,
                   KernelViews *acl_views = nullptr) {
  for (size_t bytes : {4096UL, 65536UL}) {
    for (int cpu : gCpuOwners) {
      for (const auto target : gSharedWindows) {
        for (bool read : {true, false}) {
          for (bool async : {false, true}) {
            int test = acl_views ? ++counts.acl : ++counts.hixl;
            ep.Fill(test);
            Barrier(test * 4);
            const char *path = "inactive";
            if (rank == cpu) {
              path = CpuWindowCopy(ep, peers, cpu, target, read, async, bytes,
                                   read ? kSharedSource : kSharedTarget,
                                   read ? kSharedTarget : kSharedSource, acl_views);
            }
            Barrier(test * 4 + 1);
            Write write = read ? Write{cpu, 0, kSharedTarget, target.owner, target.kind, kSharedSource, bytes}
                               : Write{target.owner, target.kind, kSharedTarget, cpu, 0, kSharedSource, bytes};
            ep.VerifyWrites(test, {write});
            Barrier(test * 4 + 2);
            if (rank == cpu) {
              std::printf("HOST_CASE_PASS id=%d actor=CPU%d target=%s%d op=%s completion=%s path=%s bytes=%zu guards=PASS\n",
                          test, cpu, Endpoint::Name(target.kind), target.owner, read ? "READ" : "WRITE",
                          async ? "ASYNC_REQUEST" : "SYNC_REQUEST", path, bytes);
            }
          }
        }
      }
    }
  }
}

void RunSharedKernel(Endpoint &ep, KernelViews &views, int rank, Counts &counts) {
  for (size_t bytes : {4096UL, 65536UL}) {
    for (int actor = 0; actor < gWorld; ++actor) {
      for (const auto target : gSharedWindows) {
        for (bool read : {true, false}) {
          int test = ++counts.kernel;
          ep.Fill(test);
          Barrier(test * 4);
          if (rank == actor) {
            views.Copy(read ? target.View() : actor * 2 + 1, read ? actor * 2 + 1 : target.View(), bytes,
                       kSharedSource, kSharedTarget);
          }
          Barrier(test * 4 + 1);
          Write write = read ? Write{actor, 1, kSharedTarget, target.owner, target.kind, kSharedSource, bytes}
                             : Write{target.owner, target.kind, kSharedTarget, actor, 1, kSharedSource, bytes};
          ep.VerifyWrites(test, {write});
          Barrier(test * 4 + 2);
          if (rank == actor) {
            std::printf("NODE_KERNEL_CASE_PASS id=%d actor=NPU%d target=%s%d op=%s bytes=%zu guards=PASS\n",
                        test, actor, Endpoint::Name(target.kind), target.owner, read ? "READ" : "WRITE", bytes);
          }
        }
      }
    }
  }
}

void RunSharedConcurrent(Endpoint &ep, KernelViews &views, int rank, Counts &counts) {
  for (size_t bytes : {4096UL, 65536UL}) {
    for (int owner : gCpuOwners) {
      for (bool read : {true, false}) {
        int test = 10000 + ++counts.shared;
        ep.Fill(test);
        Barrier(test * 4);
        // All NPU processes submit independently in this round. No ordering between writers.
        const size_t target_offset = read ? kSharedTarget : kSharedConcurrentBase + rank * kSharedConcurrentStride;
        views.Copy(read ? owner * 2 : rank * 2 + 1, read ? rank * 2 + 1 : owner * 2, bytes,
                   kSharedSource, target_offset);
        Barrier(test * 4 + 1);
        std::vector<Write> writes;
        for (int actor = 0; actor < gWorld; ++actor) {
          writes.push_back(read ? Write{actor, 1, kSharedTarget, owner, 0, kSharedSource, bytes}
                                : Write{owner, 0, kSharedConcurrentBase + actor * kSharedConcurrentStride,
                                        actor, 1, kSharedSource, bytes});
        }
        ep.VerifyWrites(test, writes);
        Barrier(test * 4 + 2);
        if (rank == 0) {
          std::printf("SHARED_GROUP_PASS id=%d dram_owner=%d operation=%s npu_actors=%d bytes=%zu guards=PASS\n",
                      counts.shared, owner, read ? "SAME_RANGE_READ" : "DISJOINT_RANGE_WRITE", gWorld, bytes);
        }
      }
    }
  }
}

void RunSharedNpuHandoffs(Endpoint &ep, KernelViews &views, int rank, Counts &counts) {
  for (size_t bytes : {4096UL, 65536UL}) {
    for (int owner : gCpuOwners) {
      for (int writer = 0; writer < gWorld; ++writer) {
        for (int reader = 0; reader < gWorld; ++reader) {
          if (writer == reader) continue;
          int test = 20000 + ++counts.handoff;
          ep.Fill(test);
          Barrier(test * 4);
          if (rank == writer) views.Copy(writer * 2 + 1, owner * 2, bytes, kSharedSource, kSharedTarget);
          Barrier(test * 4 + 1);
          if (rank == reader) views.Copy(owner * 2, reader * 2 + 1, bytes, kSharedTarget, kSharedTarget);
          Barrier(test * 4 + 2);
          ep.VerifyWrites(test, {{owner, 0, kSharedTarget, writer, 1, kSharedSource, bytes},
                                 {reader, 1, kSharedTarget, writer, 1, kSharedSource, bytes}});
          Barrier(test * 4 + 3);
          if (rank == 0) {
            std::printf("NODE_HANDOFF_PASS id=%d path=NPU_DRAM_NPU writer=%d reader=%d dram_owner=%d bytes=%zu guards=PASS\n",
                        counts.handoff, writer, reader, owner, bytes);
          }
        }
      }
    }
  }
}

void RunSharedCpuHandoffs(Endpoint &ep, KernelViews &views, const std::vector<Peer> &peers,
                         int rank, Counts &counts, KernelViews *acl_views = nullptr) {
  for (size_t bytes : {4096UL, 65536UL}) {
    for (int cpu : gCpuOwners) {
      for (int npu = 0; npu < gWorld; ++npu) {
        int owner = gNodeOwners[npu];
        for (bool host_first : {true, false}) {
          int test = 20000 + ++counts.handoff;
          ep.Fill(test);
          Barrier(test * 4);
          if (host_first) {
            if (rank == cpu) CpuWindowCopy(ep, peers, cpu, {owner, 0}, false, false, bytes,
                                           kSharedTarget, kSharedSource, acl_views);
          } else if (rank == npu) {
            views.Copy(npu * 2 + 1, owner * 2, bytes, kSharedSource, kSharedTarget);
          }
          Barrier(test * 4 + 1);
          if (host_first) {
            if (rank == npu) views.Copy(owner * 2, npu * 2 + 1, bytes, kSharedTarget, kSharedTarget);
          } else if (rank == cpu) {
            CpuWindowCopy(ep, peers, cpu, {owner, 0}, true, true, bytes, kSharedTarget, kSharedReadback, acl_views);
          }
          Barrier(test * 4 + 2);
          std::vector<Write> writes;
          if (host_first) {
            writes = {{owner, 0, kSharedTarget, cpu, 0, kSharedSource, bytes},
                      {npu, 1, kSharedTarget, cpu, 0, kSharedSource, bytes}};
          } else {
            writes = {{owner, 0, kSharedTarget, npu, 1, kSharedSource, bytes},
                      {cpu, 0, kSharedReadback, npu, 1, kSharedSource, bytes}};
          }
          ep.VerifyWrites(test, writes);
          Barrier(test * 4 + 3);
          if (rank == 0) {
            std::printf("NODE_HANDOFF_PASS id=%d path=%s cpu=%d npu=%d dram_owner=%d bytes=%zu guards=PASS\n",
                        counts.handoff, host_first ? "HOST_DRAM_KERNEL" : "KERNEL_DRAM_HOST", cpu, npu, owner, bytes);
          }
        }
      }
    }
  }
}

void RunShared(Endpoint &ep, KernelViews &views, const std::vector<Peer> &peers,
               int rank, const std::string &mode, Counts &counts) {
  if (mode == "hixl" || mode == "combined") RunSharedHost(ep, peers, rank, counts);
  if (mode == "acl") RunSharedHost(ep, peers, rank, counts, &views);
  if (mode == "kernel" || mode == "combined" || mode == "acl") {
    RunSharedKernel(ep, views, rank, counts);
    RunSharedConcurrent(ep, views, rank, counts);
    RunSharedNpuHandoffs(ep, views, rank, counts);
  }
  if (mode == "combined") RunSharedCpuHandoffs(ep, views, peers, rank, counts);
  if (mode == "acl") RunSharedCpuHandoffs(ep, views, peers, rank, counts, &views);
}
