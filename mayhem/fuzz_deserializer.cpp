// Fuzz target: one serialized Nimble batch, decoded through nimble::Deserializer.
//
// The serializer format (dwio/nimble/serializer/) is Nimble's wire format for shipping row batches
// between processes: a version header, per-stream encoded data and a trailer. It does not carry
// its schema - the receiver supplies it - so the harness deserializes against one fixed schema
// (kSchema below; the committed seeds were serialized with it). The leading version byte is part of
// the input, so every wire version, the legacy ones included, is reachable.
//
// State kept across inputs: the Velox memory manager and its root pool (fuzz_common.h) and the
// immutable schema objects.
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <string_view>

#include "dwio/nimble/serializer/Deserializer.h"
#include "dwio/nimble/velox/SchemaUtils.h"
#include "fuzz_common.h"
#include "fuzz_deserializer_schema.h"
#include "velox/vector/BaseVector.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace facebook;
  static const auto schema =
      nimble::convertToNimbleType(*nimble_fuzz::deserializerRowType());
  auto pool = nimble_fuzz::newLeafPool();
  try {
    nimble::Deserializer deserializer(
        schema, pool.get(), nimble::DeserializerOptions{.hasHeader = true});
    velox::VectorPtr output;
    deserializer.deserialize(
        std::string_view(reinterpret_cast<const char*>(data), size), output);
    if (output != nullptr) {
      (void)output->size();
    }
  } catch (const std::exception&) {
    return 0;  // a rejected input still counts for coverage
  }
  return 0;
}
