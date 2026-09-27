#include "../include/zmqacquireddatasubscriber.h"
#include <snappy.h>
#include <iostream>

#define NOMINMAX 
#undef min
#undef max
#include "../include/message.pb.h"
#include <stdexcept>
#include <sstream>
#include <spdlog/spdlog.h>

namespace
{
	// The status topic carries one line per message, as the acquisition error's does.
	std::string one_line(const std::string& text)
	{
		std::string out(text);
		for (auto& character : out)
		{
			if (character == '\r' || character == '\n' || character == '\t')
				character = ' ';
		}
		return out;
	}
}

void ZmqAcquiredDataSubscriber::publish_error(const std::string& what)
{
	// A batch this subscriber cannot sum is never published, so without this a client sees a
	// frame whose stream simply comes up short. "error data:" rather than the acquisition's bare
	// "error": the acquisition carries on and the frame still ends with finished, and a client
	// can tell a damaged frame from one that failed (lab record, task 83).
	std::string text = "error data: " + one_line(what);
	zmq::message_t message(text.size());
	memcpy((void*)message.data(), text.c_str(), text.size());
	publisher->send(message, status_subject, std::chrono::milliseconds(1000));
}

void ZmqAcquiredDataSubscriber::on_notify(std::shared_ptr<UimfFrame>& item)
{
	const EncodedResult* at = nullptr;
	try
	{
		Message msg;

		std::fill(data_vector.begin(), data_vector.end(), 0);

		for (const auto& const er : item.get()->data())
		{
			at = &er;
			msg.add_tic(er.tic);
			msg.add_time_stamps(er.timestamp);

			auto begin = std::begin(er.encoded_spectra);
			auto end = std::end(er.encoded_spectra);

			int index = 0;
			for (auto val : er.encoded_spectra)
			{
				if (val < 0)
				{
					index += (-1 * val);

					if (index >= data_vector.size())
					{
						throw std::out_of_range("index oob error -> index: " + std::to_string(index));
					}

					continue;
				}
				data_vector[index++] += val;
			}
		}

		*msg.mutable_mz() = { data_vector.begin(), data_vector.end() };

		std::string msg_s;
		msg.SerializeToString(&msg_s);

		std::string compressed_msg_s;
		snappy::Compress(msg_s.data(), msg_s.size(), &compressed_msg_s);

		zmq::message_t to_send(compressed_msg_s.size());
		memcpy((void*)to_send.data(), compressed_msg_s.c_str(), compressed_msg_s.size());

		publisher->send(to_send, subject, std::chrono::milliseconds(100));

		processed++;
	}
	catch (const std::exception& ex)
	{
		spdlog::error("Error occured when processing ZMQ data: " + std::string(ex.what()));
		spdlog::error("ZMQ unprocessed elements: " + std::to_string(items.size()));
		std::string where;
		if (at != nullptr)
		{
			// The scan the batch was refused on, as the file writer will store it.
			std::ostringstream first;
			size_t shown = std::min<size_t>(at->encoded_spectra.size(), 8);
			for (size_t i = 0; i < shown; i++)
				first << (i ? " " : "") << at->encoded_spectra[i];
			spdlog::error("ZMQ refused scan {} of frame {}: timestamp {}, non-zero count {}, tic {}, "
				"{} encoded values, first {}", at->scan, item->parameters().frame_number, at->timestamp,
				at->non_zero_count, at->tic, at->encoded_spectra.size(), first.str());
			where = " in scan " + std::to_string(at->scan) + " of frame "
				+ std::to_string(item->parameters().frame_number);
		}
		publish_error(std::string(ex.what()) + where);
	}
	catch (...)
	{
		spdlog::error("Unknown error occured when processing ZMQ data");
		spdlog::error("ZMQ unprocessed elements: " + std::to_string(items.size()));
		publish_error("unknown error when processing ZMQ data");
	}
}

void ZmqAcquiredDataSubscriber::on_completed()
{
	spdlog::info("ZMQ subscriber completed");
}