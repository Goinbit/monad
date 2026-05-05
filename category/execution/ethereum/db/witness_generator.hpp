// Copyright (C) 2025-26 Category Labs, Inc.
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
#include <category/core/config.hpp>
#include <category/execution/ethereum/state2/block_state.hpp>
#include <category/execution/ethereum/state2/state_deltas.hpp>
#include <category/mpt/node_cursor.hpp>

#include <cstdint>
#include <vector>

MONAD_MPT_NAMESPACE_BEGIN
class Db;
MONAD_MPT_NAMESPACE_END

MONAD_NAMESPACE_BEGIN

struct WitnessData
{
    /// Canonical Ethereum MPT RLP preimages, in arbitrary order. The
    /// verifier indexes them by keccak hash and reconstructs whatever
    /// subtree of the partial trie they describe.
    std::vector<byte_string> nodes;
    /// Bytecodes the EVM consumed during this block, as supplied by
    /// `BlockState::read_codes()`. Bytes are copied out of the
    /// SharedIntercode at witness-gen time so the witness owns them.
    std::vector<byte_string> codes;
};

/// Build a witness for the just-executed block by walking the live state
/// trie. The verifier replays the block against a `PartialTrieDb`
/// reconstructed from the witness; hash stubs for child references whose
/// preimages aren't present are synthesised by the decoder.
///
/// @param db                   Live MPT database holding the pre-state.
///                             Mutated only by traversal-time fiber I/O.
/// @param accounts_trie_root   Cursor at the accounts-trie root, obtained
///                             by the caller via
///                             `db.find(curr_root, prefix + state_nibbles,
///                                      block_number)`.
/// @param block_number         Block being witnessed; passed to
///                             `db.traverse_blocking`.
/// @param deltas               Touched account / slot set (every key the
///                             EVM read or wrote ends up here, since
///                             `State::original_` is folded into each
///                             `Delta<T>::first`). Also drives compression
///                             analysis: aggregated deletions identify
///                             branches that will compress at commit,
///                             whose surviving children must be in the
///                             witness.
/// @param read_codes           Every bytecode the EVM consumed, captured
///                             unconditionally by `BlockState::read_code`.
/// @param self_destruct_storage_reads  Storage slot reads salvaged from
///                             accounts that were SELFDESTRUCTed in this
///                             block. `BlockState::merge` clears
///                             `state_deltas[addr].storage` for destroyed
///                             accounts, so any slot reads done before
///                             the destruction would otherwise be dropped
///                             from `deltas`. Stashed in `BlockState`
///                             before the clear and merged into the
///                             access trie here so witness paths cover
///                             them.
WitnessData generate_witness(
    mpt::Db &db, mpt::NodeCursor const &accounts_trie_root,
    uint64_t block_number, StateDeltas const &deltas, Code const &read_codes,
    SelfDestructStorageReads const &self_destruct_storage_reads);

MONAD_NAMESPACE_END
