// Copyright (C) 2026 Meritium contributors.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#include <category/execution/ethereum/agent_state.hpp>

#include <category/core/assert.h>
#include <category/core/byte_string.hpp>
#include <category/core/keccak.hpp>
#include <category/execution/ethereum/db/commit_builder.hpp>
#include <category/execution/ethereum/db/trie_db.hpp>
#include <category/execution/ethereum/db/trie_rodb.hpp>

#include <cstring>
#include <mutex>
#include <utility>

MONAD_NAMESPACE_BEGIN
namespace meritium {

namespace
{
thread_local AgentStateTable *g_current_agent_state_table = nullptr;
} // namespace

AgentStateTable &AgentStateTable::instance()
{
    // C++11 guarantees a thread-safe first-access init for block-scope
    // statics, so this is the idiomatic thread-safe singleton.
    static AgentStateTable table;
    return table;
}

AgentStateTable &AgentStateTable::current()
{
    auto *const table = try_current();
    MONAD_ASSERT_PRINTF(
        table != nullptr,
        "AgentStateTable::current() called without "
        "ScopedAgentStateTableContext");
    return *table;
}

AgentStateTable *AgentStateTable::try_current()
{
    return current_ptr_for_thread();
}

AgentStateTable *AgentStateTable::current_ptr_for_thread()
{
    return g_current_agent_state_table;
}

void AgentStateTable::set_current_ptr_for_thread(AgentStateTable *table)
{
    g_current_agent_state_table = table;
}

bool AgentStateTable::register_agent(
    bytes32_t const &agent_id,
    Address const &wallet,
    AgentTier const tier,
    uint16_t const reputation)
{
    std::lock_guard<std::mutex> lock(mu_);

    // Enforce "one wallet -> at most one agent_id" invariant.
    if (auto const it = wallet_to_id_.find(wallet);
        it != wallet_to_id_.end() && it->second != agent_id) {
        return false;
    }

    // If this agent_id was previously bound to a different wallet,
    // remove that stale reverse index first.
    if (auto const existing = by_id_.find(agent_id); existing != by_id_.end()) {
        if (existing->second.wallet != wallet) {
            wallet_to_id_.erase(existing->second.wallet);
        }
    }

    by_id_[agent_id] = AgentRecord{
        .agent_id = agent_id,
        .wallet = wallet,
        .tier = tier,
        .reputation = reputation,
    };
    wallet_to_id_[wallet] = agent_id;
    ++version_;
    return true;
}

bool AgentStateTable::update_reputation(
    bytes32_t const &agent_id, uint16_t const reputation)
{
    std::lock_guard<std::mutex> lock(mu_);
    auto const it = by_id_.find(agent_id);
    if (it == by_id_.end()) {
        return false;
    }
    if (it->second.reputation == reputation) {
        return true;
    }
    it->second.reputation = reputation;
    ++version_;
    return true;
}

bool AgentStateTable::update_tier(
    bytes32_t const &agent_id, AgentTier const tier)
{
    std::lock_guard<std::mutex> lock(mu_);
    auto const it = by_id_.find(agent_id);
    if (it == by_id_.end()) {
        return false;
    }
    if (it->second.tier == tier) {
        return true;
    }
    it->second.tier = tier;
    ++version_;
    return true;
}

void AgentStateTable::unregister(bytes32_t const &agent_id)
{
    std::lock_guard<std::mutex> lock(mu_);
    auto const it = by_id_.find(agent_id);
    if (it == by_id_.end()) {
        return;
    }
    wallet_to_id_.erase(it->second.wallet);
    by_id_.erase(it);
    ++version_;
}

void AgentStateTable::clear()
{
    std::lock_guard<std::mutex> lock(mu_);
    if (by_id_.empty() && wallet_to_id_.empty()) {
        return;
    }
    by_id_.clear();
    wallet_to_id_.clear();
    ++version_;
}

std::optional<AgentRecord>
AgentStateTable::find_by_id(bytes32_t const &agent_id) const
{
    std::lock_guard<std::mutex> lock(mu_);
    auto const it = by_id_.find(agent_id);
    if (it == by_id_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<AgentRecord>
AgentStateTable::find_by_wallet(Address const &wallet) const
{
    std::lock_guard<std::mutex> lock(mu_);
    auto const wi = wallet_to_id_.find(wallet);
    if (wi == wallet_to_id_.end()) {
        return std::nullopt;
    }
    auto const ri = by_id_.find(wi->second);
    if (ri == by_id_.end()) {
        // Inconsistent state shouldn't happen — guard anyway.
        return std::nullopt;
    }
    return ri->second;
}

bool AgentStateTable::verify_binding(
    bytes32_t const &agent_id, Address const &wallet) const
{
    std::lock_guard<std::mutex> lock(mu_);
    auto const it = by_id_.find(agent_id);
    return it != by_id_.end() && it->second.wallet == wallet;
}

uint64_t AgentStateTable::size() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return static_cast<uint64_t>(by_id_.size());
}

uint64_t AgentStateTable::version() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return version_;
}

uint64_t AgentStateTable::warm_from_triedb(::monad::TrieDb &tdb)
{
    // Pull everything off disk outside the lock; acquire only to swap in.
    auto loaded = tdb.enumerate_agents();
    uint64_t const count = loaded.size();

    std::lock_guard<std::mutex> lock(mu_);
    by_id_.clear();
    wallet_to_id_.clear();
    for (auto &&[id, record] : loaded) {
        wallet_to_id_[record.wallet] = id;
        by_id_.emplace(id, std::move(record));
    }
    ++version_;
    return count;
}

uint64_t AgentStateTable::warm_from_triedb(::monad::TrieRODb &tdb)
{
    // Pull everything off disk outside the lock; acquire only to swap in.
    auto loaded = tdb.enumerate_agents();
    uint64_t const count = loaded.size();

    std::lock_guard<std::mutex> lock(mu_);
    by_id_.clear();
    wallet_to_id_.clear();
    for (auto &&[id, record] : loaded) {
        wallet_to_id_[record.wallet] = id;
        by_id_.emplace(id, std::move(record));
    }
    ++version_;
    return count;
}

void AgentStateTable::export_to_commit(::monad::CommitBuilder &builder) const
{
    // Snapshot under the lock so concurrent writers can't mutate mid-copy.
    std::map<bytes32_t, AgentRecord> snapshot;
    {
        std::lock_guard<std::mutex> lock(mu_);
        snapshot = by_id_;
    }
    builder.add_agent_state(snapshot);
}

bytes32_t AgentStateTable::compute_fingerprint() const
{
    std::lock_guard<std::mutex> lock(mu_);

    // Empty table => all-zeros fingerprint.
    if (by_id_.empty()) {
        return bytes32_t{};
    }

    // Record serialization: 32 (agent_id) + 20 (wallet) + 1 (tier) + 2
    // (reputation big-endian) = 55 bytes per record. std::map iterates
    // in sorted order by key, which yields determinism.
    byte_string buf;
    buf.reserve(by_id_.size() * 55);
    for (auto const &[id, record] : by_id_) {
        buf.insert(buf.end(), id.bytes, id.bytes + 32);
        buf.insert(
            buf.end(), record.wallet.bytes, record.wallet.bytes + 20);
        buf.push_back(static_cast<uint8_t>(record.tier));
        buf.push_back(
            static_cast<uint8_t>((record.reputation >> 8) & 0xff));
        buf.push_back(static_cast<uint8_t>(record.reputation & 0xff));
    }

    auto const digest = ::monad::keccak256(buf);
    bytes32_t out;
    std::memcpy(out.bytes, digest.bytes, 32);
    return out;
}

bytes32_t AgentStateTable::compute_root() const
{
    return compute_fingerprint();
}

} // namespace meritium
MONAD_NAMESPACE_END
