#include "../include/libaqmd3/cstzs1context.h"
#include "../include/libaqmd3/digitizer.h"
#include "../include/libaqmd3/helpers.h"

#include <vector>
#include <tuple>
#include <exception>
#include <fstream>
#include <iostream>
using std::cout;
using std::cerr;
#include <limits>
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <spdlog/spdlog.h>

#define variable_m_t_a

bool CstZs1Context::diagnostics = false;

namespace
{
	// One 16-element marker hunk as hex words, the form the SA220P manual draws them in.
	std::string hunk_hex(const int32_t* hunk)
	{
		std::ostringstream out;
		out << std::hex << std::setfill('0');
		for (int i = 0; i < 16; i++)
		{
			if (i)
				out << (i % 4 ? " " : " | ");
			out << std::setw(8) << uint32_t(hunk[i]);
		}
		return out.str();
	}
}

void CstZs1Context::start()
{
	StreamingContext::start();
	frame_last_trigger = last_trigger;
	frame_first_trigger_checked = false;
	frame_triggers = 0;
	frame_fetches = 0;
	frame_out_of_record = 0;
	last_walked_valid = false;
}

void CstZs1Context::stop()
{
	StreamingContext::stop();
	markers_buffer.reset();
	if (frame_out_of_record > out_of_record_reports)
	{
		spdlog::warn("markers: {} gate markers past the record in this frame, the first {} shown",
			frame_out_of_record, out_of_record_reports);
	}
	if (diagnostics)
	{
		spdlog::info("markers: frame ended after {} triggers in {} fetches, last trigger index {} at {}",
			frame_triggers, frame_fetches, last_trigger.index, last_trigger.timestamp);
	}
}

void CstZs1Context::note_trigger(uint64_t timestamp, uint32_t index)
{
	if (!frame_first_trigger_checked)
	{
		// The card's clock runs on across frames, so a frame whose first trigger is not past the
		// previous frame's last is that frame's markers read a second time (lab record, task 83).
		frame_first_trigger_checked = true;
		if (frame_last_trigger.valid && timestamp <= frame_last_trigger.timestamp)
		{
			spdlog::warn("markers replay: this frame's first trigger, index {} at {}, is not past "
				"the previous frame's last, index {} at {}",
				index, timestamp, frame_last_trigger.index, frame_last_trigger.timestamp);
			// Ended here, before a single element of the samples stream is fetched against these
			// markers. Fetching samples against a replayed markers stream is what left the samples
			// stream lagging its markers for the rest of the console process (lab record, task 99).
			throw FrameDamaged("markers replayed");
		}
		else if (diagnostics)
		{
			spdlog::info("markers: frame's first trigger index {} at {}, previous frame's last index {} at {}",
				index, timestamp, frame_last_trigger.index, frame_last_trigger.timestamp);
		}
	}
	++frame_triggers;
	if (!last_trigger.valid || timestamp > last_trigger.timestamp)
	{
		last_trigger.valid = true;
		last_trigger.timestamp = timestamp;
		last_trigger.index = index;
	}
}

void CstZs1Context::note_gate(const int32_t* hunk, uint64_t start_sample, uint64_t stop_sample,
	uint64_t record_samples, int trig_count, size_t stamps)
{
	// A gate is positioned relative to its own trigger, so neither end can lie past the record.
	// One that does is what the decode turns into a run length hundreds of millions of bins
	// long; the hunk it came from is the evidence (lab record, task 83). Reported and kept.
	++frame_out_of_record;
	if (frame_out_of_record > out_of_record_reports)
		return;
	spdlog::warn("markers: gate past the record, start {} stop {} against {} samples; trigger {} "
		"of this acquire ({} stamps), {} into the frame, fetch {}; hunk {}",
		start_sample, stop_sample, record_samples, trig_count, stamps, frame_triggers,
		frame_fetches, hunk_hex(hunk));
}

AcquiredData CstZs1Context::acquire(uint64_t triggers_to_read, std::chrono::milliseconds timeoutMs)
{
	int markers_to_acquire = triggers_to_read * markers_hunk_size;
	int active_multiplier = 1;

	int trig_count = 0;
	int gate_count = 0;
	uint64_t to_acquire = 0;
	std::vector<AcquiredData::TriggerData> stamps;

	std::shared_ptr<AcquisitionBuffer> samples_buffer = buffer_pool->get_buffer();

	ViInt64 first_element_markers;
	ViInt64 available_elements_markers = 0;
	ViInt64 actual_elements_markers = markers_buffer.get_unprocessed();

	bool use_timeout = (timeoutMs == std::chrono::milliseconds::zero()) ? false : true;

	// sanity check
	if (actual_elements_markers % 16 != 0)
	{
		throw std::runtime_error("actual_elements_markers % 16 != 0 at start of acquire");
	}

	auto finish = std::chrono::high_resolution_clock::now() + timeoutMs;
	while (trig_count <= triggers_to_read)
	{

		for (int i = 0; i < actual_elements_markers / 16; i++)
		{
			int32_t *seg = markers_buffer.get_raw_unprocessed();
			uint32_t header = seg[0];

			switch (header & 0x000000FF)
			{
			case 0x01:
			{
				++trig_count;
				if (trig_count >= triggers_to_read + 1)
					goto process;

				uint64_t low = seg[1];
				uint64_t high = seg[2];
				uint64_t timestampLow = (low >> 8) & 0x0000000000ffffffL;
				uint64_t timestampHigh = uint64_t(high) << 24;

				stamps.emplace_back(timestampHigh | timestampLow, header >> 8, (-1 * (seg[1] & 0x000000ff))/256);
				note_trigger(timestampHigh | timestampLow, header >> 8);
				markers_buffer.advance_processed(16);

				break;
			}
			case 0x04:
			case 0x0a:
			{
				int block_total = 0;
				for (int i = 0; i < 4; i++)
				{
					int32_t *l_ptr = seg + (i * 4);
					if ((*l_ptr & 0x000000FF) == 0x04)
					{
						uint32_t s_lo = l_ptr[0];
						uint32_t s_hi = l_ptr[1];
						uint32_t e_lo = l_ptr[2];
						uint32_t e_hi = l_ptr[3];

						uint64_t start_block_index = (uint64_t(s_hi & 0xffffff) << 8) | ((s_lo >> 24) & 0xff);
						uint32_t start_block_sample_indx = (s_hi >> 24) & 0xff;
						uint64_t end_block_index = (uint64_t(e_hi & 0xffffff) << 8) | ((e_lo >> 24) & 0xff);
						uint32_t end_block_sample_indx = (e_hi >> 24) & 0xff;

						uint64_t to_acquire_memory_blocks_f = (end_block_index - start_block_index) * 4;

						if (end_block_index < start_block_index)
						{
							//std::cout << "end_block_index < start_block_index\n";
							//std::cout << "end_block_index: " << end_block_index << " start_block_index: " << start_block_index << "\n";
						
							// suggested in CPP_IVIC_StreamingZeroSuppress example project
							end_block_index += uint64_t(std::numeric_limits<unsigned int>::max());
						}

						if (to_acquire_memory_blocks_f % 16 != 0)
							to_acquire_memory_blocks_f = ((to_acquire_memory_blocks_f / 16) + 1) * 16;

						if (stamps.size() == 0)
						{
							std::cerr << "\tNo elements in stamps - discarding acquired elements." << std::endl;
							markers_buffer.advance_processed(16);
							break;
						}

						{
							// The same arithmetic as GateData, done here so that a gate past the
							// record can be reported with the hunk it came from.
							uint64_t start_sample = (start_block_index - 1) * 8 + start_block_sample_indx;
							uint64_t stop_sample = (end_block_index - 1) * 8 - (8 - end_block_sample_indx);
							uint64_t record_samples = samples_buffer->get_samples_per_trigger();
							if (start_sample > record_samples || stop_sample > record_samples)
								note_gate(seg, start_sample, stop_sample, record_samples, trig_count, stamps.size());
						}

						stamps.back().gate_data.emplace_back(
							start_block_index,
							start_block_sample_indx,
							end_block_index,
							end_block_sample_indx,
							to_acquire_memory_blocks_f);

						gate_count++;
						to_acquire += to_acquire_memory_blocks_f;
					}
					else if ((*l_ptr & 0x000000FF) == 0x0a)
					{
						// Do not use pre- or post-gate samples, so no need to store information at the moment
						uint32_t r_lo = l_ptr[0];
						uint32_t r_hi = l_ptr[1];

						uint64_t record_block_index = (uint64_t(r_hi & 0xffffff) << 8) | ((r_lo >> 24) & 0xff);
						uint32_t record_block_sample_indx = (r_hi >> 24) & 0xff;
					}
				}

				markers_buffer.advance_processed(16);
				break;
			}
			case 0x08:	// Should never get here
			{
				throw std::runtime_error("dummy gate error");
			}
			default:
				throw std::string("unexpected header -> (default) header: " + std::to_string(header & 0x000000FF));
			}
		}

		if (trig_count <= triggers_to_read)
		{
			bool diagnosing = diagnostics && frame_triggers < diagnostic_triggers;
			if (diagnosing && markers_buffer.get_processed() >= 16)
			{
				// The last hunk this fetch's walk reached, kept for the line the next fetch writes.
				const int32_t* walked = markers_buffer.get_raw_unprocessed() - 16;
				std::copy(walked, walked + 16, last_walked_hunk.begin());
				last_walked_valid = true;
			}
			markers_buffer.reset();

			int next_markers_to_acquire = markers_to_acquire;
	
			first_element_markers = 0;
			available_elements_markers = 0;
			actual_elements_markers = 0;

			do
			{
				markers_to_acquire = next_markers_to_acquire;

				check_fetch_alignment(markers_channel, markers_to_acquire);
				auto rc = digitizer.stream_fetch_data(
						markers_channel.c_str(),
						markers_to_acquire,
						markers_buffer.get_size(),
						(ViInt32 *)markers_buffer.get_raw_unaquired(),
						&available_elements_markers, &actual_elements_markers, &first_element_markers);
				if (rc.second == Digitizer::Error)
				{
					throw std::runtime_error(rc.first);
				}

				if (use_timeout && std::chrono::high_resolution_clock::now() > finish)
				{
					std::stringstream timeout_conditions;
					timeout_conditions <<  "timeout conditions:\n\n" \
					<< "markers_buffer.get_size(): " << std::to_string(markers_buffer.get_size()) << "\n" \
					<< "\tmarkers_buffer.get_acquired(): " << std::to_string(markers_buffer.get_acquired()) << "\n" \
					<< "\tmarkers_to_acquire: " << std::to_string(markers_to_acquire) << "\n" \
					<< "\tavailable_elements_markers: " << std::to_string(available_elements_markers) << "\n" \
					<< "\tactual_elements_markers: " << std::to_string(actual_elements_markers) << "\n\n" \
					<< "\ttrig_count: " << std::to_string(trig_count) << "\n";

					throw std::runtime_error("timeout in acquisition.\n" + timeout_conditions.str());
				}

			} while (actual_elements_markers == 0);

#ifdef variable_m_t_a
			if (available_elements_markers > markers_to_acquire && active_multiplier < multiplier_max)
			{
				// One marker hunk is markers_hunk_size elements, so the count this asks for is a number
				// of hunks and not a number of triggers. Leaving markers_hunk_size out asked for 1000
				// elements where 16000 was meant, at 500 triggers and a multiplier of 2, and 1000 is not
				// a multiple of 16: the driver refused the fetch, the acquisition thread caught it and
				// gave up, and the frame ended early with no error anywhere but the log. It only shows
				// once a markers backlog larger than one request has built up, which needs the gate
				// traffic of a record that is actually being suppressed. markers_buffer is sized
				// max_triggers_per_read * markers_hunk_size * multiplier_max, which is this count at the
				// largest multiplier, so the buffer was always sized for what this line was meant to say.
				markers_to_acquire = int(triggers_to_read * markers_hunk_size * ++active_multiplier);
			}
#endif

			markers_buffer.advance_offset(first_element_markers);
			markers_buffer.advance_acquired(actual_elements_markers);

			++frame_fetches;
			if (diagnosing)
			{
				spdlog::info("markers: fetch {} at trigger {} of the frame ({} of this acquire): asked {}, "
					"available {}, got {}, first element {}; previous fetch ended {}; this one begins {}",
					frame_fetches, frame_triggers, trig_count, markers_to_acquire, available_elements_markers,
					actual_elements_markers, first_element_markers,
					last_walked_valid ? hunk_hex(last_walked_hunk.data()) : std::string("(none)"),
					actual_elements_markers >= 16 ? hunk_hex(markers_buffer.get_raw_unprocessed()) : std::string("(short)"));
			}
		}
	}

process:
	ViInt64 first_element_samples = 0;
	ViInt64 actual_elements_samples = 0;
	ViInt64 available_elements_samples = 0;

	// actual_elements_samples is an out-parameter that each fetch overwrites, so subtracting it
	// from to_acquire is only the outstanding count while no fetch has yet come back short. Keep
	// the running total, and pass the space that is actually left in the buffer rather than its
	// whole size, since the pointer handed over has already advanced into it.
	uint64_t fetched_elements_samples = 0;

	while (fetched_elements_samples < to_acquire)
	{
		ViInt64 outstanding = ViInt64(to_acquire - fetched_elements_samples);
		check_fetch_alignment(samples_channel, outstanding);
		auto rc = digitizer.stream_fetch_data(
				samples_channel.c_str(),
				outstanding,
				samples_buffer->get_available(),
				(ViInt32 *)samples_buffer->get_raw_unaquired(),
				&available_elements_samples, &actual_elements_samples, &first_element_samples);
		if (rc.second == Digitizer::Error)
		{
			throw std::runtime_error(rc.first);
		}

		samples_buffer->advance_offset(first_element_samples);
		samples_buffer->advance_acquired(actual_elements_samples);
		fetched_elements_samples += uint64_t(actual_elements_samples);
	}

	if (diagnostics && frame_triggers <= diagnostic_triggers + stamps.size() && !stamps.empty())
	{
		spdlog::info("markers: acquire returned {} triggers, index {} at {} to index {} at {}, {} gates, "
			"{} sample elements", stamps.size(), stamps.front().index, stamps.front().timestamp,
			stamps.back().index, stamps.back().timestamp, gate_count, to_acquire);
	}

	return AcquiredData(stamps, samples_buffer, samples_buffer->get_samples_per_trigger());

}