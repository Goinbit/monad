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

#include <category/core/byte_string.hpp>
#include <category/core/assert.h>
#include <category/core/int.hpp>
#include <category/core/likely.h>
#include <category/core/result.hpp>
#include <category/core/rlp/config.hpp>
#include <category/execution/ethereum/core/block.hpp>
#include <category/execution/ethereum/core/rlp/address_rlp.hpp>
#include <category/execution/ethereum/core/rlp/block_rlp.hpp>
#include <category/execution/ethereum/core/rlp/bytes_rlp.hpp>
#include <category/execution/ethereum/core/rlp/int_rlp.hpp>
#include <category/execution/ethereum/core/rlp/receipt_rlp.hpp>
#include <category/execution/ethereum/core/rlp/transaction_rlp.hpp>
#include <category/execution/ethereum/core/rlp/withdrawal_rlp.hpp>
#include <category/execution/ethereum/core/transaction.hpp>
#include <category/execution/ethereum/rlp/decode.hpp>
#include <category/execution/ethereum/rlp/decode_error.hpp>
#include <category/execution/ethereum/rlp/encode2.hpp>

#include <boost/outcome/try.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

MONAD_RLP_NAMESPACE_BEGIN

byte_string encode_block_header(BlockHeader const &block_header)
{
    byte_string encoded_block_header;
    encoded_block_header += encode_bytes32(block_header.parent_hash);
    encoded_block_header += encode_bytes32(block_header.ommers_hash);
    encoded_block_header += encode_address(block_header.beneficiary);
    encoded_block_header += encode_bytes32(block_header.state_root);
    encoded_block_header += encode_bytes32(block_header.transactions_root);
    encoded_block_header += encode_bytes32(block_header.receipts_root);
    encoded_block_header += encode_bloom(block_header.logs_bloom);
    encoded_block_header += encode_unsigned(block_header.difficulty);
    encoded_block_header += encode_unsigned(block_header.number);
    encoded_block_header += encode_unsigned(block_header.gas_limit);
    encoded_block_header += encode_unsigned(block_header.gas_used);
    encoded_block_header += encode_unsigned(block_header.timestamp);
    encoded_block_header += encode_string2(block_header.extra_data);
    encoded_block_header += encode_bytes32(block_header.prev_randao);
    encoded_block_header +=
        encode_string2(to_byte_string_view(block_header.nonce));

    if (block_header.base_fee_per_gas.has_value()) {
        encoded_block_header +=
            encode_unsigned(block_header.base_fee_per_gas.value());
    }

    if (block_header.withdrawals_root.has_value()) {
        encoded_block_header +=
            encode_bytes32(block_header.withdrawals_root.value());
    }

    if (block_header.blob_gas_used.has_value()) {
        encoded_block_header +=
            encode_unsigned(block_header.blob_gas_used.value());
    }
    if (block_header.excess_blob_gas.has_value()) {
        encoded_block_header +=
            encode_unsigned(block_header.excess_blob_gas.value());
    }
    if (block_header.parent_beacon_block_root.has_value()) {
        encoded_block_header +=
            encode_bytes32(block_header.parent_beacon_block_root.value());
    }

    if (block_header.requests_hash.has_value()) {
        encoded_block_header +=
            encode_bytes32(block_header.requests_hash.value());
    }
    bool const has_agent_extension =
        block_header.agent_state_root.has_value() ||
        block_header.agent_tx_count.has_value() ||
        block_header.active_agent_count.has_value() ||
        block_header.total_tasks_completed.has_value();
    if (has_agent_extension) {
        MONAD_ASSERT(block_header.agent_state_root.has_value());
        MONAD_ASSERT(block_header.agent_tx_count.has_value());
        MONAD_ASSERT(block_header.active_agent_count.has_value());
        MONAD_ASSERT(block_header.total_tasks_completed.has_value());
        encoded_block_header +=
            encode_bytes32(block_header.agent_state_root.value());
        encoded_block_header +=
            encode_unsigned(block_header.agent_tx_count.value());
        encoded_block_header +=
            encode_unsigned(block_header.active_agent_count.value());
        encoded_block_header +=
            encode_unsigned(block_header.total_tasks_completed.value());
    }

    return encode_list2(encoded_block_header);
}

byte_string encode_ommers(std::vector<BlockHeader> const &ommers)
{
    byte_string encoded;
    for (auto const &ommer : ommers) {
        encoded += rlp::encode_block_header(ommer);
    }
    return rlp::encode_list2(encoded);
}

byte_string encode_block(Block const &block)
{
    byte_string const encoded_block_header = encode_block_header(block.header);
    byte_string encoded_block_transactions;

    for (auto const &tx : block.transactions) {
        if (tx.type == TransactionType::legacy) {
            encoded_block_transactions += encode_transaction(tx);
        }
        else {
            encoded_block_transactions +=
                encode_string2(encode_transaction(tx));
        }
    }
    encoded_block_transactions = encode_list2(encoded_block_transactions);

    byte_string encoded_block;
    encoded_block += encoded_block_header;
    encoded_block += encoded_block_transactions;
    encoded_block += encode_ommers(block.ommers);

    if (block.withdrawals.has_value()) {
        byte_string encoded_block_withdrawals;
        for (auto const &withdraw : block.withdrawals.value()) {
            encoded_block_withdrawals += encode_withdrawal(withdraw);
        }
        encoded_block += encode_list2(encoded_block_withdrawals);
    }

    return encode_list2(encoded_block);
}

Result<BlockHeader> decode_block_header(byte_string_view &enc)
{
    // extraData max length is 32, see YP [4.4 - The Block]
    constexpr size_t EXTRA_DATA_MAX_LENGTH = 32;

    BlockHeader block_header;
    BOOST_OUTCOME_TRY(auto payload, parse_list_metadata(enc));

    BOOST_OUTCOME_TRY(block_header.parent_hash, decode_bytes32(payload));
    BOOST_OUTCOME_TRY(block_header.ommers_hash, decode_bytes32(payload));
    BOOST_OUTCOME_TRY(block_header.beneficiary, decode_address(payload));
    BOOST_OUTCOME_TRY(block_header.state_root, decode_bytes32(payload));
    BOOST_OUTCOME_TRY(block_header.transactions_root, decode_bytes32(payload));
    BOOST_OUTCOME_TRY(block_header.receipts_root, decode_bytes32(payload));
    BOOST_OUTCOME_TRY(block_header.logs_bloom, decode_bloom(payload));
    BOOST_OUTCOME_TRY(
        block_header.difficulty, decode_unsigned<uint256_t>(payload));
    BOOST_OUTCOME_TRY(block_header.number, decode_unsigned<uint64_t>(payload));
    BOOST_OUTCOME_TRY(
        block_header.gas_limit, decode_unsigned<uint64_t>(payload));
    BOOST_OUTCOME_TRY(
        block_header.gas_used, decode_unsigned<uint64_t>(payload));
    BOOST_OUTCOME_TRY(
        block_header.timestamp, decode_unsigned<uint64_t>(payload));
    BOOST_OUTCOME_TRY(block_header.extra_data, decode_string(payload));
    if (block_header.extra_data.size() > EXTRA_DATA_MAX_LENGTH) {
        return DecodeError::Overflow;
    }
    BOOST_OUTCOME_TRY(block_header.prev_randao, decode_bytes32(payload));
    BOOST_OUTCOME_TRY(block_header.nonce, decode_byte_string_fixed<8>(payload));

    if (payload.size() > 0) {
        BOOST_OUTCOME_TRY(
            block_header.base_fee_per_gas, decode_unsigned<uint256_t>(payload));
        if (payload.size() > 0) {
            BOOST_OUTCOME_TRY(
                block_header.withdrawals_root, decode_bytes32(payload));
            if (payload.size() > 0) {
                BOOST_OUTCOME_TRY(
                    block_header.blob_gas_used,
                    decode_unsigned<uint64_t>(payload));
                BOOST_OUTCOME_TRY(
                    block_header.excess_blob_gas,
                    decode_unsigned<uint64_t>(payload));
                BOOST_OUTCOME_TRY(
                    block_header.parent_beacon_block_root,
                    decode_bytes32(payload));

                if (payload.size() > 0) {
                    // Four tail shapes are allowed (mirrors Rust
                    // ProposedEthHeader in monad-eth-types):
                    //   (a) requests_hash only        (EIP-7685)
                    //   (b) agent 4 fields only       (Meritium without Prague)
                    //   (c) requests_hash + agent 4   (Meritium on Prague)
                    //   (d) no tail                   (already ruled out above)
                    //
                    // The first item is always a bytes32 (either requests_hash
                    // or agent_state_root). Disambiguate by length-based
                    // speculation, not fixed magic byte counts.

                    bytes32_t first_hash;
                    BOOST_OUTCOME_TRY(first_hash, decode_bytes32(payload));

                    if (payload.size() == 0) {
                        // (a) requests_hash only
                        block_header.requests_hash = first_hash;
                    }
                    else {
                        // Speculate (b): first_hash is agent_state_root and
                        // three uint64s follow that consume all remaining
                        // bytes exactly.
                        byte_string_view try_agent_only = payload;
                        auto const agent_only = [&]() -> Result<std::tuple<
                                                           uint64_t,
                                                           uint64_t,
                                                           uint64_t>> {
                            BOOST_OUTCOME_TRY(
                                auto const tx_count,
                                decode_unsigned<uint64_t>(try_agent_only));
                            BOOST_OUTCOME_TRY(
                                auto const active,
                                decode_unsigned<uint64_t>(try_agent_only));
                            BOOST_OUTCOME_TRY(
                                auto const tasks,
                                decode_unsigned<uint64_t>(try_agent_only));
                            return std::make_tuple(tx_count, active, tasks);
                        }();

                        if (agent_only.has_value() &&
                            try_agent_only.size() == 0) {
                            auto const [tx_count, active, tasks] =
                                agent_only.value();
                            block_header.agent_state_root = first_hash;
                            block_header.agent_tx_count = tx_count;
                            block_header.active_agent_count = active;
                            block_header.total_tasks_completed = tasks;
                            payload = try_agent_only; // commit consumption
                        }
                        else {
                            // (c) requests_hash + agent 4 fields
                            block_header.requests_hash = first_hash;
                            BOOST_OUTCOME_TRY(
                                block_header.agent_state_root,
                                decode_bytes32(payload));
                            BOOST_OUTCOME_TRY(
                                block_header.agent_tx_count,
                                decode_unsigned<uint64_t>(payload));
                            BOOST_OUTCOME_TRY(
                                block_header.active_agent_count,
                                decode_unsigned<uint64_t>(payload));
                            BOOST_OUTCOME_TRY(
                                block_header.total_tasks_completed,
                                decode_unsigned<uint64_t>(payload));
                            if (payload.size() != 0) {
                                return DecodeError::InputTooLong;
                            }
                        }
                    }
                }
            }
        }
    }

    if (MONAD_UNLIKELY(!payload.empty())) {
        return DecodeError::InputTooLong;
    }

    return block_header;
}

Result<std::vector<BlockHeader>>
decode_block_header_vector(byte_string_view &enc)
{
    std::vector<BlockHeader> ommers;
    BOOST_OUTCOME_TRY(auto payload, parse_list_metadata(enc));

    while (payload.size() > 0) {
        BOOST_OUTCOME_TRY(auto ommer, decode_block_header(payload));
        ommers.emplace_back(std::move(ommer));
    }

    if (MONAD_UNLIKELY(!payload.empty())) {
        return DecodeError::InputTooLong;
    }

    return ommers;
}

Result<Block> decode_block(byte_string_view &enc)
{
    Block block;
    BOOST_OUTCOME_TRY(auto payload, parse_list_metadata(enc));

    BOOST_OUTCOME_TRY(block.header, decode_block_header(payload));
    BOOST_OUTCOME_TRY(block.transactions, decode_transaction_list(payload));
    BOOST_OUTCOME_TRY(block.ommers, decode_block_header_vector(payload));

    if (payload.size() > 0) {
        BOOST_OUTCOME_TRY(auto withdrawals, decode_withdrawal_list(payload));
        block.withdrawals.emplace(std::move(withdrawals));
    }

    if (MONAD_UNLIKELY(!payload.empty())) {
        return DecodeError::InputTooLong;
    }

    return block;
}

MONAD_RLP_NAMESPACE_END
