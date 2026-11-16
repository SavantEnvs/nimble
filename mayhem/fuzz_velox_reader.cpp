// Fuzz target: a whole Nimble file, read through nimble::VeloxReader.
//
// The input is the bytes of a .nimble file. VeloxReader is the library's own entry point for
// reading one: it opens the tablet (postscript, footer, stripe groups, optional sections), rebuilds
// the schema, then decodes every stream of every stripe into Velox vectors.
//
// State kept across inputs: the Velox memory manager and its root pool only (fuzz_common.h).
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string_view>

#include "dwio/nimble/velox/VeloxReader.h"
#include "fuzz_common.h"
#include "velox/common/file/File.h"

namespace {
// Rows asked for per next() call, and in total. A few bytes of input can legitimately describe
// billions of rows (constant / run-length streams), so the harness reads a bounded prefix, the way
// a caller paging through a file does. Nothing about the input is inspected before the library runs.
constexpr uint64_t kBatchRows = 1024;
constexpr uint64_t kMaxRows = 64 * kBatchRows;
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace facebook;
  auto pool = nimble_fuzz::newLeafPool();
  try {
    velox::InMemoryReadFile file(
        std::string_view(reinterpret_cast<const char*>(data), size));
    nimble::VeloxReader reader(&file, *pool);
    (void)reader.metadata();

    velox::VectorPtr result;
    uint64_t rows = 0;
    while (rows < kMaxRows && reader.next(kBatchRows, result)) {
      rows += result->size();
    }

    // Seek back and re-read after a skip: a second pass through the stripe loader and the
    // decoders' skip paths.
    reader.seekToRow(0);
    reader.skipRows(rows / 2);
    if (reader.next(kBatchRows, result)) {
      (void)result->size();
    }
  } catch (const std::exception&) {
    // The library refused the file (NimbleException / VeloxException): not an interesting input.
    return 0;  // a rejected input still counts for coverage
  }
  return 0;
}
