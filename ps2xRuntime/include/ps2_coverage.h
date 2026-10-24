#ifndef PS2_COVERAGE_H
#define PS2_COVERAGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* First-hit function-coverage marker.  The recompiler emits a call at each
 * function entry (and each resumable entry point) only when the recompile
 * config sets general.coverage = true, so there is zero cost when the flag is
 * off.  The first hit activates periodic dumps of the hit set to the file
 * named by $PS2X_COVERAGE_OUT (nothing is written when the variable is unset).
 */
void ps2CoverageHit(uint32_t address);

/* Write the hit set (sorted, one "0xADDR" per line) to $PS2X_COVERAGE_OUT.
 * No-op until the first ps2CoverageHit(). */
void ps2CoverageFlush(void);

/* Flush at most once every ~2 s of host time; no-op (one relaxed load) until
 * the first ps2CoverageHit().  Called from the EE scheduler checkpoints. */
void ps2CoveragePeriodicFlush(void);

#ifdef __cplusplus
}
#endif

#endif /* PS2_COVERAGE_H */
