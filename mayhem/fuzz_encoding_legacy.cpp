// Fuzz target: one encoded Nimble stream, decoded through nimble::legacy::EncodingFactory.
//
// Same harness as fuzz_encoding.cpp, but the stream is parsed by the legacy encoding factory, which
// Nimble keeps for files written by older writers and which has its own implementations of the
// Trivial, RLE, SparseBool, Prefix, Dictionary, Nullable, Sentinel, ... decoders
// (dwio/nimble/encodings/legacy/).
//
// State kept across inputs: the Velox memory manager and its root pool only (fuzz_common.h).
#include "dwio/nimble/encodings/legacy/EncodingFactory.h"

#define NIMBLE_FUZZ_ENCODING_FACTORY ::facebook::nimble::legacy::EncodingFactory
#include "fuzz_encoding.cpp"
