#ifndef CST_ZS1_CONTEXT_H
#define CST_ZS1_CONTEXT_H

#include "streamingcontext.h"

#include <array>
#include <vector>
#include <tuple>
#include <algorithm>

class CstZs1Context : public StreamingContext {
private:
	uint64_t const gate_acquisition_multiplier = 2;
	uint64_t const markers_hunk_size = 16;
	//uint64_t const min_target_records;

	int active_multiplier;

	int const multiplier_min = 1;
	int const multiplier_max = 8;

	//TODO use array or vector
	AcquisitionBuffer markers_buffer;

	// What the marker diagnostics keep between calls. A frame is everything from one start() to
	// the next, which is one acquire frame; the counts restart there. The trigger seen last is
	// kept across frames on purpose, since the question the replay check asks is whether this
	// frame's first trigger lies past the previous frame's last.
	struct TriggerSeen {
		bool valid = false;
		uint64_t timestamp = 0;
		uint32_t index = 0;
	};
	TriggerSeen last_trigger;          // the latest trigger any acquire has walked
	TriggerSeen frame_last_trigger;    // the previous frame's last, frozen at start()
	bool frame_first_trigger_checked = false;
	uint64_t frame_triggers = 0;       // triggers walked since start()
	uint64_t frame_fetches = 0;        // markers fetches since start()
	uint64_t frame_out_of_record = 0;  // gate markers past the record since start()
	std::array<int32_t, 16> last_walked_hunk{};
	bool last_walked_valid = false;

	void note_trigger(uint64_t timestamp, uint32_t index);
	void note_gate(const int32_t* hunk, uint64_t start_sample, uint64_t stop_sample,
		uint64_t record_samples, int trig_count, size_t stamps);

public:
	// Off by default. The two reports that fire only on the fault this was written for, a frame
	// whose first trigger is not past the previous frame's last and a gate marker that lies past
	// the record, are always on; this adds the per-fetch and per-call lines and the hunks either
	// side of each fetch boundary in a frame's first diagnostic_triggers triggers, which are
	// written on every frame. MarkerDiagnostics=1 in config.txt.
	static bool diagnostics;
	static uint64_t const diagnostic_triggers = 2000;
	static uint64_t const out_of_record_reports = 32;

	CstZs1Context(const Digitizer& digitizer, std::string channel, std::shared_ptr<AcquisitionBufferPool> buffer_pool)
		: StreamingContext(digitizer, channel, buffer_pool)
		//, min_target_records(triggers_per_read * markers_hunk_size)
		, markers_buffer((size_t)(buffer_pool->get_max_triggers_per_read() * markers_hunk_size * multiplier_max) + 15)
		, active_multiplier(1)
	{}

	void start() override;
	void stop() override;
	AcquiredData acquire(uint64_t triggers_to_read, std::chrono::milliseconds timeoutMs) override;
};

#endif // !CST_ZS1_CONTEXT_H
