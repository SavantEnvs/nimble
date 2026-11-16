// Fuzz target: one encoded Nimble stream, decoded through nimble::EncodingFactory.
//
// The input is the serialized form of a single encoding (prefix: encoding type, data type, row
// count; then the encoding's own, possibly nested, payload) - what a stream chunk in a Nimble file
// or a serialized batch holds. EncodingFactory::create() is the entry point every reader uses to
// turn those bytes into an Encoding; the harness then materializes rows from it.
//
// State kept across inputs: the Velox memory manager and its root pool only (fuzz_common.h).
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string_view>
#include <vector>

#include "dwio/nimble/encodings/common/Encoding.h"
#include "dwio/nimble/encodings/common/EncodingFactory.h"
#include "dwio/nimble/encodings/common/EncodingPrefix.h"
#include "fuzz_common.h"
#include "velox/buffer/Buffer.h"

// The factory that parses the stream. fuzz_encoding_legacy.cpp reuses this file with the legacy
// factory (dwio/nimble/encodings/legacy), which has its own decoder implementations.
#ifndef NIMBLE_FUZZ_ENCODING_FACTORY
#define NIMBLE_FUZZ_ENCODING_FACTORY ::facebook::nimble::EncodingFactory
#endif

namespace {
// Rows materialized per input. The row count is a field of the input and a constant or run-length
// encoding can legitimately describe 2^32 rows in a few bytes, so the harness reads a bounded
// prefix of them, as a reader paging through a chunk does.
constexpr uint32_t kMaxRows = 1 << 16;
// The widest physical value materialize() writes is a std::string_view.
constexpr size_t kMaxValueSize = sizeof(std::string_view);
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace facebook;
  // Both factories read the fixed 6-byte encoding prefix (encoding type, data type, row count)
  // without a length check: their callers hand them whole stream chunks, never fewer bytes than
  // the prefix. Shorter inputs would only re-find that out-of-bounds read on every run.
  if (size < static_cast<size_t>(facebook::nimble::EncodingPrefix::kFixedPrefixSize)) {
    return 0;
  }
  auto pool = nimble_fuzz::newLeafPool();
  try {
    // String encodings ask for buffers that must outlive the encoding; same shape as the
    // library's own callers (dwio/nimble/velox/selective/ChunkedDecoder.cpp).
    std::vector<velox::BufferPtr> stringBuffers;
    auto stringBufferFactory = [&](uint32_t totalLength) {
      auto& buffer = stringBuffers.emplace_back(
          velox::AlignedBuffer::allocate<char>(totalLength, pool.get()));
      return buffer->asMutable<void>();
    };

    auto encoding = NIMBLE_FUZZ_ENCODING_FACTORY().create(
        *pool,
        std::string_view(reinterpret_cast<const char*>(data), size),
        stringBufferFactory);

    const uint32_t rows = std::min(encoding->rowCount(), kMaxRows);
    std::vector<uint64_t> values((rows * kMaxValueSize + 7) / 8 + 1);

    // Pass 1: everything in one call.
    encoding->materialize(rows, values.data());

    // Pass 2: rewind, skip the first half, read the rest.
    encoding->reset();
    encoding->skip(rows / 2);
    encoding->materialize(rows - rows / 2, values.data());

    // Pass 3: rewind and read in small steps, which crosses the encodings' internal block and
    // run boundaries at different offsets.
    encoding->reset();
    for (uint32_t done = 0; done < rows;) {
      const uint32_t step = std::min<uint32_t>(rows - done, 7);
      encoding->materialize(step, values.data());
      done += step;
    }
  } catch (const std::exception&) {
    return 0;  // a rejected input still counts for coverage
  }
  return 0;
}
