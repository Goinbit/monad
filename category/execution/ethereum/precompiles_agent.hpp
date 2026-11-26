// Copyright (C) 2026 Meritium contributors.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#pragma once

#include <category/core/byte_string.hpp>
#include <category/core/config.hpp>
#include <category/execution/ethereum/precompiles.hpp>

#include <cstdint>

// Meritium Agent precompile — deployed at address 0x0101.
//
// Exposes six selector-dispatched functions for Agent identity / reputation
// lookup (see chain/precompile/agent_precompile.rs for the Rust mirror):
//
//   0xd7f64027  verifyAgent(bytes32 agentId, address wallet) -> bool
//   0x042324b6  getAgentId(address wallet)                    -> bytes32
//   0x789d392a  getWallet(bytes32 agentId)                    -> address
//   0x27258b22  isRegistered(bytes32 agentId)                 -> bool
//   0xf31ec6c1  getAgentTier(bytes32 agentId)                 -> uint8
//   0xd14519d2  getReputation(bytes32 agentId)                -> uint16
//
// (values above are Keccak256(signature)[0:4]; test_agent_precompile.cpp
// recomputes them so a hand-typed constant cannot survive review.)
//
// Address selection — 0x0101 is authoritative (protocol manifest, Rust
// `chain/precompile/mod.rs`, and the SDK all encode it): 0x0b–0x11 are
// Monad's PRAGUE BLS12-381 precompiles and 0x0100 is P256_VERIFY (EIP-7951).
// Test fixtures keep a separate ADDR_B_TRIE_FIXTURE at 0x0101 to preserve
// historical state roots; executable-contract fixtures use 0x0201. A
// 2026-05-21 commit sidestepped this by moving the precompile itself to
// 0x0201, silently diverging C++ from every other layer.
//
// STATUS: reads the live AgentStateTable (M5); unregistered agents resolve
// to the zero/default word and gas is still metered.

MONAD_NAMESPACE_BEGIN

namespace meritium {

inline constexpr uint64_t AGENT_PRECOMPILE_ADDRESS = 0x0101;

// Gas costs — mirror of `chain/precompile/gas.rs`.
inline constexpr uint64_t AGENT_VERIFY_GAS         = 3'000;
inline constexpr uint64_t AGENT_LOOKUP_GAS         = 2'600;
inline constexpr uint64_t AGENT_TIER_GAS           = 2'600;
inline constexpr uint64_t AGENT_REPUTATION_GAS     = 2'600;
inline constexpr uint64_t AGENT_IS_REGISTERED_GAS  = 2'600;
inline constexpr uint64_t AGENT_INVALID_SELECTOR_GAS = 500;

// 4-byte function selectors (big-endian) — mirror of
// `chain/precompile/agent_precompile.rs:selectors`.
inline constexpr uint32_t SELECTOR_VERIFY_AGENT    = 0xd7f64027;
inline constexpr uint32_t SELECTOR_GET_AGENT_ID    = 0x042324b6;
inline constexpr uint32_t SELECTOR_GET_WALLET      = 0x789d392a;
inline constexpr uint32_t SELECTOR_IS_REGISTERED   = 0x27258b22;
inline constexpr uint32_t SELECTOR_GET_AGENT_TIER  = 0xf31ec6c1;
inline constexpr uint32_t SELECTOR_GET_REPUTATION  = 0xd14519d2;

// Minimum input sizes per selector (4-byte selector + ABI-encoded args).
inline constexpr size_t SELECTOR_BYTES         = 4;
inline constexpr size_t ABI_WORD               = 32;
inline constexpr size_t INPUT_VERIFY_AGENT     = SELECTOR_BYTES + 2 * ABI_WORD; // bytes32 + address
inline constexpr size_t INPUT_SINGLE_ARG       = SELECTOR_BYTES + ABI_WORD;     // remaining five

// Compute the gas cost for the call based on the leading 4-byte selector.
// Returns the invalid-selector cost when input is too short or the selector
// is unrecognized. Gas metering therefore always proceeds even when the
// actual execution fails — consistent with Monad's existing precompiles.
uint64_t agent_gas_cost(byte_string_view input);

// Dispatch on the 4-byte selector and return an ABI-encoded 32-byte result.
// Ownership note: on success the returned `obuf` is `malloc`-allocated and
// freed by the EVM host, matching the convention used by `identity_execute`.
PrecompileResult agent_execute(byte_string_view input);

} // namespace meritium

MONAD_NAMESPACE_END
