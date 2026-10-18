#include "../include/acquirepublisher.h"
#include "../include/definitions.h"
#include <ctime>
#include <chrono>
#include <iostream>
#include <spdlog/spdlog.h>
#include <format>
#include <typeinfo>
#ifdef _MSC_VER
#include <eh.h>
#endif

// TODO: move to config
#define FREE_ON_BUFFER_COUNT 5
#define WARN_ON_BUFFER_COUNT 3

namespace
{
	// The Release build uses /EHa, so a structured exception (an access violation, say) reaches the
	// acquire loop's catch (...) with nothing to say what it was. Translated on the acquisition
	// thread, it carries its code to the std::exception handler instead.
	class StructuredException : public std::runtime_error {
	public:
		explicit StructuredException(unsigned int code)
			: std::runtime_error(std::format("structured exception 0x{:08x}", code)) {}
	};

#ifdef _MSC_VER
	void translate_structured(unsigned int code, struct _EXCEPTION_POINTERS*)
	{
		throw StructuredException(code);
	}
#endif

	// What a catch (...) caught, for its log line and status message. Called only inside one.
	std::string describe_unknown()
	{
		try
		{
			throw;
		}
		catch (const std::string& text)
		{
			return "a std::string: " + text;
		}
		catch (const char* text)
		{
			return std::string("a C string: ") + text;
		}
		catch (...)
		{
			return "a throw of no type it knows";
		}
	}

	// A status message is one line by convention, and a driver error is not: the AqMD3 messages
	// carry an error code and an explanation on separate lines.
	std::string one_line(const std::string& text)
	{
		std::string out(text);
		for (auto& character : out)
		{
			if (character == '\r' || character == '\n' || character == '\t')
			{
				character = ' ';
			}
		}
		return out;
	}
}

void AcquirePublisher::publish_status(const std::string& text, std::chrono::milliseconds timeout)
{
	zmq::message_t message(text.size());
	memcpy((void *)message.data(), text.c_str(), text.size());
	publisher->send(message, subject, timeout);
}

void AcquirePublisher::start(UimfFrameParameters parameters)
{
	try
	{
		if (state.load() == State::ACQUIRING)
		{
			spdlog::warn("Acquisition has not been stopped before attempting to acquire again");
			return;
		}

		should_stop.store(false);
		stop_signal = std::promise<State>();
		auto handle = std::make_unique<std::thread>([&, parameters]()
			{
				uint64_t scans_total_count = parameters.frame_length;
				uint64_t scans_acquired_count = 0;
				bool has_errored = false;

				state = State::ACQUIRING;
#ifdef _MSC_VER
				_set_se_translator(translate_structured);
#endif

				if (digitizer->get_is_acquiring())
				{
					spdlog::warn("Error when trying to acquire frame while digitizer is already acquiring.");
				}
				else
				{
					digitizer->start();
					while (scans_acquired_count < scans_total_count)
					{
						if (has_errored || should_stop.load(std::memory_order_relaxed))
						{
							break;
						}

						try
						{
							auto available = buffer_pool->get_available_buffers();
							if (available <= WARN_ON_BUFFER_COUNT)
							{
								spdlog::warn("Available buffer count {}", available);
							}

							uint64_t to_acquire_count = (scans_total_count - scans_acquired_count) < segment_size
								? (scans_total_count - scans_acquired_count)
								: segment_size;

							auto data = digitizer->acquire(to_acquire_count, std::chrono::milliseconds(this->timeout));

							notify(UimfAcquisitionRecord(parameters, data, scans_acquired_count), SubscriberType::BOTH);

							scans_acquired_count += data.stamps.size();
						}
						catch (const FrameDamaged& ex)
						{
							// The frame ends here, with its finished: "error data:" is a damaged frame to
							// a client, where "error" is a failed one. The process is not sound after it
							// (see FrameDamaged), so a client restarts the console before acquiring again.
							spdlog::error("Frame {} ended after {} scans: {}", parameters.frame_number,
								scans_acquired_count, ex.what());
							publish_status("error data: " + one_line(ex.what()) + " in frame "
								+ std::to_string(parameters.frame_number), std::chrono::milliseconds(1000));
							has_errored = true;
						}
						catch (const std::exception& ex)
						{
							spdlog::error("Error when acquiring UIMF data: {} ({}; driver error {})", ex.what(),
								typeid(ex).name(), digitizer->driver_error());
							// Logged is not the same as reported. Without this a client sees a frame that ended
							// with fewer scans than it asked for, or none at all, and cannot tell that from a
							// frame in which nothing was above the threshold. The timeout is short and finite so
							// that a blocked publisher can never hold up the "finished" that follows.
							publish_status("error " + one_line(ex.what()), std::chrono::milliseconds(1000));
							has_errored = true;
						}
						catch (...)
						{
							// Only a throw of something other than a std::exception lands here now; the
							// one the acquire path had, a std::string in CstZs1Context, is a
							// runtime_error since 1.5.0 (lab record, tasks 83 and 105).
							std::string what = describe_unknown();
							spdlog::error("Unknown error when acquiring UIMF data: {} (driver error {})", what,
								digitizer->driver_error());
							publish_status("error unknown error when acquiring UIMF data: " + one_line(what),
								std::chrono::milliseconds(1000));
							has_errored = true;
						}
					}
				}

				state = State::STOPPED;
				//stop_signal.set_value(State::STOPPED);
				digitizer->stop();
				spdlog::info(std::format("Scans acquired: {}", scans_acquired_count));

				publish_status("finished", std::chrono::milliseconds::max());

				return;
			});

		worker_handle = std::move(handle);
	}
	catch (const std::exception& ex)
	{
		spdlog::error("Error processing UIMF request: " + std::string(ex.what()));
		publish_status("error " + one_line(ex.what()), std::chrono::milliseconds(1000));
	}
	catch (...)
	{
		spdlog::error("Unknown error when processing UIMF request");
		publish_status("error unknown error when processing UIMF request",
			std::chrono::milliseconds(1000));
	}
}

void AcquirePublisher::stop(bool terminate_acquisition_chain)
{
	try
	{
		spdlog::debug(std::format("AcquirePublisher::stop({})", terminate_acquisition_chain));
		auto start = std::chrono::high_resolution_clock::now();

		spdlog::debug("Stopping publisher");

		should_stop.store(true);

		if (terminate_acquisition_chain)
		{
			//Should invalidate this class and make it so it can't be used again
			notify_completed_and_wait();
			publish_status("finished acquire", std::chrono::milliseconds(30000));
		}

		if (worker_handle && worker_handle->joinable()) {
			worker_handle->join();
		}

		auto end = std::chrono::high_resolution_clock::now();
		auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
		spdlog::debug(std::format("Time to stop: {} ms", ms));
	}
	catch (const std::exception& ex)
	{
		spdlog::error("Error when stopping publisher: " + std::string(ex.what()));
	}
	catch (...)
	{
		spdlog::error("Uknown error when stopping publisher");
	}
}

