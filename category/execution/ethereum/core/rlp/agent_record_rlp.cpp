// Copyright (C) 2026 Meritium contributors.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#include <category/execution/ethereum/core/rlp/agent_record_rlp.hpp>

#include <category/core/byte_string.hpp>
#include <category/execution/ethereum/core/rlp/address_rlp.hpp>
#include <category/execution/ethereum/core/rlp/bytes_rlp.hpp>
#include <category/execution/ethereum/core/rlp/int_rlp.hpp>
#include <category/execution/ethereum/rlp/decode.hpp>
#include <category/execution/ethereum/rlp/decode_error.hpp>
#include <category/execution/ethereum/rlp/encode2.hpp>

#include <boost/outcome/try.hpp>

#include <cstdint>

MONAD_NAMESPACE_BEGIN
namespace rlp {

byte_string encode_agent_record(meritium::AgentRecord const &record)
{
    byte_string body;
    body += encode_bytes32(record.agent_id);
    // encode_address takes std::optional<Address> — wrap here.
    body += encode_address(std::optional<Address>{record.wallet});
    body += encode_unsigned(static_cast<uint64_t>(record.tier));
    body += encode_unsigned(static_cast<uint64_t>(record.reputation));
    return encode_list2(body);
}

Result<meritium::AgentRecord> decode_agent_record(byte_string_view &enc)
{
    BOOST_OUTCOME_TRY(auto payload, parse_list_metadata(enc));

    meritium::AgentRecord record;
    BOOST_OUTCOME_TRY(record.agent_id, decode_bytes32(payload));
    BOOST_OUTCOME_TRY(record.wallet, decode_address(payload));

    BOOST_OUTCOME_TRY(auto const tier_raw, decode_unsigned<uint64_t>(payload));
    if (tier_raw > 4u) {
        return DecodeError::Overflow;
    }
    record.tier = static_cast<meritium::AgentTier>(tier_raw);

    BOOST_OUTCOME_TRY(
        auto const reputation_raw, decode_unsigned<uint64_t>(payload));
    if (reputation_raw > 0xFFFFu) {
        return DecodeError::Overflow;
    }
    record.reputation = static_cast<uint16_t>(reputation_raw);

    if (!payload.empty()) {
        return DecodeError::InputTooLong;
    }
    return record;
}

} // namespace rlp
MONAD_NAMESPACE_END
