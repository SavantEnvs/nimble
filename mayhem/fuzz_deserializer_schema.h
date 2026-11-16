// The schema fuzz_deserializer decodes against, shared with gen_seeds so the seeds match it.
#pragma once

#include "velox/type/Type.h"

namespace nimble_fuzz {

inline facebook::velox::RowTypePtr deserializerRowType() {
  using namespace facebook::velox;
  return ROW(
      {"id", "flag", "score", "name", "tags", "attrs", "nested"},
      {BIGINT(),
       BOOLEAN(),
       DOUBLE(),
       VARCHAR(),
       ARRAY(INTEGER()),
       MAP(VARCHAR(), REAL()),
       ROW({"x", "y"}, {SMALLINT(), VARCHAR()})});
}

} // namespace nimble_fuzz
