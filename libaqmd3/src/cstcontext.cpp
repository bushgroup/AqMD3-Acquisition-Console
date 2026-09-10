#include "../include/libaqmd3/cstcontext.h"
#include "../include/libaqmd3/acquisitionbuffer.h"
#include "../include/libaqmd3/digitizer.h"
#include "../include/libaqmd3/helpers.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

AcquiredData CstContext::acquire(uint64_t triggers_to_read, std::chrono::milliseconds timeoutMs)
{
	std::shared_ptr<AcquisitionBuffer> samples_buffer = buffer_pool->get_buffer();
	auto triggers_per_read = triggers_to_read;
	auto samples_per_trigger = samples_buffer->get_samples_per_trigger();
	auto markers_to_acquire = ViInt64(triggers_per_read * markers_hunk_size);
	std::vector<int32_t> markers_buffer(size_t(markers_to_acquire) * 16);

	ViInt64 first_element_markers = 0;
	ViInt64 available_elements_markers = 0;
	ViInt64 actual_elements_markers = 0;

	std::vector<AcquiredData::TriggerData> stamps;
	uint64_t hunks_skipped = 0;

	bool use_timeout = timeoutMs != std::chrono::milliseconds::zero();
	auto finish = std::chrono::high_resolution_clock::now() + timeoutMs;

	// The timestamps are collected by scanning for trigger headers, not by stepping a fixed
	// hunk at a time from the front of one buffer.
	//
	// Upstream stepped, which assumes every hunk in this stream describes a trigger. That
	// holds only while the markers were produced in plain streaming mode, and this stream is
	// not always the one this acquisition put there. When a zero-suppressed acquisition ends
	// part way through -- a stream overflow at full occupancy is how, in about 2.3 seconds --
	// the markers left behind are ZS1 markers, and at full occupancy about sixteen of every
	// seventeen of those are gate blocks (header 0x04) rather than triggers. Stepping over
	// them decodes block indices as timestamps: eighteen of twenty headers were wrong, the
	// average difference came out near 1e17 samples against a true 257 994, and the record
	// size derived from it was refused by the driver, which took the whole console process
	// down on the next command a client sent (lab record, task 21). Upstream noticed enough
	// to write "wrong header" to std::cerr, which is not a log sink and reaches nobody, and
	// then used the number anyway.
	//
	// Scanning repairs the measurement rather than merely detecting the fault, because the
	// trigger hunks in those leftovers carry real timestamps from the same card clock: their
	// differences are the pusher period whether they were produced a moment ago or a minute
	// ago.
	while (stamps.size() < triggers_per_read)
	{
		check_fetch_alignment(markers_channel, markers_to_acquire);
		auto rc = digitizer.stream_fetch_data(
			markers_channel.c_str(),
			markers_to_acquire,
			ViInt64(markers_buffer.size()),
			(ViInt32 *)markers_buffer.data(),
			&available_elements_markers, &actual_elements_markers, &first_element_markers);
		if (rc.second == Digitizer::Error)
		{
			throw std::runtime_error(rc.first);
		}

		int32_t *ptr = markers_buffer.data() + first_element_markers;
		for (ViInt64 hunk = 0; hunk < actual_elements_markers / markers_hunk_size; hunk++)
		{
			int32_t *seg = ptr + hunk * markers_hunk_size;
			uint32_t header = uint32_t(seg[0]);

			if ((header & 0x000000FF) != 0x01)
			{
				hunks_skipped++;
				continue;
			}

			// Both words are read as unsigned. seg[] is int32_t, so assigning seg[2] to a
			// uint64_t sign-extends it whenever the top bit is set and the shift then puts a
			// run of ones above the timestamp. Harmless on a well-formed marker, whose high
			// word is a 24 bit field, and one more of the ways a malformed one turned into an
			// enormous number.
			uint64_t low = uint32_t(seg[1]);
			uint64_t high = uint32_t(seg[2]);
			uint64_t timestampLow = (low >> 8) & 0x0000000000ffffffL;
			uint64_t timestampHigh = high << 24;
			stamps.emplace_back(timestampHigh | timestampLow, header >> 8,
				(-1 * (seg[1] & 0x000000ff)) / 256);

			if (stamps.size() >= triggers_per_read)
			{
				break;
			}
		}

		// Upstream's fetch loop had no time bound at all, although the caller passes one:
		// with nothing triggering the card it asked for markers that would never come,
		// forever, and the console answered nothing ever again.
		if (use_timeout && std::chrono::high_resolution_clock::now() > finish)
		{
			throw std::runtime_error(
				"timeout collecting trigger timestamps: " + std::to_string(stamps.size()) +
				" of " + std::to_string(triggers_per_read) + " collected, " +
				std::to_string(hunks_skipped) + " marker hunks skipped as not triggers, " +
				std::to_string(available_elements_markers) + " elements available");
		}
	}

	if (hunks_skipped > 0)
	{
		// Worth saying, because it means the stream held markers this acquisition did not
		// produce, which is what an acquisition that ended early leaves behind. spdlog is not
		// linked into libaqmd3, so this is still std::cerr; the count is what a run in a
		// visible window shows, and the measurement no longer depends on anybody reading it.
		std::cerr << "cst acq: skipped " << hunks_skipped
			<< " marker hunks that were not trigger markers\n";
	}

	ViInt64 first_element_samples;
	ViInt64 actual_elements_samples = 0;
	ViInt64 available_elements_samples = 0;

	ViInt64 elements_to_acquire = ViInt64((triggers_per_read * samples_per_trigger) / 2);
	check_fetch_alignment(samples_channel, elements_to_acquire);

	do
	{
		auto rc = digitizer.stream_fetch_data(
			samples_channel.c_str(),
			elements_to_acquire,
			samples_buffer->get_size(),
			(ViInt32 *)samples_buffer->get_raw_unaquired(),
			&available_elements_samples, &actual_elements_samples, &first_element_samples);
		if (rc.second == Digitizer::Error)
		{
			throw std::runtime_error(rc.first);
		}
		if (use_timeout && std::chrono::high_resolution_clock::now() > finish)
		{
			throw std::runtime_error(
				"timeout fetching samples: asked for " + std::to_string(elements_to_acquire) +
				", " + std::to_string(available_elements_samples) + " available");
		}
	} while (actual_elements_samples < elements_to_acquire);

	samples_buffer->advance_offset(first_element_samples);
	samples_buffer->advance_acquired(actual_elements_samples);

	return AcquiredData(stamps, samples_buffer, samples_per_trigger);
}
