#include "../../src/managers/SeatActionContext.hpp"
#include "../../src/helpers/memory/Memory.hpp"
#include <gtest/gtest.h>
#include <stdexcept>

// The scope stores these identities without dereferencing them. No compositor,
// device, display or application is needed to test restoration on every exit.
class CSeatActionScopeTest : public ::testing::Test {
  protected:
    void SetUp() override {
        ASSERT_EQ(SeatInput::current(), nullptr);
        ASSERT_TRUE(SeatInput::physical());
    }

    void TearDown() override {
        EXPECT_EQ(SeatInput::current(), nullptr);
        EXPECT_TRUE(SeatInput::physical());
    }

    char m_firstIdentity  = 0;
    char m_secondIdentity = 0;
};

TEST_F(CSeatActionScopeTest, NestedTargetsRestoreTheirOwnPhysicalSource) {
    auto                    first  = sc<CSeatDesktop*>(sc<void*>(&m_firstIdentity));
    auto                    second = sc<CSeatDesktop*>(sc<void*>(&m_secondIdentity));
    SeatInput::CActionScope outer(first, false);
    ASSERT_EQ(SeatInput::current(), first);
    ASSERT_FALSE(SeatInput::physical());
    {
        SeatInput::CActionScope inner(second, true);
        EXPECT_EQ(SeatInput::current(), second);
        EXPECT_TRUE(SeatInput::physical());
    }
    EXPECT_EQ(SeatInput::current(), first);
    EXPECT_FALSE(SeatInput::physical());
}

TEST_F(CSeatActionScopeTest, ExceptionsRestoreThePreviousTarget) {
    auto                    first  = sc<CSeatDesktop*>(sc<void*>(&m_firstIdentity));
    auto                    second = sc<CSeatDesktop*>(sc<void*>(&m_secondIdentity));
    SeatInput::CActionScope outer(first, false);
    try {
        SeatInput::CActionScope inner(second, true);
        throw std::runtime_error("dispatch failed");
    } catch (const std::runtime_error&) {
        EXPECT_EQ(SeatInput::current(), first);
        EXPECT_FALSE(SeatInput::physical());
    }
}

TEST_F(CSeatActionScopeTest, UnspecifiedSourceInheritsThenRestores) {
    auto                    first  = sc<CSeatDesktop*>(sc<void*>(&m_firstIdentity));
    auto                    second = sc<CSeatDesktop*>(sc<void*>(&m_secondIdentity));
    SeatInput::CActionScope outer(first, true);
    {
        SeatInput::CActionScope inner(second);
        EXPECT_EQ(SeatInput::current(), second);
        EXPECT_TRUE(SeatInput::physical());
    }
    EXPECT_EQ(SeatInput::current(), first);
    EXPECT_TRUE(SeatInput::physical());
}

TEST_F(CSeatActionScopeTest, PrimaryScopeDoesNotLoseTheHiddenTarget) {
    auto                    first = sc<CSeatDesktop*>(sc<void*>(&m_firstIdentity));
    SeatInput::CActionScope outer(first, false);
    {
        SeatInput::CActionScope primary(nullptr);
        EXPECT_EQ(SeatInput::current(), nullptr);
        EXPECT_TRUE(SeatInput::physical());
    }
    EXPECT_EQ(SeatInput::current(), first);
    EXPECT_FALSE(SeatInput::physical());
}
