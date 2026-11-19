// SPDX-License-Identifier: MIT
//
// vehicle_sil::trace while it is off (no VEHICLE_SIL_TRACE, no start()): the recording functions
// allocate nothing, not even a ring. This executable counts every allocation of the process
// (allocation_counter.hpp), so it is built on its own.
#include "allocation_counter.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/trace/trace.hpp"

namespace trace = vehicle_sil::trace;

TEST_CASE(a_trace_that_is_off_allocates_nothing)
{
    CHECK(!trace::enabled());
    g_allocations.store(0);
    g_counting.store(true);
    for (int i = 0; i < 1000; ++i)
    {
        trace::Span span("off", i, 1);
        span.set_arg(2);
        trace::instant("off", i, 3);
        trace::flow_out("off", trace::command_flow(4, 5));
        trace::flow_in("off", trace::answer_flow(4, 5));
        trace::set_thread_name("off");
    }
    trace::set_process_name("off");
    const bool flushed = trace::flush().has_value();
    g_counting.store(false);
    CHECK(!flushed);
    CHECK_EQ(g_allocations.load(), std::size_t{0});
}
