#ifndef HELPERS_H
#define HELPERS_H

#include "../include/libaqmd3/helpers.h"
#include "../include/libaqmd3/digitizer.h"

#include <format>

template <typename T>
T check_and_throw_on_error(std::tuple<std::string, enum Digitizer::ErrorType, T> err)
{
	if (std::get<1>(err) == Digitizer::Error)
	{
		throw std::runtime_error(std::format("Error during call to digitizer. {}", std::get<0>(err)));
	}

	return std::get<2>(err);
}

void check_and_throw_on_error(std::pair<std::string, enum Digitizer::ErrorType> err);

// Every stream fetch has to ask for a strictly positive multiple of 16 elements; the driver
// refuses anything else, naming nbrElementsToFetch. Both counts in the streaming contexts are
// multiples of 16 by construction, so this is an assertion rather than a correction: rounding a
// request up would consume stream data belonging to the next read instead of fixing anything.
void check_fetch_alignment(const std::string& stream, int64_t elements_to_fetch);

#endif