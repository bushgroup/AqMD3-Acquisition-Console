#pragma once
#include <libaqmd3/sa220.h>
#include <iostream>
#include <tuple>


namespace AqirisDigitizer
{
	class TofTimingInformation
	{
    private:
            uint64_t samples_per_trigger;
            uint64_t record_size;
            uint64_t post_trigger_delay_samples;
            uint64_t trigger_rearm_samples;
            double post_trigger_delay_seconds;
            double trigger_rearm_time_seconds;

    public:
            // What a believable pusher period looks like, in seconds. A time-of-flight pusher
            // runs at tens of microseconds; this band is two orders either side of that and
            // still eleven orders below the readings a failed measurement produced, so it
            // excludes no plausible instrument and admits none of the observed garbage.
            static constexpr double min_pusher_period_seconds = 1e-6;
            static constexpr double max_pusher_period_seconds = 0.1;

            TofTimingInformation(uint64_t samples_per_trigger, uint64_t record_size, uint64_t post_trigger_delay_samples, uint64_t trigger_rearm_samples,
                double post_trigger_delay_seconds,
                double trigger_rearm_time_seconds)
                : post_trigger_delay_samples(post_trigger_delay_samples)
                , record_size(record_size)
                , samples_per_trigger(samples_per_trigger)
                , trigger_rearm_samples(trigger_rearm_samples)
                , post_trigger_delay_seconds(post_trigger_delay_seconds)
                , trigger_rearm_time_seconds(trigger_rearm_time_seconds)
            {}

            TofTimingInformation() = default;

        static TofTimingInformation create_timing_information(const SA220 *digitizer, double sample_rate, double post_trigger_delay_seconds, double trigger_rearm_time_seconds);
        static std::tuple<uint64_t, uint64_t, uint64_t> get_optimal_record_size(const SA220 *digitizer, uint64_t pusher_pulse_pulse_width_samples, double post_trigger_delay_s, double sample_rate, double trig_rearm_s);
        // Renamed from get_trigger_time_stamp_average, which no longer described it: it
        // returns the median of the differences between consecutive trigger timestamps, and
        // a mean of nineteen differences was one of the ways a single bad timestamp became a
        // record size the driver refused.
        static uint64_t get_trigger_period_samples(const SA220 *digitizer, int triggers);
        uint64_t get_record_size() const { return record_size; }
        uint64_t get_post_trigger_delay_samples() const { return post_trigger_delay_samples; }
        uint64_t get_samples_per_trigger() const { return samples_per_trigger; }
        uint64_t get_trigger_rearm_samples() const { return trigger_rearm_samples; }
    };
}