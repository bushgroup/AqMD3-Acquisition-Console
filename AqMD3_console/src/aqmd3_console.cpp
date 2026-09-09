#include "../include/server.h"
#include "../include/acquisitioncontrol.h"
#include "../include/acquirepublisher.h"
#include "../include/uimfframewritersubscriber.h"
#include "../include/zmqacquireddatasubscriber.h"
#include "../include/processsubject.h"
#include "../include/definitions.h"
#include "../include/util/config.h"
#include "../include/util/uimfhelpers.h"
#include "../include/message.pb.h"
#include "../include/massspec/toftiminginformation.h"
#include "include/app.h"

#include <libaqmd3/digitizer.h>
#include <libaqmd3/acquireddata.h>
#include <libaqmd3/sa220.h>

#include <UIMFWriter/uimfwriter.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/daily_file_sink.h>

#include <stdexcept>
#include <picosha2.h>
#include <visa.h>
#include <snappy.h>
#include <zmq.hpp>
#include <iostream>
#include <tuple>
#include <string>
#include <map>
#include <cctype>
#include <chrono>
#include <optional>
using std::cerr;
#include <windows.h>
#include "../include/diagnostic/datageneratorcontext.h"

#define NOMINMAX 
#undef min
#undef max


void disable_quick_edit()
{
	HANDLE handle;
	DWORD current_settings;
	handle = GetStdHandle(STD_INPUT_HANDLE);

	if (handle == INVALID_HANDLE_VALUE)
	{
		throw std::runtime_error("Error getting standard device handle\n");
	}

	if (!GetConsoleMode(handle, &current_settings))
	{
		throw std::runtime_error("Error getting console mode");
	}

	if (!SetConsoleMode(handle, ENABLE_EXTENDED_FLAGS | (current_settings & ~ENABLE_QUICK_EDIT_MODE)))
	{
		throw std::runtime_error("Error setting console mode");
	}
}

static char ack[] = "ack";
uint32_t calculated_post_trigger_samples = 0;
uint64_t avg_tof_period_samples = 0;

// Configuration variables
static double post_trigger_delay = 0.00001; // Default post trigger delay in 10us
static double estimated_trigger_rearm_time = 0.000002048; // Default allowed trigger rearm time is 2us
uint64_t notify_on_scans_count = 500;
std::string resource_name = "PXI0::0::0::INSTR";
int64_t acquisition_timeout_ms = 100;
uint64_t acquisition_initial_buffer_count = 40;
uint64_t acquisition_max_buffer_count = 100;
//int32_t trigger_events_per_read_count = 100;
uint64_t acquisition_buffer_reserve_elements_count = 2048;
std::string log_level = "info";

// Settings that were literals in the source until this fork. Every default below is the value
// upstream compiled in, so a config.txt that names none of them changes nothing.
static double trigger_level = 2.0;                  // volts, at the External1 input
static bool trigger_rising = true;                  // false selects the falling edge
static double full_scale_range = 0.5;               // volts peak to peak, channel 1
static int zero_suppress_threshold = -32667;        // ADC codes, must fit int16_t
static int zero_suppress_hysteresis = 100;          // ADC codes, must fit uint16_t
static int control_io_port = 2;                     // 1, 2 or 3; the port carrying In-TriggerEnable

//  std::stod and std::stoi say "invalid stod argument" and nothing about which line of
//  config.txt is wrong. These say.
static double config_double(const Config &config, const std::string &key, double fallback)
{
	if (!config.has_key(key))
		return fallback;

	try {
		return std::stod(config.get_value(key));
	} catch (const std::exception &) {
		throw std::runtime_error(key + " must be a number, got \"" + config.get_value(key) + "\"");
	}
}

static int config_int(const Config &config, const std::string &key, int fallback)
{
	if (!config.has_key(key))
		return fallback;

	try {
		return std::stoi(config.get_value(key));
	} catch (const std::exception &) {
		throw std::runtime_error(key + " must be a whole number, got \"" + config.get_value(key) + "\"");
	}
}

static std::string control_io_port_name()
{
	switch (control_io_port)
	{
	case 1: return SA220::control_io_1;
	case 3: return SA220::control_io_3;
	default: return SA220::control_io_2;
	}
}

//  A value this console can name as wrong is refused here rather than passed to the driver.
//  Trigger level and full scale are not among them: the card's own list of ranges has not been
//  read yet, so the driver is left to refuse what it will not accept.
static void reject_bad_settings()
{
	if (zero_suppress_threshold < -32768 || zero_suppress_threshold > 32767)
		throw std::runtime_error("ZeroSuppressThreshold must be between -32768 and 32767, got "
			+ std::to_string(zero_suppress_threshold));

	if (zero_suppress_hysteresis < 0 || zero_suppress_hysteresis > 65535)
		throw std::runtime_error("ZeroSuppressHysteresis must be between 0 and 65535, got "
			+ std::to_string(zero_suppress_hysteresis));

	if (control_io_port < 1 || control_io_port > 3)
		throw std::runtime_error("ControlIoPort must be 1, 2 or 3, got "
			+ std::to_string(control_io_port));
}


static void print_config_value(const std::string& key, const std::string &value, bool is_found) {
	std::string msg = "Config value \"" + key;
	if (is_found)
	{
		msg += "\" found, value set to " + value;
	}
	else
	{
		msg += " not found, value defaulted to " + value;
	}

	spdlog::info(msg);
}

void configure_logger(spdlog::level::level_enum log_level)
{
	try 
	{
		auto daily_sink = std::make_shared<spdlog::sinks::daily_file_sink_mt>("logs/log", 2, 0);
		daily_sink->set_level(spdlog::level::trace);

		auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
		console_sink->set_level(spdlog::level::trace);

		auto logger = std::make_shared<spdlog::logger>("aqmd3", spdlog::sinks_init_list( { daily_sink, console_sink }));
		logger->set_level(log_level);
		spdlog::set_default_logger(logger);

		//  The console has no quit command: server->run() loops until the process is killed, and
		//  a killed process never runs spdlog's sink destructors. Without these two lines the log
		//  file is empty after every ordinary shutdown.
		logger->flush_on(spdlog::level::info);
		spdlog::flush_every(std::chrono::seconds(1));

		spdlog::info("Logger initialized");
	}
	catch (const spdlog::spdlog_ex& ex)
	{
		std::cout << "Log init failed: " << ex.what() << std::endl;
	}
}

std::optional<Config> configure_settings()
{
	auto config = Config("config.txt");
	bool has_config = config.exists();
	std::optional<Config> return_config;

	if (has_config)
	{
		config.read();

		post_trigger_delay = config.has_key("PostTriggerDelay") ? std::stod(config.get_value("PostTriggerDelay")) : post_trigger_delay;
		estimated_trigger_rearm_time = config.has_key("TriggerRearmDeadTime") ? std::stod(config.get_value("TriggerRearmDeadTime")) : estimated_trigger_rearm_time;
		resource_name = config.has_key("ResourceName") ? config.get_value("ResourceName") : resource_name;
		notify_on_scans_count = config.has_key("NotifyOnScansCount") ? std::stoull(config.get_value("NotifyOnScansCount")) : notify_on_scans_count;
		acquisition_timeout_ms = config.has_key("AcquisitionTimeoutMs") ? std::stoll(config.get_value("AcquisitionTimeoutMs")) : acquisition_timeout_ms;
		acquisition_initial_buffer_count = config.has_key("AcquisitionInitialBufferCount") ? std::stoull(config.get_value("AcquisitionInitialBufferCount")) : acquisition_initial_buffer_count;
		acquisition_max_buffer_count = config.has_key("AcquisitionMaxBufferCount") ? std::stoull(config.get_value("AcquisitionMaxBufferCount")) : acquisition_max_buffer_count;
		acquisition_buffer_reserve_elements_count = config.has_key("AcquisitionBufferReserveElementsCount") ? std::stoull(config.get_value("AcquisitionBufferReserveElementsCount")) : acquisition_buffer_reserve_elements_count;
		log_level = config.has_key("LogLevel") ? config.get_value("LogLevel") : log_level;

		trigger_level = config_double(config, "TriggerLevel", trigger_level);
		full_scale_range = config_double(config, "FullScaleRange", full_scale_range);
		zero_suppress_threshold = config_int(config, "ZeroSuppressThreshold", zero_suppress_threshold);
		zero_suppress_hysteresis = config_int(config, "ZeroSuppressHysteresis", zero_suppress_hysteresis);
		control_io_port = config_int(config, "ControlIoPort", control_io_port);

		if (config.has_key("TriggerSlope"))
		{
			std::string slope = config.get_value("TriggerSlope");
			slope.erase(slope.find_last_not_of(" \t\r\n") + 1);
			for (auto &c : slope)
				c = (char)std::tolower(c);

			if (slope == "rising")
				trigger_rising = true;
			else if (slope == "falling")
				trigger_rising = false;
			else
				throw std::runtime_error("TriggerSlope must be rising or falling, got " + slope);
		}

		reject_bad_settings();

		return_config = config;
	}

	return return_config;
}

void print_config(Config& config)
{
	print_config_value("PostTriggerDelay", std::to_string(post_trigger_delay), config.has_key("PostTriggerDelay"));
	print_config_value("TriggerRearmDeadTime", std::to_string(estimated_trigger_rearm_time), config.has_key("TriggerRearmDeadTime"));
	print_config_value("ResourceName", resource_name, config.has_key("ResourceName"));
	print_config_value("NotifyOnScansCount", std::to_string(notify_on_scans_count), config.has_key("NotifyOnScansCount"));
	print_config_value("AcquisitionTimeoutMs", std::to_string(acquisition_timeout_ms), config.has_key("AcquisitionTimeoutMs"));
	print_config_value("AcquisitionInitialBufferCount", std::to_string(acquisition_initial_buffer_count), config.has_key("AcquisitionInitialBufferCount"));
	print_config_value("AcquisitionMaxBufferCount", std::to_string(acquisition_max_buffer_count), config.has_key("AcquisitionMaxBufferCount"));
	print_config_value("AcquisitionBufferReserveElementsCount", std::to_string(acquisition_buffer_reserve_elements_count), config.has_key("AcquisitionBufferReserveElementsCount"));
	print_config_value("TriggerLevel", std::to_string(trigger_level), config.has_key("TriggerLevel"));
	print_config_value("TriggerSlope", trigger_rising ? "rising" : "falling", config.has_key("TriggerSlope"));
	print_config_value("FullScaleRange", std::to_string(full_scale_range), config.has_key("FullScaleRange"));
	print_config_value("ZeroSuppressThreshold", std::to_string(zero_suppress_threshold), config.has_key("ZeroSuppressThreshold"));
	print_config_value("ZeroSuppressHysteresis", std::to_string(zero_suppress_hysteresis), config.has_key("ZeroSuppressHysteresis"));
	print_config_value("ControlIoPort", std::to_string(control_io_port), config.has_key("ControlIoPort"));
}

std::map<std::string, spdlog::level::level_enum> get_log_levels_map()
{
	std::map<std::string, spdlog::level::level_enum> log_levels_map;
	log_levels_map["trace"] = spdlog::level::level_enum::trace;
	log_levels_map["debug"] = spdlog::level::level_enum::debug;
	log_levels_map["info"] = spdlog::level::level_enum::info;
	log_levels_map["warn"] = spdlog::level::level_enum::warn;
	log_levels_map["err"] = spdlog::level::level_enum::err;
	log_levels_map["critical"] = spdlog::level::level_enum::critical;
	log_levels_map["off"] = spdlog::level::level_enum::off;
	
	return log_levels_map;
}

spdlog::level::level_enum get_log_level_from_string(const std::string& level_str)
{
	auto log_levels_map = get_log_levels_map();
	std::string level_str_lower = "";
	spdlog::level::level_enum level_enum_value = spdlog::level::level_enum::info;

	for (auto c : level_str)
	{
		level_str_lower += std::tolower(c);
	}

	auto it = log_levels_map.find(level_str_lower);
	if (it != log_levels_map.end())
	{
		level_enum_value = it->second;
	}

	return level_enum_value;
}

int main(int argc, char *argv[]) {
	try
	{
		// Disable 'Quick Edit Mode' since it can cause the application to hang during acquisition
		disable_quick_edit();

		std::optional<Config> config;
		try
		{
			config = configure_settings();
		}
		catch (const std::exception &ex)
		{
			//  configure_settings runs before the logger, since the log level is one of the
			//  things it reads. Give the complaint a log file to land in before repeating it.
			configure_logger(spdlog::level::level_enum::info);
			spdlog::critical("config.txt is not usable, application exiting");
			spdlog::critical(ex.what());
			return 1;
		}

		if (!config)
		{
			configure_logger(spdlog::level::level_enum::info);
			spdlog::info("No config found, using default values.");
		}
		else
		{
			auto log_level_str = config->has_key("LogLevel") ? config->get_value("LogLevel") : "info";
			auto log_level = get_log_level_from_string(log_level_str);
			configure_logger(log_level);
			print_config(*config);
		}

#if TEST_ACQUIRE
		std::unique_ptr<SA220> digitizer = std::make_unique<SA220>(resource_name, true);
#else
		std::unique_ptr<SA220> digitizer = std::make_unique<SA220>(resource_name, false);
#endif

		auto server = new Server("tcp://*:5555");
		double sampling_rate = 0.0;
		std::unique_ptr<AcquisitionControl> controller;
		std::shared_ptr<AcquisitionBufferPool> buffer_pool = nullptr;

#if REUSABLE_PUB_SUB
		int record_size_c = 0;
		std::shared_ptr<StreamingContext> context;
		std::shared_ptr<ZmqAcquiredDataSubscriber> zmq_publisher;
		std::shared_ptr<UimfFrameWriterSubscriber> frame_writer = std::make_shared<UimfFrameWriterSubscriber>(false);
#endif // reusable_pub_sub

		server->register_handler([&](Server::ReceivedRequest req)
			{
				for (const auto& command : req.payload)
				{
					spdlog::debug("command: " + command);

					if (command == "num instruments")
					{
						ViSession rm = VI_NULL;
						viOpenDefaultRM(&rm);
						ViChar search[] = "PXI?*::INSTR";
						ViFindList find = VI_NULL;
						ViUInt32 count = 0;
						ViChar rsrc[256];

						ViStatus status = viFindRsrc(rm, search, &find, &count, rsrc);
						viClose(rm);

						req.send_response(std::to_string(count));
						return;
					}

					if (command == "info")
					{
						auto info = digitizer->get_info();
						auto info_str = std::format("Digitizer Model: {} / Digitizer Serial No.: {} / Digitizer Firmware Version: {} / App: {} / App Version: {}-{} / Fork: {}@{}",
							info.instrument_model,
							info.serial_number,
							info.firmware_revision,
							PROJECT_NAME_S,
							AqMD3_console_VERSION_S,
							GIT_COMMIT_HASH,
							FORK_S,
							GIT_BRANCH);
						req.send_response(info_str);
					}

					if (command == "firmware")
					{
						auto info = digitizer->get_info();
						req.send_response(info.firmware_revision);
					}

					if (command == "serial")
					{
						auto info = digitizer->get_info();
						req.send_response(info.serial_number);
					}

					if (command == "init")
					{
						digitizer->set_trigger_parameters(digitizer->trigger_external, trigger_level, trigger_rising, post_trigger_delay);

						req.send_response(ack);
						continue;
					}

					if (command == "horizontal")
					{
						if (req.payload.size() == 2)
						{
							auto horizontal_resolution = std::stod(req.payload[1]);
							sampling_rate = 1.0 / horizontal_resolution;
							digitizer->set_sampling_rate(sampling_rate);
						}

						req.send_response(ack);
						return;
					}

					if (command == "vertical")
					{
						if (req.payload.size() == 2)
						{
							auto offset_v = std::stod(req.payload[1]);
							digitizer->set_channel_parameters(digitizer->channel_1, full_scale_range, offset_v);
						}

						req.send_response(ack);
						return;
					}

					if (command == "trig class")
					{
						// TODO trig class
						continue;
					}

					if (command == "trig source")
					{
						// TODO trig source
						continue;
					}

					if (command == "mode")
					{
						// TODO mode
						continue;
					}

					if (command == "config digitizer")
					{
						// TODO config digitizer
						continue;
					}

					if (command == "post samples")
					{
						continue;
					}

					if (command == "pre samples")
					{
						continue;
					}

					if (command == "setup array")
					{

						req.send_response(ack);
						return;
					}

					if (command == "acquire frame")
					{
						if (req.payload.size() == 2)
						{
#if TIMING_INFORMATION
							auto t_0 = std::chrono::high_resolution_clock::now();
#endif
							std::string uimf_req_msg;
							snappy::Uncompress(req.payload[1].data(), req.payload[1].size(), &uimf_req_msg);
							auto uimf = UimfRequestMessage();
							uimf.MergeFromString(uimf_req_msg);

							// Start acquire. Waits for external enabe signal.
							auto uimf_frame_params = UIMFHelpers::uimf_message_to_parameters(uimf);
							//UIMFHelpers::log_debug_uimf_frame_params(uimf_frame_params);
							controller->start(uimf_frame_params);

#if TIMING_INFORMATION
							auto t_1 = std::chrono::high_resolution_clock::now();
							auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(t_1 - t_0);
							std::cout << "time to setup acquire frame:" << diff.count() << "\n";
#endif
						}

						req.send_response(ack);
						return;
					}

					if (command == "acquire")
					{
#if TEST_ACQUIRE
						auto timing = AqirisDigitizer::TofTimingInformation(
							200000,
							200000 - 20000 - 20000,
							20000,
							20000,
							post_trigger_delay,
							estimated_trigger_rearm_time);
#else
						auto timing = AqirisDigitizer::TofTimingInformation::create_timing_information(digitizer.get(), sampling_rate, post_trigger_delay, estimated_trigger_rearm_time);
#endif
						uint64_t record_size = timing.get_record_size();
						uint64_t post_trigger_samples = timing.get_post_trigger_delay_samples();
						uint64_t tof_width = timing.get_samples_per_trigger();
						spdlog::info("tof width: " + std::to_string(tof_width));
						spdlog::info("samples per trigger: " + std::to_string(record_size + post_trigger_samples));
						spdlog::info("record size: " + std::to_string(record_size));
						spdlog::info("post trigger samples: " + std::to_string(post_trigger_samples));
						digitizer->set_record_size(record_size);
						avg_tof_period_samples = tof_width;
						calculated_post_trigger_samples = post_trigger_samples;

						auto data_pub = server->get_publisher("tcp://*:5554");

						if (buffer_pool == nullptr)
						{
							buffer_pool = std::make_shared<AcquisitionBufferPool>(notify_on_scans_count, avg_tof_period_samples, acquisition_initial_buffer_count, acquisition_max_buffer_count);
						}
#if TEST_ACQUIRE
						context = std::make_shared<DataGeneratorContext>(dynamic_cast<const Digitizer&>(*digitizer), digitizer->channel_1, notify_on_scans_count, buffer_pool);
#else
						context = digitizer->configure_cst(digitizer->channel_1, buffer_pool,
							Digitizer::ZeroSuppressParameters((int16_t)zero_suppress_threshold, (uint16_t)zero_suppress_hysteresis));
#endif

						std::unique_ptr<AcquirePublisher> p = std::make_unique<AcquirePublisher>(context, acquisition_timeout_ms, buffer_pool, notify_on_scans_count, data_pub);
						std::shared_ptr<ProcessSubject> ps = std::make_shared<ProcessSubject>(tof_width);
						std::shared_ptr<UimfFrameWriterSubscriber> fw = std::make_shared<UimfFrameWriterSubscriber>(false);
						std::shared_ptr<ZmqAcquiredDataSubscriber> zmq = std::make_shared<ZmqAcquiredDataSubscriber>(data_pub, record_size + post_trigger_samples);
						ps->Publisher<frame_ptr>::register_subscriber(zmq, SubscriberType::ACQUIRE);
						ps->Publisher<frame_ptr>::register_subscriber(fw, SubscriberType::ACQUIRE_FRAME);
						p->register_subscriber(ps, SubscriberType::BOTH);

						controller = std::move(p);

						auto uimf_frame_params = UIMFHelpers::create_inf_params();
						controller->start(uimf_frame_params);

						std::vector<std::string> to_send(2);

						TofWidthMessage tofMsg;
						tofMsg.set_num_samples(record_size + post_trigger_samples);
						tofMsg.set_pusher_pulse_width(tof_width);
						to_send[0] = (tofMsg.SerializeAsString());
						std::vector<uint8_t> hash(picosha2::k_digest_size);

						picosha2::hash256(to_send[0].begin(), to_send[0].end(), hash.begin(), hash.end());

						to_send[1] = picosha2::bytes_to_hex_string(hash.begin(), hash.end());
						req.send_responses(to_send);

						return;
					}

					if (command == "tof width")
					{
						auto timing = AqirisDigitizer::TofTimingInformation::create_timing_information(digitizer.get(), sampling_rate, post_trigger_delay, estimated_trigger_rearm_time);
						uint64_t record_size = timing.get_record_size();
						uint64_t post_trigger_samples = timing.get_post_trigger_delay_samples();
						uint64_t tof_width = timing.get_samples_per_trigger();
						spdlog::info("samples per trigger: " + std::to_string(record_size + post_trigger_samples));
						spdlog::info("record size: " + std::to_string(record_size));
						spdlog::info("post trigger samples: " + std::to_string(post_trigger_samples));
						digitizer->set_record_size(record_size);

						std::vector<std::string> to_send(2);

						TofWidthMessage tofMsg;
						tofMsg.set_num_samples(record_size + post_trigger_samples);
						tofMsg.set_pusher_pulse_width(tof_width);
						to_send[0] = (tofMsg.SerializeAsString());
						std::vector<uint8_t> hash(picosha2::k_digest_size);
						picosha2::hash256(to_send[0].begin(), to_send[0].end(), hash.begin(), hash.end());

						to_send[1] = picosha2::bytes_to_hex_string(hash.begin(), hash.end());
						req.send_responses(to_send);

						return;
					}

					if (command == "stop")
					{
						spdlog::debug(std::format("stop payload size: {}", req.payload.size()));
						if (req.payload.size() != 2)
						{
							return;
						}

						try
						{
							if (controller)
							{
#if TIMING_INFORMATION
								auto t_0 = std::chrono::high_resolution_clock::now();
#endif
								auto stop_acquire = req.payload[1] == "acquire";
								controller->stop(stop_acquire);
#if TIMING_INFORMATION
								auto t_1 = std::chrono::high_resolution_clock::now();
								auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(t_1 - t_0);
								std::cout << "time to stop:" << diff.count() << "\n";
#endif
							}
						}
						catch (const std::exception& ex)
						{
							spdlog::error("Error when trying to stop acquisition loop: " + std::string(ex.what()));
						}
						catch (...)
						{
							spdlog::error("Unknown error when trying to stop acquisition loop");
						}

						req.send_response(ack);
						return;
					}

					if (command == "invert")
					{
						if (req.payload.size() == 2)
						{
							bool invert = req.payload[1] == "true";
							digitizer->set_channel_data_inversion(digitizer->channel_1, invert);
						}
						req.send_response(ack);
						return;
					}

					if (command == "reset timestamps")
					{
						return;
					}

					if (command == "enable io port")
					{
						if (req.payload.size() == 2)
						{
							auto val = std::stoi(req.payload[1]);
							digitizer->enable_io_port(control_io_port_name());
						}

						req.send_response(ack);
						return;
					}

					if (command == "disable io port")
					{
						if (req.payload.size() == 2)
						{
							auto val = std::stoi(req.payload[1]);
							digitizer->disable_io_port(control_io_port_name());
						}

						req.send_response(ack);
						return;
					}
				}
			});

		server->run();

		return 0;
	}
	catch (const std::exception& ex)
	{
		spdlog::critical("AqMD3 console application has encountered an error that it cannot recover from, application exiting");
		spdlog::critical(ex.what());
	}
	catch (...)
	{
		spdlog::critical("AqMD3 console application has encountered an unknown error that it cannot recover from, application exiting");
	}

	return 1;
}
