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

#include <category/core/bytes.hpp>
#include <category/core/config.hpp>
#include <category/execution/ethereum/core/block.hpp>
#include <category/execution/ethereum/core/receipt.hpp>
#include <category/execution/ethereum/core/transaction.hpp>
#include <category/execution/ethereum/db/db.hpp>
#include <category/execution/ethereum/state2/state_deltas.hpp>
#include <category/execution/ethereum/trace/call_tracer.hpp>
#include <category/execution/ethereum/types/incarnation.hpp>
#include <category/vm/vm.hpp>

#include <ankerl/unordered_dense.h>

#include <memory>
#include <vector>

MONAD_NAMESPACE_BEGIN

class State;

/// Storage slot reads salvaged from accounts that were SELFDESTRUCTed
/// in this block. See `BlockState::self_destruct_storage_reads_`.
using SelfDestructStorageReads = ankerl::unordered_dense::segmented_map<
    Address, ankerl::unordered_dense::segmented_set<bytes32_t>>;

class BlockState final
{
    Db &db_;
    vm::VM &vm_;
    std::unique_ptr<StateDeltas> state_;
    Code code_;
    /// Every bytecode handed out by `read_code` since the BlockState was
    /// constructed. Populated unconditionally so the post-block witness
    /// generator has the complete set of code preimages the EVM consumed —
    /// including pre-existing contracts (which are not deployed this
    /// block, so do not show up in `code_`) and self-destructed contracts
    /// (whose accounts disappear before merge).
    ///
    /// NOTE: This is a superset of what a strictly minimal witness needs.
    /// Codes for contracts deployed *and* used inside the same block can
    /// be reconstructed by the verifier from the CREATE that produced
    /// them, so they don't need to be in the witness's codes list. Witness
    /// generation may want to filter `read_codes_` down to the subset that
    /// existed in pre-state (e.g., by intersecting with `code_hash`es
    /// reachable from `state.original()` accounts) to produce the most
    /// compact witness.
    Code read_codes_;
    /// Storage slot reads done before an account got SELFDESTRUCTed in
    /// the same block. `merge()` clears `state_deltas[addr].storage` for
    /// destroyed accounts, which would erase these entries from the
    /// witness access set — a tx that reads slot N and then destroys
    /// the account would leave slot N off the witness's emit path, and
    /// the verifier would hit a HashStub when re-executing the same
    /// read. Stashed here just before `merge` clears, so the access set
    /// remains complete.
    SelfDestructStorageReads self_destruct_storage_reads_;

public:
    BlockState(Db &, vm::VM &);

    vm::VM &vm()
    {
        return vm_;
    }

    Code const &read_codes() const noexcept
    {
        return read_codes_;
    }

    auto const &self_destruct_storage_reads() const noexcept
    {
        return self_destruct_storage_reads_;
    }

    std::optional<Account> read_account(Address const &);

    bytes32_t read_storage(Address const &, Incarnation, bytes32_t const &key);

    vm::SharedVarcode read_code(bytes32_t const &);

    bool can_merge(State &) const;

    void merge(State const &);

    struct ReleasedState
    {
        std::unique_ptr<StateDeltas> state;
        Code code;
        Code read_codes;
        SelfDestructStorageReads self_destruct_storage_reads;
    };

    ReleasedState release() &&;

    void log_debug();
};

MONAD_NAMESPACE_END
