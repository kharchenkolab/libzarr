// Coarse single-thread throughput baseline over a MemoryStore: whole-array
// write and read of a 64 MiB float32 array across codec/sharding combos.
// Build Release (never under sanitizers) and run manually; the numbers live
// in docs/DESIGN.md. This is a baseline for spotting regressions and sizing
// optimizations, not a rigorous benchmark suite.
#include <libzarr/libzarr.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr std::uint64_t kSide = 4096;  // 4096 x 4096 float32 = 64 MiB
constexpr std::size_t kBytes = kSide * kSide * 4;
constexpr int kRepeats = 3;

struct Case {
  const char* name;
  std::vector<zarr::CodecSpec> codecs;
  std::vector<std::uint64_t> shards;  // empty = unsharded
};

std::vector<float> make_data() {
  std::vector<float> data(kSide * kSide);
  std::uint32_t lcg = 12345;
  for (std::size_t i = 0; i < data.size(); ++i) {
    lcg = lcg * 1664525U + 1013904223U;
    // Smooth ramp plus low-amplitude noise: compressible but not trivial.
    data[i] = static_cast<float>(i % kSide) + static_cast<float>(lcg >> 24U) * 0.01F;
  }
  return data;
}

double best_seconds(const std::vector<double>& runs) {
  double best = runs[0];
  for (const double r : runs) {
    best = r < best ? r : best;
  }
  return best;
}

void run_case(const Case& c, const std::vector<float>& data) {
  std::vector<double> write_s;
  std::vector<double> read_s;
  std::uint64_t stored = 0;

  for (int rep = 0; rep < kRepeats; ++rep) {
    auto store = std::make_shared<zarr::MemoryStore>();
    zarr::ArraySpec spec;
    spec.format = zarr::ZarrFormat::v3;
    spec.shape = {kSide, kSide};
    spec.chunks = {256, 256};
    spec.shards = c.shards;
    spec.dtype = zarr::DataType::of(zarr::DType::float32);
    spec.codecs = c.codecs;
    auto array = zarr::Array::create(store, "a", spec);

    const auto w0 = std::chrono::steady_clock::now();
    array.write(data.data(), kBytes);
    const auto w1 = std::chrono::steady_clock::now();
    write_s.push_back(std::chrono::duration<double>(w1 - w0).count());

    std::vector<float> out(data.size());
    auto reopened = zarr::Array::open(store, "a");
    const auto r0 = std::chrono::steady_clock::now();
    reopened.read(out.data(), kBytes);
    const auto r1 = std::chrono::steady_clock::now();
    read_s.push_back(std::chrono::duration<double>(r1 - r0).count());
    if (out[12345] != data[12345]) {
      std::fprintf(stderr, "verification failed for %s\n", c.name);
      std::exit(1);
    }

    if (rep == 0) {
      for (const auto& key : store->list_prefix("a/")) {
        if (const auto size = store->size(key)) {
          stored += *size;
        }
      }
    }
  }

  const double mib = static_cast<double>(kBytes) / (1024.0 * 1024.0);
  std::printf("%-22s %8.0f %8.0f %7.2f\n", c.name, mib / best_seconds(write_s),
              mib / best_seconds(read_s),
              static_cast<double>(stored) / static_cast<double>(kBytes));
}

}  // namespace

int main() try {
  const auto data = make_data();
  std::printf("%-22s %8s %8s %7s   (64 MiB float32, chunks 256x256%s)\n", "case", "wr MiB/s",
              "rd MiB/s", "ratio", ", shards 1024x1024 where sharded");

  const std::vector<Case> cases = {
      {"raw", {}, {}},
      {"crc32c", {{"crc32c", {}}}, {}},
#ifdef LIBZARR_HAS_ZLIB
      {"gzip-1", {zarr::gzip(1)}, {}},
      {"gzip-5", {zarr::gzip(5)}, {}},
#endif
#ifdef LIBZARR_HAS_ZSTD
      {"zstd-0", {zarr::zstd(0)}, {}},
#endif
#ifdef LIBZARR_HAS_BLOSC
      {"blosc-lz4", {zarr::blosc()}, {}},
#endif
      {"sharded raw", {}, {1024, 1024}},
#ifdef LIBZARR_HAS_ZSTD
      {"sharded zstd-0", {zarr::zstd(0)}, {1024, 1024}},
#endif
  };
  for (const Case& c : cases) {
    run_case(c, data);
  }
  return 0;
} catch (const std::exception& e) {
  std::fprintf(stderr, "error: %s\n", e.what());
  return 1;
}
