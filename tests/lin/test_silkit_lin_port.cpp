// SPDX-License-Identifier: MIT
//
// SilKitLinPort lifecycle, close() handler removal, open()/close() races and in-flight callback
// synchronization (in-memory FakeLinController and real SIL Kit LIN controllers).
#include "silkit_test_util.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/core/participant_runtime.hpp"
#include "vehicle_sil/core/simulation_controller.hpp"
#include "vehicle_sil/lin/lin_master.hpp"
#include "vehicle_sil/lin/lin_slave.hpp"
#include "vehicle_sil/lin_silkit/silkit_lin_port.hpp"

#include <silkit/services/lin/ILinController.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace sk = SilKit::Services::Lin;
namespace orch = SilKit::Services::Orchestration;
using SilKit::Util::HandlerId;
using vehicle_sil::ParticipantRuntime;
using vehicle_sil::SimDuration;
using vehicle_sil::SimTime;
using vehicle_sil::SimulationController;
using vehicle_sil::SimulationControllerOptions;
using vehicle_sil::VirtualTimeOptions;
using vehicle_sil::lin::LinChecksumModel;
using vehicle_sil::lin::LinFrame;
using vehicle_sil::lin::LinId;
using vehicle_sil::lin::LinMaster;
using vehicle_sil::lin::LinPortListener;
using vehicle_sil::lin::LinResultStatus;
using vehicle_sil::lin::LinRole;
using vehicle_sil::lin::LinSlave;
using vehicle_sil::lin::SilKitLinPort;
using vehicle_sil::test::scaled_timeout;
using vehicle_sil::unit::coordinated_options;
using vehicle_sil::unit::InProcessRegistry;
using vehicle_sil::unit::start_together;
using vehicle_sil::unit::throws;

SimTime at(std::chrono::milliseconds t) { return SimTime{} + t; }

/// In-memory SIL Kit ILinController for unit and concurrency tests of SilKitLinPort.
class FakeLinController final : public sk::ILinController
{
public:
    bool throw_on_add_wakeup = false;
    bool throw_on_init = false;

    void Init(sk::LinControllerConfig) override
    {
        const std::lock_guard lock(mutex_);
        if (throw_on_init)
        {
            throw std::runtime_error("fake Init failed");
        }
        ++init_calls_;
    }

    auto Status() const -> sk::LinControllerStatus override { return sk::LinControllerStatus::Operational; }

    void SendFrame(sk::LinFrame, sk::LinFrameResponseType) override
    {
        const std::lock_guard lock(mutex_);
        ++send_frame_calls_;
    }

    void SendFrameHeader(sk::LinId) override
    {
        const std::lock_guard lock(mutex_);
        ++send_header_calls_;
    }

    void UpdateTxBuffer(sk::LinFrame) override {}

    void SetFrameResponse(sk::LinFrameResponse) override
    {
        const std::lock_guard lock(mutex_);
        ++set_response_calls_;
    }

    void GoToSleep() override
    {
        const std::lock_guard lock(mutex_);
        ++go_to_sleep_calls_;
    }

    void GoToSleepInternal() override
    {
        const std::lock_guard lock(mutex_);
        ++go_to_sleep_internal_calls_;
    }

    void Wakeup() override
    {
        const std::lock_guard lock(mutex_);
        ++wakeup_calls_;
    }

    void WakeupInternal() override
    {
        const std::lock_guard lock(mutex_);
        ++wakeup_internal_calls_;
    }

    auto AddFrameStatusHandler(FrameStatusHandler handler) -> HandlerId override
    {
        const std::lock_guard lock(cb_mutex_);
        const auto id = static_cast<HandlerId>(next_id_++);
        status_handlers_.emplace(id, std::move(handler));
        return id;
    }

    void RemoveFrameStatusHandler(HandlerId handlerId) override
    {
        const std::lock_guard lock(cb_mutex_);
        status_handlers_.erase(handlerId);
    }

    auto AddGoToSleepHandler(GoToSleepHandler handler) -> HandlerId override
    {
        const std::lock_guard lock(cb_mutex_);
        const auto id = static_cast<HandlerId>(next_id_++);
        sleep_handlers_.emplace(id, std::move(handler));
        return id;
    }

    void RemoveGoToSleepHandler(HandlerId handlerId) override
    {
        const std::lock_guard lock(cb_mutex_);
        sleep_handlers_.erase(handlerId);
    }

    auto AddWakeupHandler(WakeupHandler handler) -> HandlerId override
    {
        const std::lock_guard lock(cb_mutex_);
        if (throw_on_add_wakeup)
        {
            throw std::runtime_error("fake AddWakeupHandler failed");
        }
        const auto id = static_cast<HandlerId>(next_id_++);
        wakeup_handlers_.emplace(id, std::move(handler));
        return id;
    }

    void RemoveWakeupHandler(HandlerId handlerId) override
    {
        const std::lock_guard lock(cb_mutex_);
        wakeup_handlers_.erase(handlerId);
    }

    /// Invokes status handlers WITHOUT holding cb_mutex_, exercising SilKitLinPort's own
    /// in-flight callback wait independently of SIL Kit's SynchronizedHandlers lock.
    void deliver_frame_status_unlocked(const sk::LinFrameStatusEvent& event)
    {
        std::vector<FrameStatusHandler> copy;
        {
            const std::lock_guard lock(cb_mutex_);
            for (const auto& [id, h] : status_handlers_)
            {
                copy.push_back(h);
            }
        }
        for (const auto& h : copy)
        {
            h(this, event);
        }
    }

    /// Invokes status handlers WHILE holding cb_mutex_, matching SIL Kit's SynchronizedHandlers
    /// lock discipline to verify close() cannot deadlock on lock-order inversion.
    void deliver_frame_status_locked(const sk::LinFrameStatusEvent& event)
    {
        const std::lock_guard lock(cb_mutex_);
        for (const auto& [id, h] : status_handlers_)
        {
            h(this, event);
        }
    }

    void deliver_wakeup_unlocked(const sk::LinWakeupEvent& event)
    {
        std::vector<WakeupHandler> copy;
        {
            const std::lock_guard lock(cb_mutex_);
            for (const auto& [id, h] : wakeup_handlers_)
            {
                copy.push_back(h);
            }
        }
        for (const auto& h : copy)
        {
            h(this, event);
        }
    }

    std::size_t handler_count() const
    {
        const std::lock_guard lock(cb_mutex_);
        return status_handlers_.size() + sleep_handlers_.size() + wakeup_handlers_.size();
    }

    std::size_t init_calls() const
    {
        const std::lock_guard lock(mutex_);
        return init_calls_;
    }

    std::size_t tx_calls() const
    {
        const std::lock_guard lock(mutex_);
        return send_header_calls_ + send_frame_calls_ + go_to_sleep_calls_ + wakeup_calls_ + wakeup_internal_calls_;
    }

private:
    mutable std::mutex mutex_;
    mutable std::recursive_mutex cb_mutex_;
    std::uint64_t next_id_ = 1;
    std::map<HandlerId, FrameStatusHandler> status_handlers_;
    std::map<HandlerId, GoToSleepHandler> sleep_handlers_;
    std::map<HandlerId, WakeupHandler> wakeup_handlers_;

    std::size_t init_calls_ = 0;
    std::size_t send_header_calls_ = 0;
    std::size_t send_frame_calls_ = 0;
    std::size_t set_response_calls_ = 0;
    std::size_t go_to_sleep_calls_ = 0;
    std::size_t go_to_sleep_internal_calls_ = 0;
    std::size_t wakeup_calls_ = 0;
    std::size_t wakeup_internal_calls_ = 0;
};

struct RecordingListener final : LinPortListener
{
    std::atomic<int> responses{0};
    std::atomic<int> wakeups{0};

    void on_header(SimTime, LinId) override {}
    void on_response(SimTime, const LinFrame&, LinResultStatus) override
    {
        responses.fetch_add(1, std::memory_order_relaxed);
    }
    void on_go_to_sleep(SimTime) override {}
    void on_wakeup(SimTime) override { wakeups.fetch_add(1, std::memory_order_relaxed); }
};

sk::LinFrameStatusEvent make_status_event(LinId id)
{
    sk::LinFrameStatusEvent ev{};
    ev.timestamp = 1ms;
    ev.status = sk::LinFrameStatus::LIN_RX_OK;
    ev.frame.id = id;
    ev.frame.checksumModel = sk::LinChecksumModel::Enhanced;
    ev.frame.dataLength = 2;
    ev.frame.data = {0x11, 0x22};
    return ev;
}

} // namespace

TEST_CASE(silkit_lin_port_close_removes_handlers_is_idempotent_and_blocks_open_and_sends)
{
    FakeLinController controller;
    {
        SilKitLinPort port(controller, LinRole::Master);
        CHECK(!port.is_closed());
        CHECK_EQ(controller.handler_count(), 2u); // status + wakeup

        port.open();
        CHECK_EQ(controller.init_calls(), 1u);

        port.close();
        CHECK(port.is_closed());
        CHECK_EQ(controller.handler_count(), 0u);
        port.close(); // idempotent second close()
        CHECK_EQ(controller.handler_count(), 0u);

        // open() and send functions after close() do not call into the controller.
        port.open();
        CHECK_EQ(controller.init_calls(), 1u);
        LinFrame frame;
        frame.id = 0x10;
        frame.data_length = 2;
        port.send_header(at(1ms), 0x10, 2, LinChecksumModel::Enhanced);
        port.send_frame(at(1ms), frame);
        port.send_go_to_sleep(at(1ms));
        port.send_wakeup(at(1ms));
        port.resume();
        CHECK_EQ(controller.tx_calls(), 0u);

        // Role and argument checks still apply after close().
        CHECK(throws<std::logic_error>([&] { port.respond(at(1ms), frame); }));
        CHECK(throws<std::invalid_argument>([&] { port.send_header(at(1ms), 0x40, 2, LinChecksumModel::Enhanced); }));
    }

    // Closing before open() prevents open() from initialising the controller.
    FakeLinController closedEarly;
    {
        SilKitLinPort port(closedEarly, LinRole::Master);
        port.close();
        port.open();
        CHECK_EQ(closedEarly.init_calls(), 0u);
        CHECK_EQ(closedEarly.handler_count(), 0u);
    }

    // A failing open() leaves the port uninitialised and removable on destruction.
    FakeLinController failingOpen;
    failingOpen.throw_on_init = true;
    {
        SilKitLinPort port(failingOpen, LinRole::Master);
        CHECK(throws<std::runtime_error>([&] { port.open(); }));
        CHECK(!port.is_closed());
    }
    CHECK_EQ(failingOpen.handler_count(), 0u);

    // Partial handler registration failure in the constructor cleans up already-added handlers.
    FakeLinController failingCtor;
    failingCtor.throw_on_add_wakeup = true;
    CHECK(throws<std::runtime_error>([&] { SilKitLinPort port(failingCtor, LinRole::Slave); }));
    CHECK_EQ(failingCtor.handler_count(), 0u);
}

TEST_CASE(silkit_lin_port_close_waits_for_in_flight_callback_without_deadlock)
{
    for (const bool lockedDispatch : {false, true})
    {
        FakeLinController controller;
        SilKitLinPort port(controller, LinRole::Master);

        std::promise<void> entered;
        std::promise<void> release;
        auto enteredFuture = entered.get_future();
        auto releaseFuture = release.get_future().share();

        std::atomic<bool> callbackFinished{false};
        std::atomic<bool> closeFinished{false};

        struct BlockingListener final : LinPortListener
        {
            std::promise<void>& entered;
            std::shared_future<void> release;
            std::atomic<bool>& callbackFinished;
            std::atomic<int> calls{0};

            BlockingListener(std::promise<void>& e, std::shared_future<void> r, std::atomic<bool>& done) :
              entered(e), release(std::move(r)), callbackFinished(done)
            {
            }

            void on_header(SimTime, LinId) override {}
            void on_response(SimTime, const LinFrame&, LinResultStatus) override
            {
                if (calls.fetch_add(1, std::memory_order_acq_rel) == 0)
                {
                    entered.set_value();
                    release.wait();
                    callbackFinished.store(true, std::memory_order_release);
                }
            }
            void on_go_to_sleep(SimTime) override {}
            void on_wakeup(SimTime) override {}
        };

        BlockingListener listener(entered, releaseFuture, callbackFinished);
        port.set_listener(&listener);

        std::thread cbThread([&] {
            const auto ev = make_status_event(0x10);
            if (lockedDispatch)
            {
                controller.deliver_frame_status_locked(ev);
            }
            else
            {
                controller.deliver_frame_status_unlocked(ev);
            }
        });

        CHECK(enteredFuture.wait_for(scaled_timeout(5s)) == std::future_status::ready);

        std::promise<void> closeStarted;
        auto closeStartedFuture = closeStarted.get_future();
        std::thread closeThread([&] {
            closeStarted.set_value();
            port.close();
            closeFinished.store(true, std::memory_order_release);
        });

        CHECK(closeStartedFuture.wait_for(scaled_timeout(5s)) == std::future_status::ready);
        if (!lockedDispatch)
        {
            // Unlocked dispatch lets remove_handlers() finish immediately; close() must still
            // stay blocked in its in-flight callback wait until the callback completes.
            CHECK(vehicle_sil::unit::eventually([&] { return controller.handler_count() == 0u; }, 5s));
            CHECK(!closeFinished.load(std::memory_order_acquire));
        }

        release.set_value();
        closeThread.join();
        cbThread.join();

        CHECK(callbackFinished.load(std::memory_order_acquire));
        CHECK(closeFinished.load(std::memory_order_acquire));
        CHECK_EQ(controller.handler_count(), 0u);
        CHECK_EQ(listener.calls.load(std::memory_order_acquire), 1);

        // Late events after close() are never delivered.
        controller.deliver_frame_status_unlocked(make_status_event(0x10));
        CHECK_EQ(listener.calls.load(std::memory_order_acquire), 1);
    }

    // Even if a listener callback throws, CallbackScope releases the in-flight count so close()
    // never hangs.
    FakeLinController controller;
    SilKitLinPort port(controller, LinRole::Master);
    struct ThrowingListener final : LinPortListener
    {
        void on_header(SimTime, LinId) override {}
        void on_response(SimTime, const LinFrame&, LinResultStatus) override
        {
            throw std::runtime_error("listener boom");
        }
        void on_go_to_sleep(SimTime) override {}
        void on_wakeup(SimTime) override {}
    } throwing;
    port.set_listener(&throwing);
    CHECK(throws<std::runtime_error>([&] { controller.deliver_frame_status_unlocked(make_status_event(0x10)); }));
    port.close();
    CHECK(port.is_closed());
    CHECK_EQ(controller.handler_count(), 0u);
}

TEST_CASE(silkit_lin_port_destroy_immediately_after_close_while_callback_exits_is_safe)
{
    // Regression test for UAF if leave_callback() notified callback_cv_ after unlocking
    // callback_mutex_: destroy a heap-allocated SilKitLinPort immediately on close() return
    // while an unlocked callback thread exits its listener.
    constexpr int kRounds = 32;
    for (int round = 0; round < kRounds; ++round)
    {
        FakeLinController controller;
        auto port = std::make_unique<SilKitLinPort>(controller, LinRole::Master);

        std::promise<void> entered;
        std::promise<void> release;
        auto enteredFuture = entered.get_future();
        auto releaseFuture = release.get_future().share();

        struct BarrierListener final : LinPortListener
        {
            std::promise<void>& entered;
            std::shared_future<void> release;
            BarrierListener(std::promise<void>& e, std::shared_future<void> r) : entered(e), release(std::move(r)) {}
            void on_header(SimTime, LinId) override {}
            void on_response(SimTime, const LinFrame&, LinResultStatus) override
            {
                entered.set_value();
                release.wait();
            }
            void on_go_to_sleep(SimTime) override {}
            void on_wakeup(SimTime) override {}
        } listener(entered, releaseFuture);

        port->set_listener(&listener);

        std::thread cbThread([&] { controller.deliver_frame_status_unlocked(make_status_event(0x10)); });
        CHECK(enteredFuture.wait_for(scaled_timeout(5s)) == std::future_status::ready);

        std::thread destroyThread([&] {
            port->close();
            port.reset();
        });
        // Wait until close() has removed the handler and is waiting on callback_cv_, then let
        // the callback exit and immediately race with port.reset().
        CHECK(vehicle_sil::unit::eventually([&] { return controller.handler_count() == 0u; }, 5s));
        release.set_value();

        destroyThread.join();
        cbThread.join();
        CHECK(!port);
        CHECK_EQ(controller.handler_count(), 0u);
    }
}

TEST_CASE(silkit_lin_port_concurrent_open_close_and_callbacks_are_race_free)
{
    constexpr int kRounds = 24;
    for (int round = 0; round < kRounds; ++round)
    {
        FakeLinController controller;
        SilKitLinPort port(controller, LinRole::Master);
        RecordingListener listener;
        port.set_listener(&listener);

        std::atomic<bool> go{false};
        std::thread opener([&] {
            while (!go.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
            port.open();
        });
        std::thread closer([&] {
            while (!go.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
            port.close();
        });
        std::thread sender([&] {
            while (!go.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
            const auto ev = make_status_event(0x11);
            sk::LinWakeupEvent wake{};
            wake.timestamp = 2ms;
            wake.direction = SilKit::Services::TransmitDirection::RX;
            for (int i = 0; i < 16; ++i)
            {
                controller.deliver_frame_status_unlocked(ev);
                controller.deliver_wakeup_unlocked(wake);
            }
        });

        go.store(true, std::memory_order_release);
        opener.join();
        closer.join();
        sender.join();

        CHECK(port.is_closed());
        CHECK_EQ(controller.handler_count(), 0u);
        CHECK(controller.init_calls() <= 1u);
    }
}

TEST_CASE(silkit_lin_port_slave_and_master_real_controllers_close_cleanly)
{
    InProcessRegistry registry;
    SimulationControllerOptions controllerOptions;
    controllerOptions.registry_uri = registry.uri;
    controllerOptions.required_participants = {"LinCloseMaster", "LinCloseSlave"};
    controllerOptions.stop_at = 40ms;
    SimulationController controller(controllerOptions);

    auto masterRuntime = std::make_unique<ParticipantRuntime>(
        coordinated_options("LinCloseMaster", registry.uri, VirtualTimeOptions{5ms, std::nullopt}));
    auto slaveRuntime = std::make_unique<ParticipantRuntime>(
        coordinated_options("LinCloseSlave", registry.uri, VirtualTimeOptions{5ms, std::nullopt}));

    SilKitLinPort masterPort(*masterRuntime->participant().CreateLinController("LIN1", "LIN_CLOSE"), LinRole::Master);
    SilKitLinPort slavePort(*slaveRuntime->participant().CreateLinController("LIN1", "LIN_CLOSE"), LinRole::Slave);

    LinMaster master(masterPort, {{0x10, 2, LinChecksumModel::Enhanced, 10ms}}, 5ms);
    LinSlave slave(slavePort);
    masterRuntime->set_communication_ready_handler([&] { master.init(); });
    slaveRuntime->set_communication_ready_handler([&] { slave.init(); });
    masterRuntime->set_step_handler([&](SimTime now, SimDuration) { master.step(now); });
    slaveRuntime->set_step_handler([&](SimTime now, SimDuration) {
        slave.step(now);
        const std::array<std::uint8_t, 2> data{0xAA, 0x55};
        slave.set_response(now, 0x10, data);
    });

    start_together(controller, {masterRuntime.get(), slaveRuntime.get()}, 30s);
    CHECK(controller.wait_until_finished(scaled_timeout(30s)) == orch::ParticipantState::Shutdown);
    CHECK(masterRuntime->wait_until_finished(scaled_timeout(30s)) == orch::ParticipantState::Shutdown);
    CHECK(slaveRuntime->wait_until_finished(scaled_timeout(30s)) == orch::ParticipantState::Shutdown);

    // Explicit close() removes both master and slave handlers (including the slave's dynamic
    // header handler), is idempotent, and blocks subsequent open().
    masterPort.close();
    slavePort.close();
    CHECK(masterPort.is_closed());
    CHECK(slavePort.is_closed());
    masterPort.close();
    slavePort.close();
    masterPort.open();
    slavePort.open();
    CHECK(master.statistics().responses_ok >= 2u);
}
