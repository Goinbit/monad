// Copyright (C) 2025 Category Labs, Inc.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#pragma once

#include <category/core/byte_string.hpp>
#include <category/core/bytes.hpp>
#include <category/core/config.hpp>
#include <category/execution/ethereum/agent_state.hpp>
#include <category/execution/ethereum/state2/state_deltas.hpp>
#include <category/mpt/update.hpp>

#include <deque>
#include <map>
#include <vector>

MONAD_NAMESPACE_BEGIN

struct CallFrame;
struct Transaction;
struct BlockHeader;
struct Receipt;
struct Withdrawal;

class CommitBuilder
{
    std::deque<mpt::Update> update_alloc_;
    std::deque<byte_string> bytes_alloc_;
    std::deque<hash256> hash_alloc_;
    mpt::UpdateList updates_;
    uint64_t block_number_;

public:
    explicit CommitBuilder(uint64_t block_number);

    CommitBuilder &add_state_deltas(StateDeltas const &);

    CommitBuilder &add_code(Code const &);

    CommitBuilder &add_receipts(std::vector<Receipt> const &);

    CommitBuilder &add_transactions(
        std::vector<Transaction> const &, std::vector<Address> const &);

    CommitBuilder &add_call_frames(std::vector<std::vector<CallFrame>> const &);

    CommitBuilder &add_ommers(std::vector<BlockHeader> const &);

    CommitBuilder &add_withdrawals(std::vector<Withdrawal> const &);

    CommitBuilder &add_block_header(BlockHeader const &);

    // Meritium (M5): persist the Agent state partition under AGENT_NIBBLE.
    // Each record is RLP-encoded (see rlp::encode_agent_record) and written
    // with agent_id as the 32-byte key. Passing an empty map still installs
    // the empty subtrie (for determinism on chains with no agents yet).
    CommitBuilder &add_agent_state(
        std::map<bytes32_t, meritium::AgentRecord> const &agents);

    // Consumes updates_ but preserves the backing deque storage. New updates
    // can be added after a build() call (e.g. add_block_header between the
    // two commit stages), but previously built updates are not retained.
    mpt::UpdateList build(mpt::NibblesView);
};

MONAD_NAMESPACE_END
