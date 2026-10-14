// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include <atomic>
#include <future>

#include <gtest/gtest.h>

#include <nx/network/aio/aio_service.h>
#include <nx/network/aio/basic_pollable.h>
#include <nx/network/socket_global.h>

namespace nx::network::aio::test {

class TestPollable: public aio::BasicPollable
{
    using base_type = aio::BasicPollable;

public:
    TestPollable():
        m_cleanupDone(false)
    {
    }

    ~TestPollable()
    {
        if (isInSelfAioThread())
            stopWhileInAioThread();
    }

    bool isCleanupDone() const { return m_cleanupDone; }

protected:
    virtual void stopWhileInAioThread() override
    {
        base_type::stopWhileInAioThread();
        m_cleanupDone = true;
    }

private:
    bool m_cleanupDone;
};

class BasicPollable: public ::testing::Test
{
public:
    BasicPollable() {}

    ~BasicPollable() { m_pollable.pleaseStopSync(); }

protected:
    TestPollable& pollable() { return m_pollable; }

    void recordAsyncCall()
    {
        m_callMadePromise.set_value(SocketGlobals::aioService().getCurrentAioThread());
    }

    void givenPollableBoundTo(AbstractAioThread* aioThread)
    {
        pollable().bindToAioThread(aioThread);
    }

    void whenPostedAsyncCall()
    {
        pollable().post(
            [this]()
            {
                recordAsyncCall();
            });
    }

    void assertIfAsyncCallHasNotBeenMade()
    {
        if (!m_callResult)
            m_callResult = m_callMadePromise.get_future().get();

        ASSERT_EQ(pollable().getAioThread(), *m_callResult);
    }

    void assertThatAsyncCallHasBeenMadeWithinCurrentThread()
    {
        thenAsyncCallShouldBeMadeWithinThread(SocketGlobals::aioService().getCurrentAioThread());
    }

    void thenAsyncCallShouldBeMadeWithinThread(AbstractAioThread* aioThread)
    {
        if (!m_callResult)
            m_callResult = m_callMadePromise.get_future().get();

        ASSERT_EQ(aioThread, *m_callResult);
    }

    void whenRequestedStop() { pollable().pleaseStopSync(); }

    void thenCleanupShouldOccur() { ASSERT_TRUE(pollable().isCleanupDone()); }

private:
    TestPollable m_pollable;
    std::promise<AbstractAioThread*> m_callMadePromise;
    std::optional<AbstractAioThread*> m_callResult;
};

TEST_F(BasicPollable, post)
{
    whenPostedAsyncCall();

    assertIfAsyncCallHasNotBeenMade();
}

TEST_F(BasicPollable, dispatch_within_non_aio_thread)
{
    pollable().dispatch(
        [this]()
        {
            recordAsyncCall();
        });

    assertIfAsyncCallHasNotBeenMade();
}

TEST_F(BasicPollable, dispatch_within_aio_thread)
{
    pollable().post(
        [this]()
        {
            pollable().dispatch(
                [this]()
                {
                    recordAsyncCall();
                });

            assertIfAsyncCallHasNotBeenMade();
            assertThatAsyncCallHasBeenMadeWithinCurrentThread();
        });
}

TEST_F(BasicPollable, bindToAioThread)
{
    const auto randomAioThread = SocketGlobals::aioService().getRandomAioThread();

    givenPollableBoundTo(randomAioThread);
    whenPostedAsyncCall();
    thenAsyncCallShouldBeMadeWithinThread(randomAioThread);
}

TEST_F(BasicPollable, getAioThread)
{
    const auto randomAioThread = SocketGlobals::aioService().getRandomAioThread();
    pollable().bindToAioThread(randomAioThread);
    ASSERT_EQ(randomAioThread, pollable().getAioThread());
}

TEST_F(BasicPollable, isInSelfAioThread)
{
    ASSERT_FALSE(pollable().isInSelfAioThread());

    std::promise<void> callMadePromise;
    pollable().post(
        [this, &callMadePromise]()
        {
            ASSERT_TRUE(pollable().isInSelfAioThread());
            callMadePromise.set_value();
        });
    callMadePromise.get_future().wait();
}

TEST_F(BasicPollable, stopWhileInAioThread)
{
    whenRequestedStop();
    thenCleanupShouldOccur();
}

// TEST_F(BasicPollable, pleaseStop)

TEST_F(BasicPollable, post_cancelled_by_pleaseStopSync_called_within_aio_thread)
{
    std::promise<void> secondCallPosted;
    std::atomic<int> asyncCallsDone(0);

    pollable().post(
        [this, &asyncCallsDone, &secondCallPosted]()
        {
            ++asyncCallsDone;
            secondCallPosted.get_future().wait();
            pollable().pleaseStopSync();
        });

    pollable().post(
        [&asyncCallsDone]()
        {
            ++asyncCallsDone;
        });
    secondCallPosted.set_value();

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_EQ(1, asyncCallsDone.load());
}

} // namespace nx::network::aio::test
