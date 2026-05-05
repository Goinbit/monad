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

#include <category/execution/ethereum/db/witness_generator.hpp>

#include <category/core/assert.h>
#include <category/core/byte_string.hpp>
#include <category/core/bytes.hpp>
#include <category/core/keccak.hpp>
#include <category/execution/ethereum/core/rlp/account_rlp.hpp>
#include <category/execution/ethereum/db/util.hpp>
#include <category/execution/ethereum/rlp/encode2.hpp>
#include <category/mpt/compute.hpp>
#include <category/mpt/db.hpp>
#include <category/mpt/merkle/compact_encode.hpp>
#include <category/mpt/merkle/node_reference.hpp>
#include <category/mpt/nibbles_view.hpp>
#include <category/mpt/node.hpp>
#include <category/mpt/traverse.hpp>
#include <category/mpt/util.hpp>

#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>

MONAD_ANONYMOUS_NAMESPACE_BEGIN

using ::monad::mpt::INVALID_BRANCH;
using ::monad::mpt::Nibbles;
using ::monad::mpt::NibblesView;
using ::monad::mpt::Node;

/// 16-ary nibble trie holding every access-set path the EVM consulted.
/// One linear tree spanning the whole canonical accounts trie + per-account
/// storage subtries: account paths land at depth 64, slot paths land at
/// depth 128. Driving `should_visit` is then O(1): "does the current
/// cursor have a child for this nibble?". Mere existence of a node means
/// some access path passes through it; `has_descent()` distinguishes
/// "stop here" from "descend further".
struct AccessNode
{
    std::array<std::unique_ptr<AccessNode>, 16> children{};
    /// True on every node strictly below the anchor of a `mark_deletion`
    /// walk — i.e. nodes that may end up as the surviving sibling when
    /// commit's `trie_delete` collapses a branch. The anchor itself
    /// (the receiver of `mark_deletion`) stays unmarked, so a slot
    /// zero-out under an otherwise-live account doesn't pollute
    /// emission of the accounts-trie path above the account leaf.
    bool deletion{false};

    bool has_descent() const noexcept
    {
        for (auto const &c : children) {
            if (c) {
                return true;
            }
        }
        return false;
    }

    /// Walk `path` from this node, creating children as needed, and
    /// return a reference to the terminal node. Returning the terminus
    /// lets callers chain marks (e.g. mark each slot relative to its
    /// owning account node, avoiding a re-walk from the root).
    AccessNode &mark(NibblesView const path)
    {
        AccessNode *cur = this;
        for (unsigned i = 0; i < path.nibble_size(); ++i) {
            auto &child = cur->children[path.get(i)];
            if (!child) {
                child = std::make_unique<AccessNode>();
            }
            cur = child.get();
        }
        return *cur;
    }

    /// Same shape as `mark`, but additionally tags every node walked
    /// into with `deletion=true`. The receiver is left unmarked. The
    /// returned terminus lets callers chain — e.g. selfdestruct calls
    /// `root.mark_deletion(addr_hash)` and then continues with
    /// per-slot reads on the returned account node.
    AccessNode &mark_deletion(NibblesView const path)
    {
        AccessNode *cur = this;
        for (unsigned i = 0; i < path.nibble_size(); ++i) {
            auto &child = cur->children[path.get(i)];
            if (!child) {
                child = std::make_unique<AccessNode>();
            }
            cur = child.get();
            cur->deletion = true;
        }
        return *cur;
    }
};

/// `bytes32_t` viewed as a 64-nibble sequence.
NibblesView nibbles_of(bytes32_t const &b)
{
    return NibblesView{byte_string_view{b.bytes, sizeof(b.bytes)}};
}

/// Build the access trie from `StateDeltas`. Every key in the deltas map
/// was touched (read or written) during the block — `State::original_`
/// reads are folded into each `Delta<T>::first` at merge time, so the key
/// set covers reads as well as writes.
AccessNode build_access_trie(
    StateDeltas const &deltas,
    SelfDestructStorageReads const &self_destruct_storage_reads)
{
    AccessNode root;
    for (auto const &[addr, state_delta] : deltas) {
        bytes32_t const addr_hash =
            to_bytes(keccak256({addr.bytes, sizeof(addr.bytes)}));
        auto const &account = state_delta.account;
        bool const selfdestruct =
            account.first.has_value() && !account.second.has_value();
        AccessNode &acct_node = selfdestruct
                                    ? root.mark_deletion(nibbles_of(addr_hash))
                                    : root.mark(nibbles_of(addr_hash));
        // Selfdestruct wipes the storage subtree wholesale at commit;
        // per-slot deletion marking is moot. Slots read pre-SELFDESTRUCT
        // are tracked separately in `self_destruct_storage_reads` below
        // and still need plain `mark` for pre-state replay.
        if (!selfdestruct) {
            for (auto const &[slot, sdelta] : state_delta.storage) {
                bytes32_t const slot_hash =
                    to_bytes(keccak256({slot.bytes, sizeof(slot.bytes)}));
                bool const zero_out =
                    sdelta.first != bytes32_t{} && sdelta.second == bytes32_t{};
                if (zero_out) {
                    acct_node.mark_deletion(nibbles_of(slot_hash));
                }
                else {
                    acct_node.mark(nibbles_of(slot_hash));
                }
            }
        }
    }
    // Slots read before SELFDESTRUCT cleared them out of `deltas`.
    for (auto const &[addr, slots] : self_destruct_storage_reads) {
        bytes32_t const addr_hash =
            to_bytes(keccak256({addr.bytes, sizeof(addr.bytes)}));
        AccessNode &acct_node = root.mark(nibbles_of(addr_hash));
        for (auto const &slot : slots) {
            bytes32_t const slot_hash =
                to_bytes(keccak256({slot.bytes, sizeof(slot.bytes)}));
            acct_node.mark(nibbles_of(slot_hash));
        }
    }
    return root;
}

// Canonical Ethereum MPT helpers for witness emission. The mpt `Compute`
// machinery folds RLP construction with hashing via `to_node_reference`,
// so it can't return the bare RLP. These mirror that logic but stop one
// step short of hashing.

byte_string rlp_encode_account(Node const &node)
{
    MONAD_ASSERT(node.has_value());
    auto enc = node.value();
    auto const acct = decode_account_db_ignore_address(enc);
    MONAD_ASSERT(!acct.has_error());
    bytes32_t storage_root = NULL_ROOT;
    if (node.number_of_children()) {
        MONAD_ASSERT(node.data().size() == sizeof(bytes32_t));
        std::copy_n(node.data().data(), sizeof(bytes32_t), storage_root.bytes);
    }
    return rlp::encode_account(acct.value(), storage_root);
}

byte_string rlp_encode_storage(Node const &node)
{
    MONAD_ASSERT(node.has_value());
    auto enc = node.value();
    auto const compact_val = decode_storage_db_ignore_slot(enc);
    MONAD_ASSERT(!compact_val.has_error());
    return rlp::encode_string2(compact_val.value());
}

byte_string rlp_encode_list2(
    NibblesView const path, byte_string_view const second, bool const has_value)
{
    constexpr size_t max_compact_encode_size = KECCAK256_SIZE + 1;

    MONAD_ASSERT(path.data_size() <= KECCAK256_SIZE);

    unsigned char path_arr[max_compact_encode_size] = {0};
    auto const first = mpt::compact_encode(path_arr, path, has_value);
    MONAD_ASSERT(first.size() <= max_compact_encode_size);
    // leaf and hashed node ref requires rlp encoding,
    // rlp encoded but unhashed branch node ref doesn't
    bool const need_encode_second =
        has_value || second.size() >= KECCAK256_SIZE;
    auto const concat_len =
        rlp::string_length(first) +
        (need_encode_second ? rlp::string_length(second) : second.size());
    byte_string concat_rlp(concat_len, 0);
    auto result = rlp::encode_string(concat_rlp, first);
    result = need_encode_second ? rlp::encode_string(result, second) : [&] {
        memcpy(result.data(), second.data(), second.size());
        return result.subspan(second.size());
    }();
    MONAD_ASSERT(
        (unsigned long)(result.data() - concat_rlp.data()) == concat_len);

    byte_string rlp(rlp::list_length(concat_len), 0);
    rlp::encode_list(rlp, {concat_rlp.data(), concat_rlp.size()});
    return rlp;
}

byte_string rlp_encode_branch(Node const &node)
{
    // Matches mpt::MerkleComputeBase::max_branch_rlp_size.
    constexpr unsigned MAX_BRANCH_RLP = 532;
    unsigned char buf[MAX_BRANCH_RLP];
    auto tail = mpt::encode_16_children(node, {buf, MAX_BRANCH_RLP});
    tail = mpt::encode_empty_string(tail);
    auto const len = static_cast<size_t>(tail.data() - buf);
    return rlp::encode_list2(byte_string{buf, len});
}

/// Emits canonical Ethereum MPT RLP for every node visited in the live
/// accounts trie that lies on a path to a touched (account or slot) leaf.
/// Hash stubs for unvisited siblings are synthesised by the verifier from
/// the live parent's child-data view — the producer simply doesn't emit
/// them.
///
/// The access trie cursor follows the live traversal in lockstep:
/// `should_visit(branch)` reduces to "does the current cursor have a
/// child for this nibble?".
class WitnessEmitMachine final : public mpt::TraverseMachine
{
    std::vector<byte_string> &out_nodes_;

    struct Frame
    {
        /// Cursor into the access trie, recording touched parts of the MPT in
        /// the execution of a block. `nullptr` means this subtree was not
        /// accesssed in the block execution
        AccessNode const *cursor;

        unsigned consumed_nibbles{0};
        bool single_branch{false};
        uint8_t sibling_count{0};
        /// initially popcount of node.mask at this level — total live children.
        uint8_t live_count{0};
        /// Backing store for `iterator()`'s returned span.
        std::array<std::pair<uint8_t, unsigned char>, 16> iter_buf;
    };

    /// `std::deque` rather than `std::vector` because the engine holds a
    /// span into `frames_.back().iter_buf` across recursive `down()`
    /// calls. A vector reallocation on push_back would invalidate that
    /// span; deque has stable references under push_back/pop_back.
    std::deque<Frame> frames_;

public:
    WitnessEmitMachine(
        AccessNode const &root, std::vector<byte_string> &out_nodes)
        : out_nodes_{out_nodes}
    {
        frames_.emplace_back(&root);
    }

    std::span<std::pair<uint8_t, unsigned char> const>
    iterator(uint16_t const mask) override
    {
        size_t n = 0;
        auto &frame = frames_.back();

        if (frame.cursor) {
            for (auto const [idx, b] : mpt::NodeChildrenRange(mask)) {
                auto const &c = frame.cursor->children[b];
                if (c && c->deletion) {
                    frame.iter_buf[n++] = {idx, b};
                }
            }
            for (auto const [idx, b] : mpt::NodeChildrenRange(mask)) {
                auto const &c = frame.cursor->children[b];
                if (!(c && c->deletion)) {
                    frame.iter_buf[n++] = {idx, b};
                }
            }
        }
        else {
            for (auto const [idx, b] : mpt::NodeChildrenRange(mask)) {
                frame.iter_buf[n++] = {idx, b};
            }
        }
        MONAD_ASSERT(n == frame.sibling_count);

        return {frame.iter_buf.data(), n};
    }

    /// Walk the access cursor down by `path` nibbles. Always advances
    /// `consumed_nibbles` by the full path length; the access cursor
    /// may go null mid-walk (no further descent possible from here),
    /// but depth tracking must still reflect the live trie's actual
    /// position so dispatch picks the right branch.
    void walk_cursor(NibblesView const path)
    {
        auto &frame = frames_.back();
        for (unsigned i = 0; i < path.nibble_size(); ++i) {
            if (frame.cursor != nullptr) {
                frame.cursor = frame.cursor->children[path.get(i)].get();
            }
            ++frame.consumed_nibbles;
        }
    }

    void walk_cursor(unsigned char const nibble)
    {
        auto &frame = frames_.back();
        if (frame.cursor != nullptr) {
            frame.cursor = frame.cursor->children[nibble].get();
        }
        ++frame.consumed_nibbles;
    }

    bool should_visit(Node const &, unsigned char const branch) override
    {
        auto &frame = frames_.back();
        // Force descent into a stashed fusion target. `single_branch`
        // is set when a single-child node needs to be fused with its only
        // child to form a canonical extension/leaf.
        if (frame.single_branch) {
            return true;
        }
        if (frame.cursor != nullptr &&
            frame.cursor->children[branch] != nullptr) {
            return true;
        }
        if (frame.live_count == 1 && frame.sibling_count > frame.live_count) {
            return true;
        }
        return false;
    }

    void rlp_encode_ext_or_branch(Node const &node, NibblesView const ext_path)
    {
        MONAD_ASSERT(node.number_of_children() > 0);
        if (node.number_of_children() == 1) {
            MONAD_ASSERT(ext_path.nibble_size() > 0);
            out_nodes_.push_back(rlp_encode_list2(
                ext_path,
                node.child_data_view(0),
                /*has_value=*/false));
            return;
        }

        byte_string branch_rlp = rlp_encode_branch(node);

        if (ext_path.nibble_size() > 0) {
            // branch wrapped in an extension
            unsigned char branch_ref[KECCAK256_SIZE];
            unsigned const branch_ref_len =
                mpt::to_node_reference(branch_rlp, branch_ref);
            out_nodes_.push_back(rlp_encode_list2(
                ext_path,
                {branch_ref, branch_ref_len},
                /*has_value=*/false));
        }

        out_nodes_.push_back(std::move(branch_rlp));
    }

    bool down(unsigned char const branch_nibble, Node const &node) override
    {
        if (branch_nibble == INVALID_BRANCH) {

            MONAD_ASSERT(node.path_nibble_view().nibble_size() == 0);
            MONAD_ASSERT(node.has_value() && node.value().empty());
            MONAD_ASSERT(frames_.back().consumed_nibbles == 0);
            // empty accounts trie
            if (node.number_of_children() == 0) {
                return false;
            }
            if (node.number_of_children() == 1) {
                auto &root_frame = frames_.back();
                root_frame.sibling_count =
                    static_cast<uint8_t>(__builtin_popcount(node.mask));
                root_frame.live_count = root_frame.sibling_count;
                root_frame.single_branch = true;
                return true;
            }
        }
        else {
            frames_.push_back(auto(frames_.back()));
            walk_cursor(branch_nibble);
        }

        walk_cursor(node.path_nibble_view());

        auto &frame = frames_.back();
        frame.sibling_count =
            static_cast<uint8_t>(__builtin_popcount(node.mask));
        frame.live_count = frame.sibling_count;

        Nibbles const leaf_or_ext_path =
            frame.single_branch ? concat(branch_nibble, node.path_nibble_view())
                                : Nibbles{node.path_nibble_view()};

        constexpr unsigned ACCOUNT_LEAF_DEPTH = KECCAK256_SIZE * 2;

        // Walking account trie
        if (frame.consumed_nibbles < ACCOUNT_LEAF_DEPTH) {
            rlp_encode_ext_or_branch(node, leaf_or_ext_path);
        }
        else if (frame.consumed_nibbles == ACCOUNT_LEAF_DEPTH) {
            MONAD_ASSERT(node.has_value());
            out_nodes_.push_back(rlp_encode_list2(
                leaf_or_ext_path,
                rlp_encode_account(node),
                /*has_value=*/true));
            if (node.number_of_children() > 1) {
                out_nodes_.push_back(rlp_encode_branch(node));
            }
        }
        // Walking account storage trie
        else if (frame.consumed_nibbles < 2 * ACCOUNT_LEAF_DEPTH) {
            rlp_encode_ext_or_branch(node, leaf_or_ext_path);
        }
        else {
            MONAD_ASSERT(frame.consumed_nibbles == 2 * ACCOUNT_LEAF_DEPTH);
            MONAD_ASSERT(node.has_value() && !node.value().empty());
            MONAD_ASSERT(node.number_of_children() == 0);
            out_nodes_.push_back(rlp_encode_list2(
                leaf_or_ext_path,
                rlp_encode_storage(node),
                /*has_value=*/true));
        }

        frame.single_branch = node.number_of_children() == 1;

        return true;
    }

    void up(unsigned char const, Node const &node) override
    {
        auto &frame = frames_.back();
        bool branch_deleted = false;
        if (frame.cursor && frame.cursor->deletion) {
            // node.has_value()=true means leaf-shape (account leaf at
            // depth 64 or storage leaf at depth 128). On a deletion path
            // the leaf itself is the deletion target — selfdestruct
            // wipes the whole account subtree wholesale at commit, slot
            // zero-out wipes the leaf.
            if (node.has_value()) {
                branch_deleted = true;
            }
            else {
                // Internal node within a deletion subtree — deleted iff
                // every live child was also deleted.
                branch_deleted = (frame.live_count == 0);
            }
        }
        frames_.pop_back();
        if (branch_deleted && !frames_.empty()) {
            --frames_.back().live_count;
        }
    }

    std::unique_ptr<mpt::TraverseMachine> clone() const override
    {
        return std::make_unique<WitnessEmitMachine>(*this);
    }
};

MONAD_ANONYMOUS_NAMESPACE_END

MONAD_NAMESPACE_BEGIN

WitnessData generate_witness(
    mpt::Db &db, mpt::NodeCursor const &accounts_trie_root,
    uint64_t const block_number, StateDeltas const &deltas,
    Code const &read_codes,
    SelfDestructStorageReads const &self_destruct_storage_reads)
{
    WitnessData wd;

    AccessNode const access_root =
        build_access_trie(deltas, self_destruct_storage_reads);

    // TODO(witness_generator): compression analysis pass — derive
    // surviving children of branches that will compress at commit and
    // extend `access_root` with their full subtree prefixes before
    // traversing.

    if (accounts_trie_root.is_valid() && access_root.has_descent()) {
        WitnessEmitMachine machine{access_root, wd.nodes};
        MONAD_ASSERT(db.traverse_blocking<true>(
            accounts_trie_root, machine, block_number));
    }

    wd.codes.reserve(read_codes.size());
    for (auto const &[_hash, intercode] : read_codes) {
        auto const span = intercode->code_span();
        wd.codes.emplace_back(span.begin(), span.end());
    }

    return wd;
}

MONAD_NAMESPACE_END
