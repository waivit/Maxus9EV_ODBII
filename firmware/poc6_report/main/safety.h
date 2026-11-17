/*
 * Read-only safety gate for the inspection device (docs/inspection-safety.md).
 *
 * The ELM327 sends whatever it is told to, so the firmware enforces the
 * "reads only, changes nothing in the car" promise itself. Fail-closed: a
 * command that is not explicitly allowed is refused.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* True only for commands on the allowlist. `cmd` is the text sent to the
 * ELM327 without the trailing CR (e.g. "ATSH 7E5", "22 B0 42"). Case and
 * spacing are normalised. Anything with control characters, an unknown
 * service or a VIN request is refused. */
bool safety_cmd_allowed(const char *cmd);

/* Replace every run of >= 17 consecutive VIN-alphabet bytes (0-9, A-Z except
 * I O Q) with '*'. Returns the number of bytes masked. Call on reassembled
 * reply data before it is stored or shown. */
int safety_scrub_vin(uint8_t *data, int n);

/* Boot-time check of the cases above; logs each one and returns the number of
 * failures. A regression guard, not proof about any car. */
int safety_selftest(void);
