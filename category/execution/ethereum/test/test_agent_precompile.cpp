// Copyright (C) 2026 Meritium contributors.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// Tests for the Meritium Agent precompile at address 0x0201.
// Covers:
//   - selector-based gas metering
//   - selector-based dispatch in agent_execute
//   - input length validation
//   - the M4 stub contract (all lookups -> 32-byte zero word)
//
// When M5 (Agent State Partition) lands, replace the "returns zero" checks
// with real fixtures populated via TrieDB FFI.

#include <category/core/byte_string.hpp>
#include <category/execution/ethereum/agent_state.hpp>
#include <category/execution/ethereum/precompiles.hpp>
#include <category/execution/ethereum/precompiles_agent.hpp>

#include <evmc/evmc.h>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>

using namespace monad;
using monad::meritium::AgentStateTable;
using monad::meritium::AgentTier;
using monad::meritium::ABI_WORD;
using monad::meritium::AGENT_INVALID_SELECTOR_GAS;
using monad::meritium::AGENT_IS_REGISTERED_GAS;
using monad::meritium::AGENT_LOOKUP_GAS;
using monad::meritium::AGENT_REPUTATION_GAS;
using monad::meritium::AGENT_TIER_GAS;
using monad::meritium::AGENT_VERIFY_GAS;
using monad::meritium::INPUT_SINGLE_ARG;
using monad::meritium::INPUT_VERIFY_AGENT;
using monad::meritium::SELECTOR_BYTES;
using monad::meritium::SELECTOR_GET_AGENT_ID;
using monad::meritium::SELECTOR_GET_AGENT_TIER;
using monad::meritium::SELECTOR_GET_REPUTATION;
using monad::meritium::SELECTOR_GET_WALLET;
using monad::meritium::SELECTOR_IS_REGISTERED;
using monad::meritium::SELECTOR_VERIFY_AGENT;

namespace {

// Build an input buffer: 4-byte selector (big-endian) + `payload_bytes` of
// trailing ABI-encoded argument data (zero-filled).
byte_string make_input(uint32_t const selector, size_t const payload_bytes = 0)
{
    byte_string out;
    out.reserve(SELECTOR_BYTES + payload_bytes);
    out.push_back(static_cast<uint8_t>((selector >> 24) & 0xff));
    out.push_back(static_cast<uint8_t>((selector >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((selector >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>(selector & 0xff));
    out.append(payload_bytes, uint8_t{0});
    return out;
}

// Release any allocated output buffer attached to a PrecompileResult.
void free_output(PrecompileResult const &r) noexcept
{
    if (r.obuf != nullptr) {
        std::free(r.obuf);
    }
}

} // namespace

// ============================================================================
// Gas cost metering
// ============================================================================

TEST(AgentPrecompile, GasVerifyAgent)
{
    auto const input = make_input(SELECTOR_VERIFY_AGENT, 2 * ABI_WORD);
    EXPECT_EQ(monad::meritium::agent_gas_cost(input), AGENT_VERIFY_GAS);
}

TEST(AgentPrecompile, GasGetAgentId)
{
    auto const input = make_input(SELECTOR_GET_AGENT_ID, ABI_WORD);
    EXPECT_EQ(monad::meritium::agent_gas_cost(input), AGENT_LOOKUP_GAS);
}

TEST(AgentPrecompile, GasGetWallet)
{
    auto const input = make_input(SELECTOR_GET_WALLET, ABI_WORD);
    EXPECT_EQ(monad::meritium::agent_gas_cost(input), AGENT_LOOKUP_GAS);
}

TEST(AgentPrecompile, GasIsRegistered)
{
    auto const input = make_input(SELECTOR_IS_REGISTERED, ABI_WORD);
    EXPECT_EQ(monad::meritium::agent_gas_cost(input), AGENT_IS_REGISTERED_GAS);
}

TEST(AgentPrecompile, GasGetAgentTier)
{
    auto const input = make_input(SELECTOR_GET_AGENT_TIER, ABI_WORD);
    EXPECT_EQ(monad::meritium::agent_gas_cost(input), AGENT_TIER_GAS);
}

TEST(AgentPrecompile, GasGetReputation)
{
    auto const input = make_input(SELECTOR_GET_REPUTATION, ABI_WORD);
    EXPECT_EQ(monad::meritium::agent_gas_cost(input), AGENT_REPUTATION_GAS);
}

TEST(AgentPrecompile, GasInvalidSelector)
{
    auto const input = make_input(0xDEADBEEF, ABI_WORD);
    EXPECT_EQ(
        monad::meritium::agent_gas_cost(input), AGENT_INVALID_SELECTOR_GAS);
}

TEST(AgentPrecompile, GasEmptyInput)
{
    byte_string const empty;
    EXPECT_EQ(
        monad::meritium::agent_gas_cost(empty), AGENT_INVALID_SELECTOR_GAS);
}

TEST(AgentPrecompile, GasSelectorTooShort)
{
    byte_string const three_bytes{0x85, 0x50, 0x4c};
    EXPECT_EQ(
        monad::meritium::agent_gas_cost(three_bytes),
        AGENT_INVALID_SELECTOR_GAS);
}

// ============================================================================
// Execute: happy path — M4 stub returns 32 bytes of zeros.
// ============================================================================

TEST(AgentPrecompile, ExecuteVerifyAgentReturnsZeroWord)
{
    auto const input = make_input(SELECTOR_VERIFY_AGENT, 2 * ABI_WORD);
    auto const result = monad::meritium::agent_execute(input);

    ASSERT_EQ(result.status_code, EVMC_SUCCESS);
    ASSERT_EQ(result.output_size, ABI_WORD);
    ASSERT_NE(result.obuf, nullptr);
    for (size_t i = 0; i < ABI_WORD; ++i) {
        EXPECT_EQ(result.obuf[i], 0)
            << "byte " << i << " should be zero (unregistered)";
    }
    free_output(result);
}

TEST(AgentPrecompile, ExecuteSingleArgSelectorsReturnZeroWord)
{
    uint32_t const selectors[] = {
        SELECTOR_GET_AGENT_ID,
        SELECTOR_GET_WALLET,
        SELECTOR_IS_REGISTERED,
        SELECTOR_GET_AGENT_TIER,
        SELECTOR_GET_REPUTATION,
    };
    for (uint32_t const sel : selectors) {
        auto const input = make_input(sel, ABI_WORD);
        auto const result = monad::meritium::agent_execute(input);
        ASSERT_EQ(result.status_code, EVMC_SUCCESS)
            << "selector 0x" << std::hex << sel;
        ASSERT_EQ(result.output_size, ABI_WORD);
        ASSERT_NE(result.obuf, nullptr);
        for (size_t i = 0; i < ABI_WORD; ++i) {
            EXPECT_EQ(result.obuf[i], 0);
        }
        free_output(result);
    }
}

// ============================================================================
// Execute: input validation
// ============================================================================

TEST(AgentPrecompile, ExecuteEmptyInputFails)
{
    byte_string const empty;
    auto const result = monad::meritium::agent_execute(empty);
    EXPECT_EQ(result.status_code, EVMC_PRECOMPILE_FAILURE);
    EXPECT_EQ(result.obuf, nullptr);
    EXPECT_EQ(result.output_size, 0u);
}

TEST(AgentPrecompile, ExecuteSelectorOnlyFails)
{
    // Only 4 bytes — selector with no ABI args.
    auto const input = make_input(SELECTOR_GET_AGENT_ID, 0);
    auto const result = monad::meritium::agent_execute(input);
    EXPECT_EQ(result.status_code, EVMC_PRECOMPILE_FAILURE);
    free_output(result);
}

TEST(AgentPrecompile, ExecuteVerifyAgentTruncatedFails)
{
    // verifyAgent needs 4 + 2*32 bytes; supply only 4 + 32.
    auto const input = make_input(SELECTOR_VERIFY_AGENT, ABI_WORD);
    auto const result = monad::meritium::agent_execute(input);
    EXPECT_EQ(result.status_code, EVMC_PRECOMPILE_FAILURE);
    free_output(result);
}

TEST(AgentPrecompile, ExecuteUnknownSelectorFails)
{
    auto const input = make_input(0xDEADBEEF, ABI_WORD);
    auto const result = monad::meritium::agent_execute(input);
    EXPECT_EQ(result.status_code, EVMC_PRECOMPILE_FAILURE);
    free_output(result);
}

// ============================================================================
// Selector constants match chain/precompile/agent_precompile.rs Rust mirror.
// Failures here indicate ABI drift between the C++ precompile and Rust client.
// ============================================================================

TEST(AgentPrecompile, SelectorConstantsMatchRust)
{
    EXPECT_EQ(SELECTOR_VERIFY_AGENT, 0x85504c3fu);
    EXPECT_EQ(SELECTOR_GET_AGENT_ID, 0x736f42f1u);
    EXPECT_EQ(SELECTOR_GET_WALLET, 0x697b79e5u);
    EXPECT_EQ(SELECTOR_IS_REGISTERED, 0x3d3e1996u);
    EXPECT_EQ(SELECTOR_GET_AGENT_TIER, 0x8ca719ceu);
    EXPECT_EQ(SELECTOR_GET_REPUTATION, 0x5a1c253bu);
}

TEST(AgentPrecompile, GasConstantsMatchRust)
{
    // Rust: chain/precompile/gas.rs
    EXPECT_EQ(AGENT_VERIFY_GAS, 3'000u);
    EXPECT_EQ(AGENT_LOOKUP_GAS, 2'600u);
    EXPECT_EQ(AGENT_TIER_GAS, 2'600u);
    EXPECT_EQ(AGENT_REPUTATION_GAS, 2'600u);
    EXPECT_EQ(AGENT_IS_REGISTERED_GAS, 2'600u);
}

// ============================================================================
// State-backed tests (M5): precompile reads from AgentStateTable singleton.
// ============================================================================

namespace {

// Fixture that pre-populates the AgentStateTable with 3 known agents.
class AgentPrecompileWithState : public ::testing::Test
{
protected:
    // Fixed test vectors — mirrored in expected outputs below.
    bytes32_t id_alice_{};
    Address wallet_alice_{};
    bytes32_t id_bob_{};
    Address wallet_bob_{};
    bytes32_t id_carol_{};
    Address wallet_carol_{};

    void SetUp() override
    {
        AgentStateTable::instance().clear();

        // Alice: Tier2, reputation 450
        std::memset(id_alice_.bytes, 0xAA, 32);
        std::memset(wallet_alice_.bytes, 0x11, 20);
        // Bob: Tier4, reputation 800
        std::memset(id_bob_.bytes, 0xBB, 32);
        std::memset(wallet_bob_.bytes, 0x22, 20);
        // Carol: Tier1, reputation 50
        std::memset(id_carol_.bytes, 0xCC, 32);
        std::memset(wallet_carol_.bytes, 0x33, 20);

        auto &tbl = AgentStateTable::instance();
        tbl.register_agent(id_alice_, wallet_alice_, AgentTier::Tier2, 450);
        tbl.register_agent(id_bob_, wallet_bob_, AgentTier::Tier4, 800);
        tbl.register_agent(id_carol_, wallet_carol_, AgentTier::Tier1, 50);
    }

    void TearDown() override
    {
        AgentStateTable::instance().clear();
    }

    // Build verifyAgent(bytes32 id, address wallet) input.
    byte_string make_verify_input(
        bytes32_t const &id, Address const &wallet) const
    {
        byte_string out = make_input(SELECTOR_VERIFY_AGENT, 0);
        out.append(id.bytes, id.bytes + 32);
        // address is left-padded to 32 bytes in ABI
        out.append(12, uint8_t{0});
        out.append(wallet.bytes, wallet.bytes + 20);
        return out;
    }

    // Build single-bytes32-arg selector input.
    byte_string make_id_input(uint32_t selector, bytes32_t const &id) const
    {
        byte_string out = make_input(selector, 0);
        out.append(id.bytes, id.bytes + 32);
        return out;
    }

    // Build single-address-arg selector input.
    byte_string make_addr_input(uint32_t selector, Address const &wallet) const
    {
        byte_string out = make_input(selector, 0);
        out.append(12, uint8_t{0});
        out.append(wallet.bytes, wallet.bytes + 20);
        return out;
    }
};

// Read a uint N-byte big-endian right-aligned value from a 32-byte ABI word.
uint64_t read_uint_abi(uint8_t const *word, size_t n)
{
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) {
        v = (v << 8) | word[ABI_WORD - n + i];
    }
    return v;
}

} // namespace

TEST_F(AgentPrecompileWithState, VerifyAgentMatchingBindingReturnsTrue)
{
    auto const input = make_verify_input(id_alice_, wallet_alice_);
    auto const r = monad::meritium::agent_execute(input);
    ASSERT_EQ(r.status_code, EVMC_SUCCESS);
    ASSERT_EQ(r.output_size, ABI_WORD);
    EXPECT_EQ(r.obuf[31], 1); // true
    for (size_t i = 0; i < 31; ++i) {
        EXPECT_EQ(r.obuf[i], 0);
    }
    free_output(r);
}

TEST_F(AgentPrecompileWithState, VerifyAgentWrongWalletReturnsFalse)
{
    auto const input = make_verify_input(id_alice_, wallet_bob_);
    auto const r = monad::meritium::agent_execute(input);
    ASSERT_EQ(r.status_code, EVMC_SUCCESS);
    ASSERT_EQ(r.output_size, ABI_WORD);
    EXPECT_EQ(r.obuf[31], 0);
    free_output(r);
}

TEST_F(AgentPrecompileWithState, VerifyAgentUnregisteredReturnsFalse)
{
    bytes32_t id_unknown{};
    std::memset(id_unknown.bytes, 0xEE, 32);
    auto const input = make_verify_input(id_unknown, wallet_alice_);
    auto const r = monad::meritium::agent_execute(input);
    ASSERT_EQ(r.status_code, EVMC_SUCCESS);
    EXPECT_EQ(r.obuf[31], 0);
    free_output(r);
}

TEST_F(AgentPrecompileWithState, GetAgentIdReturnsBinding)
{
    auto const input = make_addr_input(SELECTOR_GET_AGENT_ID, wallet_bob_);
    auto const r = monad::meritium::agent_execute(input);
    ASSERT_EQ(r.status_code, EVMC_SUCCESS);
    ASSERT_EQ(r.output_size, ABI_WORD);
    EXPECT_EQ(std::memcmp(r.obuf, id_bob_.bytes, 32), 0);
    free_output(r);
}

TEST_F(AgentPrecompileWithState, GetAgentIdUnknownWalletReturnsZero)
{
    Address unknown{};
    std::memset(unknown.bytes, 0xEE, 20);
    auto const input = make_addr_input(SELECTOR_GET_AGENT_ID, unknown);
    auto const r = monad::meritium::agent_execute(input);
    ASSERT_EQ(r.status_code, EVMC_SUCCESS);
    for (size_t i = 0; i < ABI_WORD; ++i) {
        EXPECT_EQ(r.obuf[i], 0);
    }
    free_output(r);
}

TEST_F(AgentPrecompileWithState, GetWalletReturnsLeftPaddedAddress)
{
    auto const input = make_id_input(SELECTOR_GET_WALLET, id_carol_);
    auto const r = monad::meritium::agent_execute(input);
    ASSERT_EQ(r.status_code, EVMC_SUCCESS);
    ASSERT_EQ(r.output_size, ABI_WORD);
    // First 12 bytes should be zero-padding.
    for (size_t i = 0; i < 12; ++i) {
        EXPECT_EQ(r.obuf[i], 0);
    }
    // Last 20 bytes should equal wallet_carol_.
    EXPECT_EQ(std::memcmp(r.obuf + 12, wallet_carol_.bytes, 20), 0);
    free_output(r);
}

TEST_F(AgentPrecompileWithState, IsRegisteredTrueForKnownAgent)
{
    auto const input = make_id_input(SELECTOR_IS_REGISTERED, id_alice_);
    auto const r = monad::meritium::agent_execute(input);
    ASSERT_EQ(r.status_code, EVMC_SUCCESS);
    EXPECT_EQ(r.obuf[31], 1);
    free_output(r);
}

TEST_F(AgentPrecompileWithState, IsRegisteredFalseForUnknownAgent)
{
    bytes32_t id_unknown{};
    std::memset(id_unknown.bytes, 0xEE, 32);
    auto const input = make_id_input(SELECTOR_IS_REGISTERED, id_unknown);
    auto const r = monad::meritium::agent_execute(input);
    ASSERT_EQ(r.status_code, EVMC_SUCCESS);
    EXPECT_EQ(r.obuf[31], 0);
    free_output(r);
}

TEST_F(AgentPrecompileWithState, GetAgentTierReturnsTierEnumValue)
{
    auto const input = make_id_input(SELECTOR_GET_AGENT_TIER, id_bob_);
    auto const r = monad::meritium::agent_execute(input);
    ASSERT_EQ(r.status_code, EVMC_SUCCESS);
    EXPECT_EQ(read_uint_abi(r.obuf, 1), 4u); // Tier4
    free_output(r);
}

TEST_F(AgentPrecompileWithState, GetAgentTierUnknownReturnsZero)
{
    bytes32_t id_unknown{};
    std::memset(id_unknown.bytes, 0xEE, 32);
    auto const input = make_id_input(SELECTOR_GET_AGENT_TIER, id_unknown);
    auto const r = monad::meritium::agent_execute(input);
    ASSERT_EQ(r.status_code, EVMC_SUCCESS);
    EXPECT_EQ(read_uint_abi(r.obuf, 1), 0u);
    free_output(r);
}

TEST_F(AgentPrecompileWithState, GetReputationReturnsUint16)
{
    auto const input = make_id_input(SELECTOR_GET_REPUTATION, id_alice_);
    auto const r = monad::meritium::agent_execute(input);
    ASSERT_EQ(r.status_code, EVMC_SUCCESS);
    EXPECT_EQ(read_uint_abi(r.obuf, 2), 450u);
    free_output(r);
}

TEST_F(AgentPrecompileWithState, GetReputationUnknownReturnsZero)
{
    bytes32_t id_unknown{};
    std::memset(id_unknown.bytes, 0xEE, 32);
    auto const input = make_id_input(SELECTOR_GET_REPUTATION, id_unknown);
    auto const r = monad::meritium::agent_execute(input);
    ASSERT_EQ(r.status_code, EVMC_SUCCESS);
    EXPECT_EQ(read_uint_abi(r.obuf, 2), 0u);
    free_output(r);
}
