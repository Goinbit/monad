// Copyright (C) 2026 Meritium contributors.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// M5 Part 3: AgentStateTable ↔ TrieDb sync hooks.
//
// Covers:
//   * TrieDb::enumerate_agents() — walks AGENT_NIBBLE subtree
//   * TrieDb::agent_state_root() — MPT root of the subtree
//   * AgentStateTable::warm_from_triedb() — startup warm-up
//   * AgentStateTable::export_to_commit() — stage cache into a block commit
//   * Full round-trip: cache → MPT commit → fresh cache → warm → same data

#include <category/core/bytes.hpp>
#include <category/execution/ethereum/agent_state.hpp>
#include <category/execution/ethereum/core/address.hpp>
#include <category/execution/ethereum/core/block.hpp>
#include <category/execution/ethereum/db/commit_builder.hpp>
#include <category/execution/ethereum/db/trie_db.hpp>
#include <category/execution/ethereum/db/util.hpp>
#include <category/execution/ethereum/state2/state_deltas.hpp>
#include <category/mpt/db.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <map>

using namespace monad;
using namespace monad::meritium;

namespace {

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

// Commit a block that carries only Agent state (plus the minimum other
// tables required for TrieDb::commit to succeed).
void commit_with_agent_snapshot(
    TrieDb &tdb,
    uint64_t block_number,
    bytes32_t const &block_id,
    AgentStateTable const &table)
{
    CommitBuilder builder(block_number);
    builder.add_state_deltas(StateDeltas{})
        .add_code(Code{})
        .add_receipts({})
        .add_transactions({}, {})
        .add_call_frames({})
        .add_ommers({});
    table.export_to_commit(builder);

    BlockHeader header;
    header.number = block_number;
    tdb.commit(block_id, builder, header, StateDeltas{}, [](BlockHeader &) {});
}

class AgentSyncFixture : public ::testing::Test
{
protected:
    InMemoryMachine machine{};
    mpt::Db db{machine};
    TrieDb tdb{db};

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
// enumerate_agents / agent_state_root
// ============================================================================

TEST_F(AgentSyncFixture, EnumerateEmptyPartitionReturnsEmptyMap)
{
    auto &table = AgentStateTable::instance();
    commit_with_agent_snapshot(tdb, 1, make_id(0xff), table);

    auto const loaded = tdb.enumerate_agents();
    EXPECT_TRUE(loaded.empty());
}

TEST_F(AgentSyncFixture, EnumerateReturnsAllRegisteredAgents)
{
    auto &table = AgentStateTable::instance();
    for (uint8_t s = 1; s <= 5; ++s) {
        table.register_agent(
            make_id(s),
            make_wallet(s),
            static_cast<AgentTier>((s % 4) + 1),
            static_cast<uint16_t>(s * 100));
    }
    commit_with_agent_snapshot(tdb, 1, make_id(0xff), table);

    auto const loaded = tdb.enumerate_agents();
    ASSERT_EQ(loaded.size(), 5u);
    for (uint8_t s = 1; s <= 5; ++s) {
        auto const it = loaded.find(make_id(s));
        ASSERT_NE(it, loaded.end()) << "seed " << int{s};
        EXPECT_EQ(it->second.wallet, make_wallet(s));
        EXPECT_EQ(
            it->second.tier, static_cast<AgentTier>((s % 4) + 1));
        EXPECT_EQ(it->second.reputation, static_cast<uint16_t>(s * 100));
    }
}

TEST_F(AgentSyncFixture, AgentStateRootIsZeroWhenEmpty)
{
    auto &table = AgentStateTable::instance();
    commit_with_agent_snapshot(tdb, 1, make_id(0xff), table);
    // No agents committed -> root is NULL_ROOT (bytes32 of zeros).
    EXPECT_EQ(tdb.agent_state_root(), NULL_ROOT);
}

TEST_F(AgentSyncFixture, AgentStateRootChangesWhenStateChanges)
{
    auto &table = AgentStateTable::instance();
    table.register_agent(
        make_id(0x01), make_wallet(0x11), AgentTier::Tier1, 100);
    commit_with_agent_snapshot(tdb, 1, make_id(0x01), table);
    auto const root1 = tdb.agent_state_root();
    EXPECT_NE(root1, NULL_ROOT);

    // Mutate -> commit again -> different root.
    table.update_reputation(make_id(0x01), 999);
    commit_with_agent_snapshot(tdb, 2, make_id(0x02), table);
    auto const root2 = tdb.agent_state_root();
    EXPECT_NE(root1, root2);
}

// ============================================================================
// AgentStateTable::warm_from_triedb
// ============================================================================

TEST_F(AgentSyncFixture, WarmFromEmptyTrieDbYieldsEmptyCache)
{
    auto &table = AgentStateTable::instance();
    commit_with_agent_snapshot(tdb, 1, make_id(0xff), table);

    // Seed the cache with noise then warm — expect clean wipe.
    table.register_agent(
        make_id(0xee), make_wallet(0xee), AgentTier::Tier1, 7);
    EXPECT_EQ(table.size(), 1u);

    uint64_t const n = table.warm_from_triedb(tdb);
    EXPECT_EQ(n, 0u);
    EXPECT_EQ(table.size(), 0u);
}

TEST_F(AgentSyncFixture, WarmFromPopulatedTrieDbRestoresAllRecords)
{
    auto &table = AgentStateTable::instance();
    for (uint8_t s = 1; s <= 3; ++s) {
        table.register_agent(
            make_id(s),
            make_wallet(s),
            static_cast<AgentTier>(s),
            static_cast<uint16_t>(s * 10));
    }
    commit_with_agent_snapshot(tdb, 1, make_id(0x01), table);

    // Simulate a fresh node start.
    table.clear();
    EXPECT_EQ(table.size(), 0u);

    uint64_t const n = table.warm_from_triedb(tdb);
    EXPECT_EQ(n, 3u);
    EXPECT_EQ(table.size(), 3u);

    for (uint8_t s = 1; s <= 3; ++s) {
        auto const rec = table.find_by_id(make_id(s));
        ASSERT_TRUE(rec.has_value());
        EXPECT_EQ(rec->wallet, make_wallet(s));
        EXPECT_EQ(rec->tier, static_cast<AgentTier>(s));
        EXPECT_EQ(rec->reputation, static_cast<uint16_t>(s * 10));
        // Wallet reverse index must also be rebuilt.
        auto const by_w = table.find_by_wallet(make_wallet(s));
        ASSERT_TRUE(by_w.has_value());
        EXPECT_EQ(by_w->agent_id, make_id(s));
    }
}

// ============================================================================
// Full round-trip
// ============================================================================

TEST_F(AgentSyncFixture, RoundTrip_RegisterCommitClearWarmProducesSameState)
{
    auto &table = AgentStateTable::instance();

    // Step 1: populate cache.
    table.register_agent(
        make_id(0xaa), make_wallet(0x11), AgentTier::Tier2, 450);
    table.register_agent(
        make_id(0xbb), make_wallet(0x22), AgentTier::Tier4, 800);
    auto const root_before_commit = table.compute_root();

    // Step 2: commit cache to MPT.
    commit_with_agent_snapshot(tdb, 1, make_id(0x01), table);

    // Step 3: blow cache away (simulates restart).
    table.clear();
    EXPECT_EQ(table.size(), 0u);

    // Step 4: warm from MPT.
    uint64_t const n = table.warm_from_triedb(tdb);
    EXPECT_EQ(n, 2u);

    // Step 5: in-memory root must match pre-commit state.
    EXPECT_EQ(table.compute_root(), root_before_commit);

    // Step 6: individual lookups return correct data.
    auto const a = table.find_by_id(make_id(0xaa));
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->tier, AgentTier::Tier2);
    EXPECT_EQ(a->reputation, 450u);

    auto const b = table.find_by_id(make_id(0xbb));
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->tier, AgentTier::Tier4);
    EXPECT_EQ(b->reputation, 800u);
}

TEST_F(AgentSyncFixture, AcrossTwoBlocks_UpdatesPersistThroughWarm)
{
    auto &table = AgentStateTable::instance();

    // Block 1: register one agent.
    table.register_agent(
        make_id(0xaa), make_wallet(0x11), AgentTier::Tier1, 100);
    commit_with_agent_snapshot(tdb, 1, make_id(0x01), table);

    // Block 2: bump reputation (cache + commit).
    table.update_reputation(make_id(0xaa), 950);
    commit_with_agent_snapshot(tdb, 2, make_id(0x02), table);

    // Fresh start.
    table.clear();
    table.warm_from_triedb(tdb);

    auto const rec = table.find_by_id(make_id(0xaa));
    ASSERT_TRUE(rec.has_value());
    EXPECT_EQ(rec->reputation, 950u); // latest block wins
}

TEST_F(AgentSyncFixture, ExportToCommitIsIdempotent)
{
    auto &table = AgentStateTable::instance();
    table.register_agent(
        make_id(0xaa), make_wallet(0x11), AgentTier::Tier3, 300);

    commit_with_agent_snapshot(tdb, 1, make_id(0x01), table);
    auto const root1 = tdb.agent_state_root();

    // Commit again without any mutation -> same root.
    commit_with_agent_snapshot(tdb, 2, make_id(0x02), table);
    auto const root2 = tdb.agent_state_root();
    EXPECT_EQ(root1, root2);
}
