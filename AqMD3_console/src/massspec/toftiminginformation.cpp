#include "../../include/massspec/toftiminginformation.h"

#include <algorithm>
#include <format>
#include <stdexcept>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>


namespace AqirisDigitizer
{
    TofTimingInformation TofTimingInformation::create_timing_information(const SA220 *digitizer, double sample_rate, double post_trigger_delay_seconds, double trigger_rearm_time_seconds)
    {
        auto samples_per_trigger = TofTimingInformation::get_trigger_period_samples(digitizer, 20);

        // Nothing checked this number before it reached the driver, and one instance of that
        // killed the console process rather than earning an error: after a stream overflow the
        // period read 9.7e17 samples, get_optimal_record_size took the delay off it in
        // unsigned arithmetic, and set_record_size threw out of the command handler and out of
        // main (lab record, task 21). The measurement itself is repaired where it is made, in
        // CstContext::acquire; this is the guard for whatever else can go wrong with it. A
        // measured period is a measurement, and a measurement can fail.
        double period_seconds = double(samples_per_trigger) / sample_rate;
        if (period_seconds < min_pusher_period_seconds || period_seconds > max_pusher_period_seconds)
        {
            throw std::runtime_error(std::format(
                "measured pusher period {} samples ({:g} s at {:g} S/s) is outside the "
                "believable band {:g} s to {:g} s; the measurement failed rather than the "
                "instrument having changed",
                samples_per_trigger, period_seconds, sample_rate,
                min_pusher_period_seconds, max_pusher_period_seconds));
        }

        auto t = TofTimingInformation::get_optimal_record_size(digitizer, samples_per_trigger, post_trigger_delay_seconds, sample_rate, trigger_rearm_time_seconds);

        return TofTimingInformation(samples_per_trigger, std::get<1>(t), std::get<0>(t), std::get<2>(t),
            post_trigger_delay_seconds, trigger_rearm_time_seconds);
    }

    uint64_t TofTimingInformation::get_trigger_period_samples(const SA220 *digitizer, int triggers)
    {
        uint64_t record_size = 1024;
        digitizer->set_record_size(record_size);
        auto dig_context = digitizer->configure_cst(digitizer->channel_1, std::make_shared<AcquisitionBufferPool>(triggers, record_size, 10, 10));

        // The context is stopped however this leaves, and that is not tidiness.
        //
        // Upstream could not fail here: acquire() had no time bound, so it either returned or
        // never came back. Now that it can throw, an exception on the way out would leave the
        // digitizer initiated, and everything afterwards fails at apply_setup with
        //
        //     Error Code: -1074118653  Error Message: Acquisition running
        //
        // for as long as the process lives. Measured: one measurement that timed out wedged
        // the console for every command that followed it, which is exactly the failure this
        // task exists to end, arrived at from the other side.
        struct StopOnLeaving
        {
            std::shared_ptr<StreamingContext> context;
            ~StopOnLeaving() { try { context->stop(); } catch (...) {} }
        } stopper{dig_context};

        // One retry, because the first attempt may be reading past what an earlier
        // acquisition left in the stream and the abort and restart in between is the only
        // thing here that can shorten that. A second timeout is a real failure and is
        // reported as one.
        AcquiredData result = [&]
        {
            dig_context->start();
            try
            {
                return dig_context->acquire(triggers, measurement_timeout);
            }
            catch (const std::exception& first)
            {
                spdlog::warn("measuring the pusher period: {}. Restarting the streaming "
                    "context and trying once more.", first.what());
                dig_context->stop();
                dig_context->start();
                return dig_context->acquire(triggers, measurement_timeout);
            }
        }();

        if (result.stamps.size() < 2)
        {
            throw std::runtime_error(std::format(
                "{} trigger timestamps is not enough to measure a period; two is the minimum",
                result.stamps.size()));
        }

        // The median difference, where upstream took the mean.
        //
        // A mean of nineteen differences has no defence against one bad one. A single backward
        // step between two timestamps wraps in unsigned arithmetic to about 1.8e19, and a
        // nineteenth of that is 9.7e17 -- which is, to three figures, the large reading this
        // console produced on two of the three occasions it was seen. The timestamps that
        // reach here are now scanned out of the marker stream rather than assumed, so a bad
        // difference should be rare; the two places one can still come from are real, though,
        // being a wrap of the card's timestamp counter and the step across the boundary
        // between markers left by an earlier acquisition and markers from this one. A median
        // rides out any minority of those. A mean does not ride out even one.
        std::vector<uint64_t> differences;
        differences.reserve(result.stamps.size() - 1);
        for (size_t i = 0; i + 1 < result.stamps.size(); i++)
        {
            differences.push_back(result.stamps[i + 1].timestamp - result.stamps[i].timestamp);
        }

        auto middle = differences.begin() + differences.size() / 2;
        std::nth_element(differences.begin(), middle, differences.end());

        return *middle;
    }

    std::tuple<uint64_t, uint64_t, uint64_t> TofTimingInformation::get_optimal_record_size(const SA220 *digitizer, uint64_t pusher_pulse_pulse_width_samples, double post_trigger_delay_s, double sample_rate, double trig_rearm_s)
    {
        uint64_t actual_trigger_width_samples = uint64_t(double(pusher_pulse_pulse_width_samples) * (sample_rate / digitizer->max_sample_rate));
        uint64_t trig_rearm_samples = uint64_t(trig_rearm_s * sample_rate);
        uint64_t delay_samples = uint64_t(post_trigger_delay_s * sample_rate);

        // Unsigned arithmetic, so a period smaller than what is taken off it does not go
        // negative: it wraps to about 1.8e19 and is handed to the driver as a record size. The
        // band check in create_timing_information keeps a garbage period from reaching here at
        // all; this covers what the band admits, which is a real pusher running faster than
        // the post-trigger delay and the rearm time together allow.
        if (actual_trigger_width_samples <= delay_samples + trig_rearm_samples)
        {
            throw std::runtime_error(std::format(
                "a pusher period of {} samples leaves no record: the post-trigger delay is {} "
                "samples and the trigger rearm time {}",
                actual_trigger_width_samples, delay_samples, trig_rearm_samples));
        }

        auto record_size_samples = actual_trigger_width_samples - delay_samples - trig_rearm_samples;
        if (record_size_samples % 32 != 0)
            record_size_samples = (record_size_samples / 32) * 32;

        return std::make_tuple(delay_samples, record_size_samples, trig_rearm_samples);
    }
}
