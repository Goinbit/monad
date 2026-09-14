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
#include <category/execution/ethereum/core/address.hpp>
#include <category/execution/ethereum/core/transaction.hpp>
#include <category/execution/monad/staking/util/constants.hpp>
#include <category/execution/monad/system_sender.hpp>
#include <category/execution/monad/validate_monad_block.hpp>
#include <monad/test/traits_test.hpp>

#include <algorithm>
#include <array>
#include <vector>

#include <gtest/gtest.h>

using namespace monad;

namespace
{
    constexpr Address AGENT_REWARD_CA = Address{0x1001};
    constexpr std::array<unsigned char, 20> PROTOCOL_FUND_WALLET = {
        0x3c, 0xa5, 0x51, 0x97, 0xa2, 0x58, 0xfb, 0xc3, 0xab, 0xfd,
        0xb3, 0xf3, 0xd8, 0x70, 0xd7, 0xcc, 0xa8, 0xb5, 0x2d, 0xec};
    constexpr std::array<unsigned char, 20> AGENT_WALLET = {
        0xef, 0x08, 0xab, 0xd8, 0x6c, 0x98, 0x3e, 0xad, 0x29, 0x08,
        0x66, 0x3d, 0x11, 0x15, 0xfb, 0x8a, 0xbe, 0x4e, 0xf1, 0x76};

    Transaction make_agent_reward_tx(
        uint256_t value, std::array<unsigned char, 20> const &wallet)
    {
        Transaction tx{};
        tx.value = value;
        tx.to = AGENT_REWARD_CA;
        tx.data = byte_string(36, 0x00);
        std::copy_n(wallet.begin(), wallet.size(), tx.data.data() + 16);
        return tx;
    }
}

TYPED_TEST(MonadTraitsTest, no_system_txns)
{
    std::vector<Address> senders{
        0xaaaa_address,
        0xbbbb_address,
        0xcccc_address,
    };
    std::vector<Transaction> txns(senders.size());
    auto const res =
        static_validate_monad_body<typename TestFixture::Trait>(senders, txns);
    EXPECT_FALSE(res.has_error());
}

TYPED_TEST(MonadTraitsTest, multiple_system_txns_ok)
{
    std::vector<Address> senders{
        SYSTEM_SENDER,
        SYSTEM_SENDER,
        0xaaaa_address,
        0xbbbb_address,
    };
    std::vector<Transaction> txns(senders.size());
    txns[1].value = 25 * staking::MON;
    auto const res =
        static_validate_monad_body<typename TestFixture::Trait>(senders, txns);
    EXPECT_FALSE(res.has_error());
}

TYPED_TEST(MonadTraitsTest, system_txn_comes_after_user_txn)
{
    using Trait = typename TestFixture::Trait;

    std::vector<Address> senders{
        0xaaaa_address,
        0xbbbb_address,
        SYSTEM_SENDER,
        0xcccc_address,
    };
    std::vector<Transaction> txns(senders.size());
    auto const res =
        static_validate_monad_body<typename TestFixture::Trait>(senders, txns);
    if constexpr (Trait::monad_rev() < MONAD_FOUR) {
        EXPECT_FALSE(res.has_error());
    }
    else {
        ASSERT_TRUE(res.has_error());
        EXPECT_EQ(
            res.error(), MonadBlockError::SystemTransactionNotFirstInBlock);
    }
}

TYPED_TEST(MonadTraitsTest, multiple_reward_txns_ok_within_maximum)
{
    std::vector<Address> senders{
        SYSTEM_SENDER,
        SYSTEM_SENDER,
        0xaaaa_address,
        0xbbbb_address,
    };
    std::vector<Transaction> txns(senders.size());
    txns[0].value = 24 * staking::MON;
    txns[1].value = 1 * staking::MON;
    auto const res =
        static_validate_monad_body<typename TestFixture::Trait>(senders, txns);
    EXPECT_FALSE(res.has_error());
}

TYPED_TEST(MonadTraitsTest, reward_txn_exceeds_maximum)
{
    using Trait = typename TestFixture::Trait;

    std::vector<Address> senders{
        SYSTEM_SENDER,
        0xaaaa_address,
    };
    std::vector<Transaction> txns(senders.size());
    txns[0].value = 26 * staking::MON;
    auto const res =
        static_validate_monad_body<typename TestFixture::Trait>(senders, txns);
    if constexpr (Trait::monad_rev() < MONAD_FOUR) {
        EXPECT_FALSE(res.has_error());
    }
    else {
        ASSERT_TRUE(res.has_error());
        EXPECT_EQ(res.error(), MonadBlockError::InvalidRewardValue);
    }
}

TYPED_TEST(MonadTraitsTest, meritium_phase_a_reward_ratio_valid)
{
    std::vector<Address> senders{
        SYSTEM_SENDER,
        SYSTEM_SENDER,
        SYSTEM_SENDER,
        SYSTEM_SENDER,
        SYSTEM_SENDER,
        SYSTEM_SENDER,
        0xaaaa_address,
    };
    std::vector<Transaction> txns(senders.size());
    txns[0] = make_agent_reward_tx(594, AGENT_WALLET);
    txns[1] = make_agent_reward_tx(594, AGENT_WALLET);
    txns[2] = make_agent_reward_tx(594, AGENT_WALLET);
    txns[3] = make_agent_reward_tx(594, AGENT_WALLET);
    txns[4] = make_agent_reward_tx(594, AGENT_WALLET);
    txns[5] = make_agent_reward_tx(30, PROTOCOL_FUND_WALLET);
    auto const res =
        static_validate_monad_body<typename TestFixture::Trait>(senders, txns);

    EXPECT_FALSE(res.has_error());
}

TYPED_TEST(MonadTraitsTest, meritium_phase_a_reward_ratio_invalid)
{
    using Trait = typename TestFixture::Trait;

    std::vector<Address> senders{
        SYSTEM_SENDER,
        SYSTEM_SENDER,
        SYSTEM_SENDER,
        SYSTEM_SENDER,
        SYSTEM_SENDER,
        SYSTEM_SENDER,
        0xaaaa_address,
    };
    std::vector<Transaction> txns(senders.size());
    txns[0] = make_agent_reward_tx(594, AGENT_WALLET);
    txns[1] = make_agent_reward_tx(594, AGENT_WALLET);
    txns[2] = make_agent_reward_tx(594, AGENT_WALLET);
    txns[3] = make_agent_reward_tx(594, AGENT_WALLET);
    txns[4] = make_agent_reward_tx(594, AGENT_WALLET);
    txns[5] = make_agent_reward_tx(1, PROTOCOL_FUND_WALLET);
    auto const res =
        static_validate_monad_body<typename TestFixture::Trait>(senders, txns);
    if constexpr (Trait::monad_rev() < MONAD_FOUR) {
        EXPECT_FALSE(res.has_error());
    }
    else {
        ASSERT_TRUE(res.has_error());
        EXPECT_EQ(res.error(), MonadBlockError::InvalidRewardValue);
    }
}

TYPED_TEST(MonadTraitsTest, meritium_phase_a_missing_protocol_fund_fails)
{
    using Trait = typename TestFixture::Trait;

    std::vector<Address> senders{
        SYSTEM_SENDER,
        SYSTEM_SENDER,
        0xaaaa_address,
    };
    std::vector<Transaction> txns(senders.size());
    txns[0] = make_agent_reward_tx(594, AGENT_WALLET);
    txns[1] = make_agent_reward_tx(594, AGENT_WALLET);
    auto const res =
        static_validate_monad_body<typename TestFixture::Trait>(senders, txns);
    if constexpr (Trait::monad_rev() < MONAD_FOUR) {
        EXPECT_FALSE(res.has_error());
    }
    else {
        ASSERT_TRUE(res.has_error());
        EXPECT_EQ(res.error(), MonadBlockError::InvalidRewardValue);
    }
}
