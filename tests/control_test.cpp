#include "control.h"
#include "pattern.h"
#include <csignal>
#include <cstdio>

int main(int argc, char **argv) {
  signal(SIGPIPE, SIG_IGN);
  try {
    if (argc == 2 && std::string(argv[1]) == "pattern") {
      for (int rank = 0; rank < 64; ++rank) {
        for (int kind = 0; kind < 2; ++kind) {
          bool distinct = false;
          for (size_t offset = 0; offset < 4096; ++offset) {
            distinct |= Pattern(1, rank, kind, 256 + offset) != Pattern(1, 0, 1, 512 + offset);
          }
          if (!distinct) throw std::runtime_error("window identity/offset alias");
        }
      }
      return 0;
    }
    if (argc != 5) return 2;
    int rank = std::stoi(argv[1]);
    control::Group group(rank, 3, argv[2], 2);
    std::string mode = argv[3];
    for (int stage = 0; stage < 10; ++stage) {
      if (mode == "exit" && rank == 2 && stage == 2) return 1;
      struct Record { int rank; int stage; char label[32]; };
      Record record{rank, stage, "metadata-only"};
      auto gathered = group.AllGather(record);
      for (int peer = 0; peer < 3; ++peer) {
        if (gathered[peer].rank != peer || gathered[peer].stage != stage ||
            std::string(gathered[peer].label) != "metadata-only") throw std::runtime_error("gather corruption");
      }
      group.Barrier(stage + (mode == "mismatch" && rank == 2 ? 1 : 0));
    }
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
