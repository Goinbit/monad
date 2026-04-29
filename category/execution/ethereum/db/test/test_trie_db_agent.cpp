// Copyright (C) 2026 Meritium contributors.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// Round-trip tests for the Meritium Agent state partition (AGENT_NIBBLE)
// via the production TrieDb / CommitBuilder path, using an in-memory MPT.
//
// These are the persistence tests for M5 — proving that Agent records
// survive the full write→commit→read cycle through Monad's native MPT.

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
#include <optional>

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

// Commit a block with only the given agent state (plus the minimum required
// tables for commit to succeed: block_header). We use non-zero block_id to
// satisfy TrieDb's MONAD_ASSERT(block_id != bytes32_t{}).
void commit_agents(
    TrieDb &tdb,
    uint64_t block_number,
    bytes32_t const &block_id,
    std::map<bytes32_t, AgentRecord> const &agents)
{
    CommitBuilder builder(block_number);
    builder.add_state_deltas(StateDeltas{})
        .add_code(Code{})
        .add_receipts({})
        .add_transactions({}, {})
        .add_call_frames({})
        .add_ommers({})
        .add_agent_state(agents);

    BlockHeader header;
    header.number = block_number;
    tdb.commit(block_id, builder, header, StateDeltas{}, [](BlockHeader &) {});
}

class TrieDbAgentFixture : public ::testing::Test
{
protected:
    InMemoryMachine machine{};
    mpt::Db db{machine};
    TrieDb tdb{db};
};

} // namespace

TEST_F(TrieDbAgentFixture, EmptyPartitionReturnsNullopt)
{
    commit_agents(tdb, 1, make_id(0x01), {});

    EXPECT_FALSE(tdb.read_agent_record(make_id(0xaa)).has_value());
}

TEST_F(TrieDbAgentFixture, SingleAgentRoundtrip)
{
    auto const id = make_id(0xaa);
    auto const wallet = make_wallet(0x11);

    std::map<bytes32_t, AgentRecord> agents;
    agents[id] = AgentRecord{
        .agent_id = id,
        .wallet = wallet,
        .tier = AgentTier::Tier2,
        .reputation = 450,
    };

    commit_agents(tdb, 1, make_id(0x01), agents);

    auto const rec = tdb.read_agent_record(id);
    ASSERT_TRUE(rec.has_value());
    EXPECT_EQ(rec->agent_id, id);
    EXPECT_EQ(rec->wallet, wallet);
    EXPECT_EQ(rec->tier, AgentTier::Tier2);
    EXPECT_EQ(rec->reputation, 450u);
}

TEST_F(TrieDbAgentFixture, MultipleAgentsRoundtripIndependently)
{
    std::map<bytes32_t, AgentRecord> agents;
    for (uint8_t seed = 1; seed <= 5; ++seed) {
        auto const id = make_id(seed);
        agents[id] = AgentRecord{
            .agent_id = id,
            .wallet = make_wallet(seed),
            .tier = static_cast<AgentTier>((seed % 4) + 1),
            .reputation = static_cast<uint16_t>(seed * 100),
        };
    }

    commit_agents(tdb, 1, make_id(0xff), agents);

    for (uint8_t seed = 1; seed <= 5; ++seed) {
        auto const rec = tdb.read_agent_record(make_id(seed));
        ASSERT_TRUE(rec.has_value()) << "seed " << int{seed};
        EXPECT_EQ(rec->wallet, make_wallet(seed));
        EXPECT_EQ(
            rec->tier, static_cast<AgentTier>((seed % 4) + 1));
        EXPECT_EQ(rec->reputation, static_cast<uint16_t>(seed * 100));
    }
}

TEST_F(TrieDbAgentFixture, UnregisteredLookupReturnsNullopt)
{
    std::map<bytes32_t, AgentRecord> agents;
    agents[make_id(0x01)] = AgentRecord{
        .agent_id = make_id(0x01),
        .wallet = make_wallet(0x11),
        .tier = AgentTier::Tier1,
        .reputation = 1,
    };
    commit_agents(tdb, 1, make_id(0xaa), agents);

    // Registered
    EXPECT_TRUE(tdb.read_agent_record(make_id(0x01)).has_value());
    // Not registered
    EXPECT_FALSE(tdb.read_agent_record(make_id(0x99)).has_value());
}

TEST_F(TrieDbAgentFixture, UpdateAcrossBlocksOverwritesRecord)
{
    auto const id = make_id(0xaa);
    auto const wallet = make_wallet(0x11);

    std::map<bytes32_t, AgentRecord> agents;
    agents[id] = AgentRecord{
        .agent_id = id,
        .wallet = wallet,
        .tier = AgentTier::Tier1,
        .reputation = 100,
    };
    commit_agents(tdb, 1, make_id(0x01), agents);
    auto const rec1 = tdb.read_agent_record(id);
    ASSERT_TRUE(rec1.has_value());
    EXPECT_EQ(rec1->tier, AgentTier::Tier1);
    EXPECT_EQ(rec1->reputation, 100u);

    // Next block: bump tier + reputation
    agents[id].tier = AgentTier::Tier4;
    agents[id].reputation = 999;
    commit_agents(tdb, 2, make_id(0x02), agents);

    auto const rec2 = tdb.read_agent_record(id);
    ASSERT_TRUE(rec2.has_value());
    EXPECT_EQ(rec2->tier, AgentTier::Tier4);
    EXPECT_EQ(rec2->reputation, 999u);
}
