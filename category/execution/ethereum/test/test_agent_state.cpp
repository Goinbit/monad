// Copyright (C) 2026 Meritium contributors.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// Tests for AgentStateTable (Meritium Agent state partition, M5 MVP).
// Covers CRUD, determinism of the in-memory state fingerprint, and invariants.

#include <category/core/bytes.hpp>
#include <category/execution/ethereum/agent_state.hpp>
#include <category/execution/ethereum/core/address.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

using namespace monad;
using namespace monad::meritium;

namespace {

// Small helpers for building deterministic test fixtures.
bytes32_t make_id(uint8_t seed)
{
    bytes32_t id{};
    for (size_t i = 0; i < 32; ++i) {
        id.bytes[i] = seed;
    }
    return id;
}

Address make_wallet(uint8_t seed)
{
    Address addr{};
    for (size_t i = 0; i < 20; ++i) {
        addr.bytes[i] = seed;
    }
    return addr;
}

// Each TEST gets a clean table — global singleton state leaks across tests
// otherwise.
class AgentStateFixture : public ::testing::Test
{
protected:
    void SetUp() override
    {
        AgentStateTable::instance().clear();
    }

    void TearDown() override
    {
        AgentStateTable::instance().clear();
    }
};

} // namespace

// ============================================================================
// Basic CRUD
// ============================================================================

TEST_F(AgentStateFixture, EmptyTableRootIsZero)
{
    auto &table = AgentStateTable::instance();
    EXPECT_EQ(table.size(), 0u);
    EXPECT_EQ(table.compute_fingerprint(), bytes32_t{});
}

TEST_F(AgentStateFixture, RegisterThenLookup)
{
    auto &table = AgentStateTable::instance();
    auto const id = make_id(0xaa);
    auto const wallet = make_wallet(0x11);

    ASSERT_TRUE(table.register_agent(id, wallet, AgentTier::Tier2, 450));
    EXPECT_EQ(table.size(), 1u);

    auto const by_id = table.find_by_id(id);
    ASSERT_TRUE(by_id.has_value());
    EXPECT_EQ(by_id->agent_id, id);
    EXPECT_EQ(by_id->wallet, wallet);
    EXPECT_EQ(by_id->tier, AgentTier::Tier2);
    EXPECT_EQ(by_id->reputation, 450u);

    auto const by_wallet = table.find_by_wallet(wallet);
    ASSERT_TRUE(by_wallet.has_value());
    EXPECT_EQ(by_wallet->agent_id, id);
}

TEST_F(AgentStateFixture, LookupUnregisteredReturnsNullopt)
{
    auto &table = AgentStateTable::instance();
    EXPECT_FALSE(table.find_by_id(make_id(0xee)).has_value());
    EXPECT_FALSE(table.find_by_wallet(make_wallet(0xee)).has_value());
    EXPECT_FALSE(table.verify_binding(make_id(0xee), make_wallet(0xee)));
}

TEST_F(AgentStateFixture, VerifyBindingMatchesAndRejects)
{
    auto &table = AgentStateTable::instance();
    auto const id1 = make_id(0xaa);
    auto const id2 = make_id(0xbb);
    auto const wallet1 = make_wallet(0x11);
    auto const wallet2 = make_wallet(0x22);

    table.register_agent(id1, wallet1, AgentTier::Tier1, 100);
    table.register_agent(id2, wallet2, AgentTier::Tier1, 100);

    EXPECT_TRUE(table.verify_binding(id1, wallet1));
    EXPECT_TRUE(table.verify_binding(id2, wallet2));
    // Wrong pairings must fail.
    EXPECT_FALSE(table.verify_binding(id1, wallet2));
    EXPECT_FALSE(table.verify_binding(id2, wallet1));
}

// ============================================================================
// Invariants
// ============================================================================

TEST_F(AgentStateFixture, RejectsWalletBoundToDifferentAgent)
{
    auto &table = AgentStateTable::instance();
    auto const wallet = make_wallet(0x11);

    ASSERT_TRUE(
        table.register_agent(make_id(0x01), wallet, AgentTier::Tier1, 100));
    // Same wallet, different agent_id -> must fail.
    EXPECT_FALSE(
        table.register_agent(make_id(0x02), wallet, AgentTier::Tier1, 100));
}

TEST_F(AgentStateFixture, ReregisterSameAgentReplacesFields)
{
    auto &table = AgentStateTable::instance();
    auto const id = make_id(0xaa);
    auto const wallet = make_wallet(0x11);

    table.register_agent(id, wallet, AgentTier::Tier1, 100);
    ASSERT_TRUE(table.register_agent(id, wallet, AgentTier::Tier3, 800));

    auto const rec = table.find_by_id(id);
    ASSERT_TRUE(rec.has_value());
    EXPECT_EQ(rec->tier, AgentTier::Tier3);
    EXPECT_EQ(rec->reputation, 800u);
    EXPECT_EQ(table.size(), 1u);
}

TEST_F(AgentStateFixture, RebindAgentToNewWallet)
{
    auto &table = AgentStateTable::instance();
    auto const id = make_id(0xaa);
    auto const old_wallet = make_wallet(0x11);
    auto const new_wallet = make_wallet(0x22);

    table.register_agent(id, old_wallet, AgentTier::Tier1, 100);
    ASSERT_TRUE(
        table.register_agent(id, new_wallet, AgentTier::Tier1, 100));

    // Old reverse index must be gone.
    EXPECT_FALSE(table.find_by_wallet(old_wallet).has_value());
    // New reverse index is correct.
    auto const rec = table.find_by_wallet(new_wallet);
    ASSERT_TRUE(rec.has_value());
    EXPECT_EQ(rec->agent_id, id);
}

TEST_F(AgentStateFixture, UpdateReputationOnlyChangesReputation)
{
    auto &table = AgentStateTable::instance();
    auto const id = make_id(0xaa);
    table.register_agent(id, make_wallet(0x11), AgentTier::Tier2, 100);

    ASSERT_TRUE(table.update_reputation(id, 999));
    auto const rec = table.find_by_id(id);
    ASSERT_TRUE(rec.has_value());
    EXPECT_EQ(rec->reputation, 999u);
    EXPECT_EQ(rec->tier, AgentTier::Tier2); // unchanged
}

TEST_F(AgentStateFixture, UpdateTierOnlyChangesTier)
{
    auto &table = AgentStateTable::instance();
    auto const id = make_id(0xaa);
    table.register_agent(id, make_wallet(0x11), AgentTier::Tier1, 500);

    ASSERT_TRUE(table.update_tier(id, AgentTier::Tier4));
    auto const rec = table.find_by_id(id);
    ASSERT_TRUE(rec.has_value());
    EXPECT_EQ(rec->tier, AgentTier::Tier4);
    EXPECT_EQ(rec->reputation, 500u); // unchanged
}

TEST_F(AgentStateFixture, UpdateUnknownAgentIsNoop)
{
    auto &table = AgentStateTable::instance();
    EXPECT_FALSE(table.update_reputation(make_id(0xee), 500));
    EXPECT_FALSE(table.update_tier(make_id(0xee), AgentTier::Tier2));
}

TEST_F(AgentStateFixture, UnregisterClearsBothIndexes)
{
    auto &table = AgentStateTable::instance();
    auto const id = make_id(0xaa);
    auto const wallet = make_wallet(0x11);
    table.register_agent(id, wallet, AgentTier::Tier1, 100);

    table.unregister(id);
    EXPECT_EQ(table.size(), 0u);
    EXPECT_FALSE(table.find_by_id(id).has_value());
    EXPECT_FALSE(table.find_by_wallet(wallet).has_value());
}

// ============================================================================
// Fingerprint determinism
// ============================================================================

TEST_F(AgentStateFixture, RootIsDeterministicRegardlessOfInsertionOrder)
{
    auto &table = AgentStateTable::instance();

    // Forward order
    table.register_agent(make_id(0x01), make_wallet(0x11), AgentTier::Tier1, 10);
    table.register_agent(make_id(0x02), make_wallet(0x22), AgentTier::Tier2, 20);
    table.register_agent(make_id(0x03), make_wallet(0x33), AgentTier::Tier3, 30);
    auto const root_forward = table.compute_fingerprint();

    table.clear();

    // Reverse order
    table.register_agent(make_id(0x03), make_wallet(0x33), AgentTier::Tier3, 30);
    table.register_agent(make_id(0x02), make_wallet(0x22), AgentTier::Tier2, 20);
    table.register_agent(make_id(0x01), make_wallet(0x11), AgentTier::Tier1, 10);
    auto const root_reverse = table.compute_fingerprint();

    EXPECT_EQ(root_forward, root_reverse);
    EXPECT_NE(root_forward, bytes32_t{});
}

TEST_F(AgentStateFixture, RootChangesWhenStateChanges)
{
    auto &table = AgentStateTable::instance();
    auto const empty_root = table.compute_fingerprint();

    table.register_agent(make_id(0x01), make_wallet(0x11), AgentTier::Tier1, 10);
    auto const one_agent_root = table.compute_fingerprint();
    EXPECT_NE(empty_root, one_agent_root);

    table.update_reputation(make_id(0x01), 999);
    auto const reputed_root = table.compute_fingerprint();
    EXPECT_NE(one_agent_root, reputed_root);

    table.update_tier(make_id(0x01), AgentTier::Tier4);
    auto const tiered_root = table.compute_fingerprint();
    EXPECT_NE(reputed_root, tiered_root);
}

TEST_F(AgentStateFixture, RootStableAcrossUnrelatedReads)
{
    auto &table = AgentStateTable::instance();
    table.register_agent(make_id(0xaa), make_wallet(0x11), AgentTier::Tier2, 250);
    auto const root_before = table.compute_fingerprint();
    (void)table.find_by_id(make_id(0xff));
    (void)table.find_by_wallet(make_wallet(0xff));
    auto const root_after = table.compute_fingerprint();
    EXPECT_EQ(root_before, root_after);
}

// ============================================================================
// Counter
// ============================================================================

TEST_F(AgentStateFixture, SizeTracksRegistrations)
{
    auto &table = AgentStateTable::instance();
    EXPECT_EQ(table.size(), 0u);
    table.register_agent(make_id(1), make_wallet(1), AgentTier::Tier1, 0);
    table.register_agent(make_id(2), make_wallet(2), AgentTier::Tier1, 0);
    table.register_agent(make_id(3), make_wallet(3), AgentTier::Tier1, 0);
    EXPECT_EQ(table.size(), 3u);
    table.unregister(make_id(2));
    EXPECT_EQ(table.size(), 2u);
}
