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

#include <category/core/config.hpp>
#include <category/core/result.hpp>
#include <category/execution/monad/core/monad_block.hpp>
#include <category/execution/monad/staking/util/constants.hpp>
#include <category/execution/monad/system_sender.hpp>
#include <category/execution/monad/validate_monad_block.hpp>
#include <category/vm/evm/explicit_traits.hpp>

#include <algorithm>
#include <cstring>

// TODO unstable paths between versions
#if __has_include(<boost/outcome/experimental/status-code/status-code/config.hpp>)
    #include <boost/outcome/experimental/status-code/status-code/config.hpp>
    #include <boost/outcome/experimental/status-code/status-code/generic_code.hpp>
#else
    #include <boost/outcome/experimental/status-code/config.hpp>
    #include <boost/outcome/experimental/status-code/generic_code.hpp>
#endif

#include <concepts>

MONAD_NAMESPACE_BEGIN

template <class MonadConsensusBlockHeader>
Result<void>
static_validate_consensus_header(MonadConsensusBlockHeader const &header)
{
    uint64_t const timestamp_s = uint64_t{header.timestamp_ns / 1'000'000'000};
    if (MONAD_UNLIKELY(timestamp_s != header.execution_inputs.timestamp)) {
        return MonadBlockError::TimestampMismatch;
    }

    if constexpr (std::same_as<
                      MonadConsensusBlockHeader,
                      MonadConsensusBlockHeaderV2>) {
        if (MONAD_UNLIKELY(
                uint256_t{header.base_fee} !=
                header.execution_inputs.base_fee_per_gas)) {
            return MonadBlockError::BaseFeeMismatch;
        }
    }

    return outcome::success();
}

EXPLICIT_MONAD_CONSENSUS_BLOCK_HEADER(static_validate_consensus_header);

template <Traits traits>
Result<void> static_validate_monad_body(
    std::span<Address const> const senders,
    std::span<Transaction const> const txns)
{
    MONAD_ASSERT(senders.size() == txns.size());

    if constexpr (traits::monad_rev() < MONAD_FOUR) {
        return outcome::success();
    }

    // Find the first user txn.
    auto const first_user_sender = std::find_if_not(
        senders.begin(), senders.end(), [](Address const &sender) {
            return sender == SYSTEM_SENDER;
        });

    // No other system txns should come after it.
    auto const bad_system_sender =
        std::find(first_user_sender, senders.end(), SYSTEM_SENDER);
    if (MONAD_UNLIKELY(bad_system_sender != senders.end())) {
        return MonadBlockError::SystemTransactionNotFirstInBlock;
    }

    auto const end_system_txn =
        txns.begin() + std::distance(senders.begin(), first_user_sender);
    auto const is_reward = [](Transaction const &tx) { return tx.value > 0; };

    // Meritium Phase A: allow multiple reward system transactions per block.
    // PoAW distributes the per-block reward across multiple recipients
    // (5 founder Agent wallets + protocol sustainability fund), so the
    // validator sums all reward values and enforces a cumulative upper
    // bound rather than the upstream Monad single-tx cap. The sum check
    // keeps the original inflation guarantee intact.
    constexpr uint256_t MAXIMUM_BLOCK_REWARD = 25 * staking::MON;
    constexpr Address AGENT_REWARD_CA = Address{0x1001};
    // 0x3Ca55197a258FBC3ABFDB3F3d870D7Cca8b52DEC
    constexpr uint8_t PROTOCOL_FUND_ADDRESS_BYTES[20] = {
        0x3c, 0xa5, 0x51, 0x97, 0xa2, 0x58, 0xfb, 0xc3, 0xab, 0xfd,
        0xb3, 0xf3, 0xd8, 0x70, 0xd7, 0xcc, 0xa8, 0xb5, 0x2d, 0xec};
    // Allow tiny integer-rounding drift when checking 99:1 split.
    constexpr uint256_t PROTOCOL_RATIO_TOLERANCE_WEI = 10;

    uint256_t total_reward = 0;
    uint256_t agent_reward_total = 0;
    uint256_t protocol_fund_total = 0;
    for (auto it = txns.begin(); it != end_system_txn; ++it) {
        if (is_reward(*it)) {
            total_reward += it->value;
            if (MONAD_UNLIKELY(total_reward > MAXIMUM_BLOCK_REWARD)) {
                return MonadBlockError::InvalidRewardValue;
            }

            if (it->to == AGENT_REWARD_CA) {
                // Meritium AgentReward calldata shape:
                // selector(4) + agent_id(32) + ... where wallet is
                // right-aligned in agent_id (last 20 bytes).
                if (MONAD_UNLIKELY(it->data.size() < 36)) {
                    return MonadBlockError::InvalidRewardValue;
                }

                auto const *const wallet_ptr = it->data.data() + 4 + 12;
                if (std::memcmp(
                        wallet_ptr, PROTOCOL_FUND_ADDRESS_BYTES, sizeof(PROTOCOL_FUND_ADDRESS_BYTES))
                    == 0) {
                    protocol_fund_total += it->value;
                } else {
                    agent_reward_total += it->value;
                }
            }
        }
    }

    if (agent_reward_total > 0 || protocol_fund_total > 0) {
        // Enforce ProtocolFund ≈ 1% of reward emissions:
        //   protocol_fund_total * 99 ~= agent_reward_total
        auto const lhs = protocol_fund_total * 99;
        auto const rhs = agent_reward_total;
        auto const diff = lhs > rhs ? lhs - rhs : rhs - lhs;
        if (MONAD_UNLIKELY(diff > PROTOCOL_RATIO_TOLERANCE_WEI)) {
            return MonadBlockError::InvalidRewardValue;
        }
    }

    return outcome::success();
}

EXPLICIT_MONAD_TRAITS(static_validate_monad_body);

MONAD_NAMESPACE_END

BOOST_OUTCOME_SYSTEM_ERROR2_NAMESPACE_BEGIN

std::initializer_list<
    quick_status_code_from_enum<monad::MonadBlockError>::mapping> const &
quick_status_code_from_enum<monad::MonadBlockError>::value_mappings()
{
    using monad::MonadBlockError;

    static std::initializer_list<mapping> const v = {
        {MonadBlockError::Success, "success", {errc::success}},
        {MonadBlockError::TimestampMismatch, "timestamp mismatch", {}},
        {MonadBlockError::BaseFeeMismatch, "base fee mismatch", {}},
        {MonadBlockError::SystemTransactionNotFirstInBlock,
         "system transaction not first in block",
         {}},
        {MonadBlockError::MultipleRewardTransactions,
         "multiple reward transactions",
         {}},
        {MonadBlockError::InvalidRewardValue, "invalid reward value", {}},
    };

    return v;
}

BOOST_OUTCOME_SYSTEM_ERROR2_NAMESPACE_END
