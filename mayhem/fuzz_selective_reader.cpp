// Fuzz target: a whole Nimble file, read through the selective reader
// (nimble::SelectiveNimbleReaderFactory), the Velox dwio::common::Reader implementation query
// engines use to scan Nimble files. It shares the tablet layer with VeloxReader but has its own
// column readers and chunk decoders (dwio/nimble/velox/selective/).
//
// State kept across inputs: the Velox memory manager and its root pool (fuzz_common.h), and the
// reader-factory registration, which is done once and never changed by an input.
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <string_view>

#include "dwio/nimble/velox/selective/SelectiveNimbleReader.h"
#include "fuzz_common.h"
#include "velox/common/file/File.h"
#include "velox/dwio/common/BufferedInput.h"
#include "velox/dwio/common/Options.h"
#include "velox/dwio/common/Reader.h"
#include "velox/dwio/common/ReaderFactory.h"
#include "velox/dwio/common/ScanSpec.h"
#include "velox/vector/BaseVector.h"
#include "velox/vector/ComplexVector.h"

namespace {
// See fuzz_velox_reader.cpp: a bounded prefix of the rows the file declares.
constexpr uint64_t kBatchRows = 1024;
constexpr uint64_t kMaxRows = 64 * kBatchRows;
} // namespace

extern "C" int LLVMFuzzerInitialize(int*, char***) {
  (void)nimble_fuzz::rootPool();
  facebook::nimble::registerSelectiveNimbleReaderFactory();
  return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace facebook;
  auto pool = nimble_fuzz::newLeafPool();
  try {
    auto readFile = std::make_shared<velox::InMemoryReadFile>(
        std::string_view(reinterpret_cast<const char*>(data), size));
    auto factory = velox::dwio::common::getReaderFactory(
        velox::dwio::common::FileFormat::NIMBLE);

    velox::dwio::common::ReaderOptions options(pool.get());
    auto reader = factory->createReader(
        std::make_unique<velox::dwio::common::BufferedInput>(readFile, *pool),
        options);

    // Project every column of the file's own schema, with no filters.
    const auto rowType = reader->rowType();
    auto scanSpec = std::make_shared<velox::common::ScanSpec>("root");
    scanSpec->addAllChildFields(*rowType);
    velox::dwio::common::RowReaderOptions rowOptions;
    rowOptions.setScanSpec(scanSpec);
    rowOptions.setRequestedType(rowType);
    auto rowReader = reader->createRowReader(rowOptions);

    velox::VectorPtr result = velox::BaseVector::create(rowType, 0, pool.get());
    uint64_t rows = 0;
    while (rows < kMaxRows) {
      const auto scanned = rowReader->next(kBatchRows, result);
      if (scanned == 0) {
        break;
      }
      rows += scanned;
      // Columns may come back lazy; load them so the column readers actually decode.
      velox::BaseVector::loadedVectorShared(result);
    }
  } catch (const std::exception&) {
    return 0;  // a rejected input still counts for coverage
  }
  return 0;
}
