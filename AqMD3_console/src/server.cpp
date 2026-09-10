#include "../include/server.h"

#define NOMINMAX 
#undef min
#undef max
#include "../include/message.pb.h"

#include <map>
#include <stdexcept>
#include <string>

#include <spdlog/spdlog.h>

namespace
{
	// A reply is one frame, and a driver error is not one line: the AqMD3 messages carry a
	// code and an explanation with newlines between them. Same treatment the status topic's
	// error messages get in acquirepublisher.cpp.
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

static inline bool send(zmq::socket_t& socket, const std::string& message) {

	zmq::message_t zmq_msg(message.size());
	memcpy((void *)zmq_msg.data(), message.data(), message.size());
	return socket.send(zmq_msg);
}

static inline bool send_more(zmq::socket_t& socket, const std::string& message) {
	
	zmq::message_t zmq_msg(message.size());
	memcpy((void *)zmq_msg.data(), message.data(), message.size());
	return socket.send(zmq_msg, ZMQ_SNDMORE);
}

void Server::respond(const std::string& client, const std::string& response) {
	responded_to_request = true;
	// addr frame
	send_more(router, client);
	// null frame
	send_more(router, "");
	// message frame
	send(router, response);
}

void Server::respond_more(const std::string& client, const std::vector<std::string>& responses) {
	
	if (responses.size() == 0)
		return;
	if (responses.size() == 1) {
		this->respond(client, responses[0]);
		return;
	}

	responded_to_request = true;
	send_more(router, client);
	send_more(router, "");

	for (auto response = responses.begin(); response != std::prev(responses.end()); response++)
	{
		send_more(router, *response);
	}

	send(router, responses.back());
}

std::tuple<std::string, std::vector<std::string>> Server::receive() {
	zmq::message_t message;
	std::vector<std::string> payload;

	int more;
	size_t more_size = sizeof(more);

	/* begin receive */
	// get id
	router.recv(&message);
	std::string client = std::string(static_cast<char*>(message.data()), message.size());

	// null msg
	router.recv(&message);

	// payload
	do {
		router.recv(&message);
		router.getsockopt(ZMQ_RCVMORE, &more, &more_size);
		std::string msg = std::string(static_cast<char*>(message.data()), message.size());
		payload.push_back(msg);
	} while (more);

	return std::make_tuple(client, payload);
}

void Server::run() 
{
	should_run = true;

	zmq::pollitem_t items[] = {
		{static_cast<void*>(router), 0, ZMQ_POLLIN, 0 }
	};

	while (should_run)
	{
		//std::cout << "Polling ..." << std::endl;
		zmq::poll(&items[0], 1, 1);

		if (!(items[0].revents & ZMQ_POLLIN))
			continue;

		std::string id;
		std::vector<std::string> msgs;
		std::tie(id, msgs) = receive();

		if (msgs.size() <= 0)
			continue;

		if (message_handler == NULL)
			continue;

		// A command that throws answers with an error and the server stays up.
		//
		// There was no boundary here at all. Every handler ran inside the poll loop with
		// nothing between it and main's catch, so any exception out of any command unwound
		// through run(), was logged as critical, and returned from main: the console exited.
		// One instance of that is what task 21 is about -- a record size the driver refused
		// after an overflow spoiled the period measurement -- but the record size is one
		// case of a general shape, and the general shape is what this fixes.
		//
		// The client is told, on the command socket, in one frame. It has to be told
		// something: it is blocked on a reply, and a console that stays up while its client
		// waits out a timeout is only a quieter way to lose the session. `error <what>` is
		// additive in the sense that matters -- no existing command's successful reply
		// changes -- and it is documented in docs/console-protocol.md.
		//
		// Only if the handler has not already answered. A few handlers do work after their
		// reply, and a second reply on a ROUTER socket would leave a frame in the client's
		// queue to be read as the answer to whatever it sent next.
		responded_to_request = false;
		try
		{
			message_handler(ReceivedRequest(*this, id, msgs));
		}
		catch (const std::exception& ex)
		{
			spdlog::error("Error handling command '" + msgs[0] + "': " + std::string(ex.what()));
			if (!responded_to_request)
			{
				respond(id, "error " + one_line(ex.what()));
			}
		}
		catch (...)
		{
			spdlog::error("Unknown error handling command '" + msgs[0] + "'");
			if (!responded_to_request)
			{
				respond(id, "error unknown error handling command " + msgs[0]);
			}
		}
	}
}

void Server::stop() {
	should_run = false;
}

void Server::register_handler(std::function<void(const ReceivedRequest)> handler)
{
	message_handler = handler;
}

std::shared_ptr<Server::Publisher> Server::get_publisher(std::string address)
{
	auto publisher = publishers[address].lock();
	if (!publisher)
	{
		zmq::socket_t sock(context, ZMQ_PUB);
		sock.bind(address);
		publishers[address] = publisher = std::make_shared<Server::Publisher>(std::move(sock), address);
	}
	return publisher;
}
