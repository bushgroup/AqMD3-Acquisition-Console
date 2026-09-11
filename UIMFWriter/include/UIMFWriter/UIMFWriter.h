#ifndef UIMFWRITER_H
#define UIMFWRITER_H

#include "encodedresult.h"
#include "uimfframe.h"

#include <SQLiteCpp/SQLiteCpp.h>
#include <memory>
#include <string>
#include <vector>
#include <iostream>
#include <tuple>
#include <mutex>

//  The calibration one frame states, as the file states it. `usable` is false when the
//  frame carries no mass axis, which is every frame of a file acquired without one.
struct UimfCalibration
{
	double slope = 0.0;
	double intercept = 0.0;
	double bin_width_ns = 0.0;
	bool usable = false;

	//  UIMF-Library's calibration: m/z is the square of the slope times the flight time
	//  past the intercept, in microseconds. Time is clamped at the intercept, so the bins
	//  before the calibration's own origin come out at m/z 0 rather than at a mirror image
	//  of the low mass range. `slope` is K itself and not K / 10000, which is what every
	//  file read so far stores (lab record, task 01).
	double mz(int32_t bin) const
	{
		double const time_us = double(bin) * bin_width_ns / 1000.0;
		double const past_origin = time_us - intercept;
		if (past_origin <= 0.0)
			return 0.0;
		return (slope * past_origin) * (slope * past_origin);
	}
};

class UimfWriter {
private:
	static const std::string frames_table_name;
	std::mutex sync;
	SQLite::Database db;
	bool calibration_read = false;
	UimfCalibration calibration;

public:
	UimfWriter(std::string file);

	int write_scan_data(const UimfFrame& frame);
	void update_timing_information(const UimfFrame& frame, double timestamp_sample_period_s);

private:
	//  Read once per writer, which is once per batch of scans, from the parameters the
	//  client wrote before it asked for the frame.
	const UimfCalibration& calibration_for(uint32_t frame_number);

	enum FrameParamKeyType
	{
		StartTimeMinutes = 1,
		DurationSeconds = 2,
	};

	enum GlobalParamKeyType
	{
		BinWidth = 5,
	};

	enum CalibrationParamKeyType
	{
		CalibrationSlope = 12,
		CalibrationIntercept = 13,
	};

};

#endif // !UIMFWRITER_H
