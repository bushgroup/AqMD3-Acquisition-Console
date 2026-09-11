#include "UIMFWriter/UIMFWriter.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <spdlog/spdlog.h>
#include <iostream>
#include <fstream>

const std::string UimfWriter::frames_table_name = "Frame_Params";

UimfWriter::UimfWriter(std::string file)
	: db(file, SQLite::OPEN_READWRITE)
{}

//  `BPI_MZ` is defined as an m/z and upstream stores a bin index in it, which makes a raw
//  file disagree with every other UIMF writer and with the summed companion the client folds
//  beside it (lab record, task 24). The calibration is in the file this writer already holds
//  open: the frame's own `CalibrationSlope` and `CalibrationIntercept`, and the global
//  `BinWidth`. A frame that states no calibration keeps the bin index, since a bin index is
//  what a file with no mass axis has to offer.
const UimfCalibration& UimfWriter::calibration_for(uint32_t frame_number)
{
	if (calibration_read)
		return calibration;

	calibration_read = true;

	try
	{
		SQLite::Statement frame_params(db,
			"SELECT ParamID, ParamValue FROM " + frames_table_name
			+ " WHERE FrameNum = ? AND ParamID IN (?, ?)");
		frame_params.bind(1, (int)frame_number);
		frame_params.bind(2, (int)CalibrationParamKeyType::CalibrationSlope);
		frame_params.bind(3, (int)CalibrationParamKeyType::CalibrationIntercept);

		while (frame_params.executeStep())
		{
			int const id = frame_params.getColumn(0).getInt();
			double const value = frame_params.getColumn(1).getDouble();
			if (id == CalibrationParamKeyType::CalibrationSlope)
				calibration.slope = value;
			else
				calibration.intercept = value;
		}

		SQLite::Statement bin_width(db,
			"SELECT ParamValue FROM Global_Params WHERE ParamID = ?");
		bin_width.bind(1, (int)GlobalParamKeyType::BinWidth);
		if (bin_width.executeStep())
			calibration.bin_width_ns = bin_width.getColumn(0).getDouble();
	}
	catch (SQLite::Exception& ex)
	{
		//  A file whose parameters cannot be read is still a file worth writing scans to.
		spdlog::warn("Could not read the calibration of frame " + std::to_string(frame_number)
			+ ", storing bin indices in BPI_MZ: " + std::string(ex.what()));
		calibration = UimfCalibration();
		return calibration;
	}

	calibration.usable = calibration.slope > 0.0 && calibration.bin_width_ns > 0.0;

	if (!calibration.usable)
		spdlog::info("Frame " + std::to_string(frame_number)
			+ " states no usable calibration, storing bin indices in BPI_MZ");

	return calibration;
}

int UimfWriter::write_scan_data(const UimfFrame& frame)
{
	int bytes = 0;
	std::string insert_scan_statement = "INSERT INTO Frame_Scans (FrameNum, ScanNum, NonZeroCount, BPI, BPI_MZ, TIC, Intensities) VALUES(%d, %d, %d, %d, %lf, %d, ?)";
	int insert_scan_statement_size_bytes = 110;

	const std::lock_guard<std::mutex> lock(sync);

	auto const& calibration = calibration_for(frame.parameters().frame_number);

	int const extra = 128;

	SQLite::Statement sync_off(db, "PRAGMA synchronous=0");
	sync_off.exec();
	SQLite::Statement b_trans(db, "BEGIN TRANSACTION");
	b_trans.exec();
		
	char *statement = new char[insert_scan_statement_size_bytes + extra];

	for (auto& er : frame.data())
	{
		if (er.scan < frame.parameters().start_trigger)
			continue;

		if (er.encoded_spectra.size() > 1 || er.scan == 0)
		{
			int count = sprintf(statement,
				insert_scan_statement.c_str(),
				frame.parameters().frame_number,
				er.scan - frame.parameters().start_trigger,
				er.non_zero_count,
				er.bpi,
				calibration.usable ? calibration.mz(er.index_max_intensity)
				                   : double(er.index_max_intensity),
				er.tic);

			auto compressed = er.get_compressed_spectra();

			bytes += compressed.size + count;

			SQLite::Statement sql_statement(db, (const char *)statement);
			sql_statement.bind(1, compressed.data, compressed.size);
			sql_statement.exec();
		}
	}

	SQLite::Statement e_trans(db, "END TRANSACTION");
	e_trans.exec();
	SQLite::Statement sync_on(db, "PRAGMA synchronous=1");
	sync_on.exec();

	return bytes;
}

void UimfWriter::update_timing_information(const UimfFrame& frame, double timestamp_sample_period_s)
{
	std::string frame_update_statement = "UPDATE Frame_Params SET ParamValue = ? WHERE FrameNum = ? AND ParamID = ?";
	SQLite::Statement statement(db, frame_update_statement);

	const std::lock_guard<std::mutex> lock(sync);

	try
	{
		auto frame_duration_seconds = frame.get_frame_duration_seconds(timestamp_sample_period_s);
		statement.bind(1, std::to_string(frame_duration_seconds));
		statement.bind(2, std::to_string(frame.parameters().frame_number));
		statement.bind(3, std::to_string(FrameParamKeyType::DurationSeconds));

		statement.exec();
	}
	catch (...)
	{
	}

}
