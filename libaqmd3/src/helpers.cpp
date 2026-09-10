#include "../include/libaqmd3/helpers.h"

void check_fetch_alignment(const std::string& stream, int64_t elements_to_fetch)
{
	if (elements_to_fetch <= 0 || elements_to_fetch % 16 != 0)
	{
		throw std::runtime_error(std::format(
			"refusing to fetch {} elements from {}: nbrElementsToFetch must be a strict positive "
			"multiple of 16", elements_to_fetch, stream));
	}
}

void check_and_throw_on_error(std::pair<std::string, enum Digitizer::ErrorType> err)
{
	if (err.second == Digitizer::Error)
	{
		throw std::runtime_error(std::format("Error during call to digitizer. {}", err.first));
	}
}