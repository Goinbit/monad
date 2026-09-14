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
#include <utility>

// Meritium Agent State Partition (M5).
//
// Stores bindings and metadata for every registered Agent on the chain:
//   agent_id (bytes32) ↔ wallet (address)
//   tier (uint8, 0=unregistered, 1..4=Tier1..Tier4)
//   reputation (uint16)
//
// DESIGN STATUS (2026-04-15): MVP in-memory implementation with a
// deterministic Keccak256 fingerprint (NOT the canonical MPT root).
// Designed as a replaceable abstraction —
// the production version will back `AgentStateTable` with a MonadDB MPT
// partition reached via the monad-triedb Rust FFI, without changing the
// public API below.
//
// Writes happen via AgentRegistry.sol system transactions (see L6 contracts).
// Reads happen from the Agent precompile at 0x0101 (category/execution/
// ethereum/precompiles_agent.cpp) via a worker-thread scoped accessor.

MONAD_NAMESPACE_BEGIN

// Forward declarations — header keeps trie_db / commit_builder out of the
// public include surface (they transitively depend on this file).
class TrieDb;
class TrieRODb;
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
// Transitional design for M-03:
// - legacy global singleton remains available for compatibility
// - execution path binds a block-scoped table per worker thread via
//   ScopedAgentStateTableContext and reads/writes through `current()`.
class AgentStateTable
{
public:
    // Transitional M-03: allow explicitly owned table instances to be
    // constructed by node/runloop wiring while singleton fallback remains.
    AgentStateTable() = default;

    // Global accessor. Always returns the same instance for the lifetime
    // of the process. Thread-safe.
    static AgentStateTable &instance();

    // Execution-scoped accessor.
    //
    // Returns the table bound by ScopedAgentStateTableContext for the current
    // worker thread. Calling this without an active scoped binding is a logic
    // error and will assert.
    static AgentStateTable &current();

    // Returns the table currently bound to this worker thread, or nullptr if
    // no scoped context is active.
    static AgentStateTable *try_current();

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

    // Monotonic mutation counter used by transaction execution to detect
    // Agent state races and trigger retry.
    uint64_t version() const;

    // M5 Part 3: warm the in-memory cache from persistent MPT storage.
    // Enumerates every AgentRecord under AGENT_NIBBLE via TraverseMachine
    // and replaces the current in-memory state with the result. Called once
    // at node startup (and again after state sync / snapshot restore).
    // Returns the number of records loaded.
    uint64_t warm_from_triedb(::monad::TrieDb &tdb);
    uint64_t warm_from_triedb(::monad::TrieRODb &tdb);

    // M5 Part 3: stage the current in-memory state into a CommitBuilder
    // (via add_agent_state). Call before `TrieDb::commit(...)` each block
    // that mutated AgentStateTable. Produces the subtree that will be
    // merged into the block's MPT root under AGENT_NIBBLE.
    void export_to_commit(::monad::CommitBuilder &builder) const;

    // Deterministic in-memory fingerprint of table contents.
    //
    // NOTE: this is intentionally NOT a canonical MPT root. The canonical
    // on-chain root for header.agent_state_root is produced by triedb commit
    // path and read via TrieDb::agent_state_root().
    bytes32_t compute_fingerprint() const;

    [[deprecated(
        "compute_root() is a legacy alias for compute_fingerprint() and "
        "does NOT return canonical MPT root; use TrieDb::agent_state_root()")]]
    bytes32_t compute_root() const;

private:
    static AgentStateTable *current_ptr_for_thread();
    static void set_current_ptr_for_thread(AgentStateTable *);
    friend class ScopedAgentStateTableContext;

    AgentStateTable(AgentStateTable const &) = delete;
    AgentStateTable &operator=(AgentStateTable const &) = delete;

    mutable std::mutex mu_;
    // Sorted by agent_id for deterministic root.
    std::map<bytes32_t, AgentRecord> by_id_;
    // Reverse index for wallet -> agent_id lookups.
    std::map<Address, bytes32_t> wallet_to_id_;
    uint64_t version_{0};
};

// Binds an AgentStateTable to the current worker thread for the scope.
// Nested bindings are supported and restored in LIFO order.
class ScopedAgentStateTableContext
{
public:
    explicit ScopedAgentStateTableContext(AgentStateTable &table)
        : previous_{AgentStateTable::current_ptr_for_thread()}
    {
        AgentStateTable::set_current_ptr_for_thread(&table);
    }

    ~ScopedAgentStateTableContext()
    {
        AgentStateTable::set_current_ptr_for_thread(previous_);
    }

    ScopedAgentStateTableContext(ScopedAgentStateTableContext const &) = delete;
    ScopedAgentStateTableContext &
    operator=(ScopedAgentStateTableContext const &) = delete;

private:
    AgentStateTable *previous_;
};

} // namespace meritium
MONAD_NAMESPACE_END
