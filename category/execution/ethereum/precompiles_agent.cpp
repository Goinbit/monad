// Copyright (C) 2026 Meritium contributors.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#include <category/execution/ethereum/precompiles_agent.hpp>

#include <category/core/assert.h>
#include <category/core/byte_string.hpp>
#include <category/execution/ethereum/agent_state.hpp>
#include <category/execution/ethereum/core/address.hpp>

#include <cstdint>
#include <cstdlib>
#include <cstring>

MONAD_NAMESPACE_BEGIN
namespace meritium {

namespace {

// Big-endian read of the 4-byte selector. Caller guarantees size >= 4.
constexpr uint32_t read_selector(byte_string_view const input) noexcept
{
    return (static_cast<uint32_t>(input[0]) << 24) |
           (static_cast<uint32_t>(input[1]) << 16) |
           (static_cast<uint32_t>(input[2]) << 8) |
           static_cast<uint32_t>(input[3]);
}

// Read an ABI-encoded bytes32 starting at `offset`. Caller guarantees input
// has at least `offset + 32` bytes.
bytes32_t read_bytes32_abi(byte_string_view const input, size_t const offset)
{
    bytes32_t out;
    std::memcpy(out.bytes, input.data() + offset, 32);
    return out;
}

// Read an ABI-encoded address starting at `offset`. Address is stored in the
// last 20 bytes of a 32-byte ABI word (left-padded with 12 zero bytes).
Address read_address_abi(byte_string_view const input, size_t const offset)
{
    Address out;
    std::memcpy(out.bytes, input.data() + offset + 12, 20);
    return out;
}

// Allocate a 32-byte ABI word. Caller (EVM host) frees.
uint8_t *alloc_word() noexcept
{
    auto *const buf = static_cast<uint8_t *>(std::malloc(ABI_WORD));
    if (buf != nullptr) {
        std::memset(buf, 0, ABI_WORD);
    }
    return buf;
}

PrecompileResult make_zero_result() noexcept
{
    auto *const buf = alloc_word();
    if (MONAD_UNLIKELY(buf == nullptr)) {
        return PrecompileResult::failure();
    }
    return {EVMC_SUCCESS, buf, ABI_WORD};
}

// Encode a bool as a 32-byte ABI word.
PrecompileResult make_bool_result(bool const value) noexcept
{
    auto *const buf = alloc_word();
    if (MONAD_UNLIKELY(buf == nullptr)) {
        return PrecompileResult::failure();
    }
    if (value) {
        buf[31] = 0x01;
    }
    return {EVMC_SUCCESS, buf, ABI_WORD};
}

// Encode a bytes32 as a 32-byte ABI word (identity encoding).
PrecompileResult make_bytes32_result(bytes32_t const &value) noexcept
{
    auto *const buf = alloc_word();
    if (MONAD_UNLIKELY(buf == nullptr)) {
        return PrecompileResult::failure();
    }
    std::memcpy(buf, value.bytes, 32);
    return {EVMC_SUCCESS, buf, ABI_WORD};
}

// Encode a 20-byte address as a 32-byte ABI word (left-padded with zeros).
PrecompileResult make_address_result(Address const &value) noexcept
{
    auto *const buf = alloc_word();
    if (MONAD_UNLIKELY(buf == nullptr)) {
        return PrecompileResult::failure();
    }
    std::memcpy(buf + 12, value.bytes, 20);
    return {EVMC_SUCCESS, buf, ABI_WORD};
}

// Encode a small integer as a 32-byte ABI word (right-aligned big-endian).
// Template accepts uint8_t / uint16_t / uint64_t etc.
template <typename Int>
PrecompileResult make_uint_result(Int const value) noexcept
{
    auto *const buf = alloc_word();
    if (MONAD_UNLIKELY(buf == nullptr)) {
        return PrecompileResult::failure();
    }
    static_assert(sizeof(Int) <= ABI_WORD);
    for (size_t i = 0; i < sizeof(Int); ++i) {
        buf[ABI_WORD - 1 - i] =
            static_cast<uint8_t>((value >> (8 * i)) & 0xff);
    }
    return {EVMC_SUCCESS, buf, ABI_WORD};
}

} // namespace

uint64_t agent_gas_cost(byte_string_view const input)
{
    if (input.size() < SELECTOR_BYTES) {
        return AGENT_INVALID_SELECTOR_GAS;
    }
    switch (read_selector(input)) {
        case SELECTOR_VERIFY_AGENT:
            return AGENT_VERIFY_GAS;
        case SELECTOR_GET_AGENT_ID:
        case SELECTOR_GET_WALLET:
            return AGENT_LOOKUP_GAS;
        case SELECTOR_IS_REGISTERED:
            return AGENT_IS_REGISTERED_GAS;
        case SELECTOR_GET_AGENT_TIER:
            return AGENT_TIER_GAS;
        case SELECTOR_GET_REPUTATION:
            return AGENT_REPUTATION_GAS;
        default:
            return AGENT_INVALID_SELECTOR_GAS;
    }
}

PrecompileResult agent_execute(byte_string_view const input)
{
    if (input.size() < SELECTOR_BYTES) {
        return PrecompileResult::failure();
    }

    uint32_t const selector = read_selector(input);
    auto const &table = AgentStateTable::instance();

    switch (selector) {
        case SELECTOR_VERIFY_AGENT: {
            if (input.size() < INPUT_VERIFY_AGENT) {
                return PrecompileResult::failure();
            }
            auto const agent_id = read_bytes32_abi(input, SELECTOR_BYTES);
            auto const wallet =
                read_address_abi(input, SELECTOR_BYTES + ABI_WORD);
            return make_bool_result(table.verify_binding(agent_id, wallet));
        }

        case SELECTOR_GET_AGENT_ID: {
            if (input.size() < INPUT_SINGLE_ARG) {
                return PrecompileResult::failure();
            }
            auto const wallet = read_address_abi(input, SELECTOR_BYTES);
            if (auto const rec = table.find_by_wallet(wallet); rec) {
                return make_bytes32_result(rec->agent_id);
            }
            return make_zero_result();
        }

        case SELECTOR_GET_WALLET: {
            if (input.size() < INPUT_SINGLE_ARG) {
                return PrecompileResult::failure();
            }
            auto const agent_id = read_bytes32_abi(input, SELECTOR_BYTES);
            if (auto const rec = table.find_by_id(agent_id); rec) {
                return make_address_result(rec->wallet);
            }
            return make_zero_result();
        }

        case SELECTOR_IS_REGISTERED: {
            if (input.size() < INPUT_SINGLE_ARG) {
                return PrecompileResult::failure();
            }
            auto const agent_id = read_bytes32_abi(input, SELECTOR_BYTES);
            return make_bool_result(table.find_by_id(agent_id).has_value());
        }

        case SELECTOR_GET_AGENT_TIER: {
            if (input.size() < INPUT_SINGLE_ARG) {
                return PrecompileResult::failure();
            }
            auto const agent_id = read_bytes32_abi(input, SELECTOR_BYTES);
            auto const rec = table.find_by_id(agent_id);
            return make_uint_result<uint8_t>(
                rec ? static_cast<uint8_t>(rec->tier) : 0);
        }

        case SELECTOR_GET_REPUTATION: {
            if (input.size() < INPUT_SINGLE_ARG) {
                return PrecompileResult::failure();
            }
            auto const agent_id = read_bytes32_abi(input, SELECTOR_BYTES);
            auto const rec = table.find_by_id(agent_id);
            return make_uint_result<uint16_t>(rec ? rec->reputation : 0);
        }

        default:
            return PrecompileResult::failure();
    }
}

} // namespace meritium
MONAD_NAMESPACE_END
