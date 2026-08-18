// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Retains only cumulative values required for Runtime shutdown and error diagnostics.
 * Unreal Insights STAT/TRACE instrumentation handles detailed per-Frame performance
 * measurement.
 *
 * This struct does not determine rendering policy. Its values must not affect capture
 * cadence or publication
 * results; they are used only for shutdown logs and error-frequency inspection. Unlike
 * the previous periodic
 * summary counters, it does not accumulate dozens of values each Frame and retains only
 * two values useful for
 * long-term diagnostics.
 *
 * For detailed performance analysis, use Unreal Insights `STAT_WP_*`, CPU scopes, and
 * Renderer GPU events
 * rather than adding fields here. This avoids per-Frame counter updates and periodic
 * log-string construction
 * during normal play while still allowing resource/publication lifetime anomalies to be
 * detected at shutdown.
 */
struct FWPRuntimeTelemetry
{
	/**
	 * Number of Packets committed after Renderer::UpdatePair succeeded during the Subsystem
	 * lifetime.
	 * The value persists when a Pair is removed and is reported in the final Deinitialize
	 * log.
	 */
	uint64 PublishedPacketCount = 0;

	/**
	 * Number of ACKs rejected because their Ownership Epoch or Packet Sequence did not
	 * match the current warmup
	 * request. Used to investigate delayed Renderer feedback or a stale mailbox; never used
	 * as normal policy input.
	 */
	uint64 RejectedOwnershipAckCount = 0;

	/** Resets all cumulative diagnostic values to zero before a new World lifetime begins. */
	void Reset()
	{
		PublishedPacketCount = 0;
		RejectedOwnershipAckCount = 0;
	}
};
