// Copyright (C) 2026 Meritium contributors.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#pragma once

#include <category/core/byte_string.hpp>
#include <category/core/config.hpp>
#include <category/core/result.hpp>
#include <category/execution/ethereum/agent_state.hpp>

// RLP codec for Meritium AgentRecord. Used for MPT value serialization in the
// Agent state partition (AGENT_NIBBLE = 10).
//
// Wire format (RLP list of 4 items):
//   [ agent_id (32 bytes),
//     wallet   (20 bytes),
//     tier     (uint, 0..4),
//     reputation (uint, 0..65535) ]
//
// Note that `tier` is encoded as the enum's underlying uint8_t value.

MONAD_NAMESPACE_BEGIN
namespace rlp {

// Encode a single AgentRecord as an RLP list. Stable byte layout — used as
// the leaf value under the AGENT_NIBBLE subtrie.
byte_string encode_agent_record(meritium::AgentRecord const &record);

// Inverse of encode_agent_record. Consumes the encoded bytes from `enc`.
Result<meritium::AgentRecord> decode_agent_record(byte_string_view &enc);

} // namespace rlp
MONAD_NAMESPACE_END
