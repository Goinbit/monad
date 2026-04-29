// Copyright (C) 2026 Meritium contributors.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#pragma once

#include <category/core/bytes.hpp>
#include <category/core/config.hpp>
#include <category/execution/ethereum/core/address.hpp>

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>

// Meritium Agent State Partition (M5).
//
// Stores bindings and metadata for every registered Agent on the chain:
//   agent_id (bytes32) ↔ wallet (address)
//   tier (uint8, 0=unregistered, 1..4=Tier1..Tier4)
//   reputation (uint16)
//
// DESIGN STATUS (2026-04-15): MVP in-memory implementation with a
// deterministic Keccak256 root. Designed as a replaceable abstraction —
// the production version will back `AgentStateTable` with a MonadDB MPT
// partition reached via the monad-triedb Rust FFI, without changing the
// public API below.
//
// Writes happen via AgentRegistry.sol system transactions (see L6 contracts).
// Reads happen from the Agent precompile at 0x0101 (category/execution/
// ethereum/precompiles_agent.cpp) via the singleton accessor.

MONAD_NAMESPACE_BEGIN

// Forward declarations — header keeps trie_db / commit_builder out of the
// public include surface (they transitively depend on this file).
class TrieDb;
class CommitBuilder;

namespace meritium {

// The four-tier staking system from CLAUDE.md §Tokenomics.
// Tier 0 is the sentinel for "not registered".
enum class AgentTier : uint8_t {
    Unregistered = 0,
    Tier1 = 1, // 500 MTC
    Tier2 = 2, // 2,000 MTC
    Tier3 = 3, // 5,000 MTC
    Tier4 = 4, // 15,000 MTC
};

struct AgentRecord
{
    bytes32_t agent_id{};
    Address wallet{};
    AgentTier tier{AgentTier::Unregistered};
    uint16_t reputation{0};

    friend bool operator==(AgentRecord const &, AgentRecord const &) = default;
};

// Thread-safe in-memory store of every registered Agent on the chain.
//
// Singleton — in MVP there is exactly one AgentStateTable per process,
// implicitly scoped to the current chain being executed. In production this
// will be superseded by an MPT partition commit object created per block.
class AgentStateTable
{
public:
    // Global accessor. Always returns the same instance for the lifetime
    // of the process. Thread-safe.
    static AgentStateTable &instance();

    // Registration — called by AgentRegistry.sol via system transactions.
    // Replaces any existing record for the same agent_id.
    // Returns false if `wallet` is already bound to a DIFFERENT agent_id.
    bool register_agent(
        bytes32_t const &agent_id,
        Address const &wallet,
        AgentTier tier,
        uint16_t reputation);

    // Update an Agent's reputation. No-op if agent_id isn't registered.
    // Returns true if updated.
    bool update_reputation(bytes32_t const &agent_id, uint16_t reputation);

    // Update an Agent's tier (e.g. after stake changes). No-op if
    // agent_id isn't registered. Returns true if updated.
    bool update_tier(bytes32_t const &agent_id, AgentTier tier);

    // Remove an Agent from the table. No-op if not present.
    void unregister(bytes32_t const &agent_id);

    // Wipe all state. For tests + chain reset only.
    void clear();

    // Lookups used by the Agent precompile.
    std::optional<AgentRecord> find_by_id(bytes32_t const &agent_id) const;
    std::optional<AgentRecord> find_by_wallet(Address const &wallet) const;

    // Returns true iff the agent_id is registered AND bound to this wallet.
    bool
    verify_binding(bytes32_t const &agent_id, Address const &wallet) const;

    // Number of registered agents. Used to populate
    // BlockHeader.active_agent_count.
    uint64_t size() const;

    // M5 Part 3: warm the in-memory cache from persistent MPT storage.
    // Enumerates every AgentRecord under AGENT_NIBBLE via TraverseMachine
    // and replaces the current in-memory state with the result. Called once
    // at node startup (and again after state sync / snapshot restore).
    // Returns the number of records loaded.
    uint64_t warm_from_triedb(::monad::TrieDb &tdb);

    // M5 Part 3: stage the current in-memory state into a CommitBuilder
    // (via add_agent_state). Call before `TrieDb::commit(...)` each block
    // that mutated AgentStateTable. Produces the subtree that will be
    // merged into the block's MPT root under AGENT_NIBBLE.
    void export_to_commit(::monad::CommitBuilder &builder) const;

    // Deterministic root hash of the table contents.
    //
    // Serialization (deterministic because std::map iterates sorted):
    //   for each (agent_id, record) in sorted order:
    //     append agent_id (32 bytes)
    //     append wallet (20 bytes)
    //     append tier (1 byte)
    //     append reputation (2 bytes, big-endian)
    //   root = keccak256(concat)
    //
    // When the table is empty, returns bytes32_t{} (all zeros) so
    // unmodified blocks keep the same hash as upstream Monad.
    bytes32_t compute_root() const;

private:
    AgentStateTable() = default;
    AgentStateTable(AgentStateTable const &) = delete;
    AgentStateTable &operator=(AgentStateTable const &) = delete;

    mutable std::mutex mu_;
    // Sorted by agent_id for deterministic root.
    std::map<bytes32_t, AgentRecord> by_id_;
    // Reverse index for wallet -> agent_id lookups.
    std::map<Address, bytes32_t> wallet_to_id_;
};

} // namespace meritium
MONAD_NAMESPACE_END
