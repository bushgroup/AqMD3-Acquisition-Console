#ifndef CST_CONTEXT_H
#define CST_CONTEXT_H

#include "streamingcontext.h"

class CstContext : public StreamingContext {
private:
	// One marker is 16 int32 elements, the same hunk CstZs1Context walks. Named here
	// because this path now has to skip hunks rather than assume every one is a trigger.
	static const int64_t markers_hunk_size = 16;

	// How many times the base request one fetch may grow to when the stream has a backlog.
	// CstZs1Context's equivalent is 8; this path walks past markers rather than using them,
	// so it wants a longer stride, and its buffer is sized for the largest request.
	static const int64_t markers_multiplier_max = 16;

public:
	CstContext(const Digitizer& digitizer, std::string channel, std::shared_ptr<AcquisitionBufferPool> buffer_pool)
		: StreamingContext(digitizer, channel, buffer_pool)
	{}

	AcquiredData acquire(uint64_t triggers_to_read, std::chrono::milliseconds timeoutMs) override;
};

#endif // !CST_CONTEXT_H
