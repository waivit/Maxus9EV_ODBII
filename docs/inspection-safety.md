# Inspection device: read-only safety policy

Applies to `firmware/poc6_report` from version **1.0.0**, the device taladkrub
uses to inspect customers' cars (hub handoff H-0138). The website's terms of use
(version 4) promise that the device *reads from the OBD port only and changes
nothing in the car*. This document records how the firmware enforces that, and
where it cannot.

## The rule

The ELM327 will put any frame on the bus that it is told to. The adapter
itself enforces nothing. Enforcement therefore lives in the firmware.
**Every** command to the ELM327 goes through one function, `elm_cmd()`, and
`elm_cmd()` refuses any command that `safety_cmd_allowed()` does not allow.
The design is **fail-closed**: anything not explicitly listed is blocked. That
includes malformed input and commands added later by mistake. A blocked command
is never sent. It is logged as `BLOCKED` on the serial console.

Decided by the user on 2026-11-17: **ELM327 only** (no native-CAN board), and
DTC reading stays **blocked** for now.

## Allowed

**Adapter configuration (AT commands).** The command name is matched exactly.
The name is the run of letters after `AT`. Any parameter must be 0–8 hex
digits, optionally after one space. A parameter that begins with a letter
(`ATSHE40`) merges into the name and is refused. Write it with a space
(`ATSH E40`). This is fail-closed on purpose.

| Name | Purpose |
| --- | --- |
| `Z` `I` `RV` `DPN` | reset adapter, identify, read 12 V, show protocol |
| `E` `L` `S` `H` | echo, linefeeds, spaces, headers on/off |
| `AT` `ST` `SP` | adaptive timing, timeout, protocol |
| `SH` `CRA` `CF` `CM` | request header, receive filters |
| `CSM` `MA` | silent monitoring, monitor all (listen only) |

**Requests to the car.** The hex bytes are parsed and the service byte is
checked:

| Service | Allowed | Excluded |
| --- | --- | --- |
| `01` (OBD current data) | yes | — |
| `09` (OBD vehicle info) | yes | `09 02` (VIN) |
| `22` (UDS ReadDataByIdentifier) | yes | DID `F190` (VIN) |

## Blocked (examples, not exhaustive — everything else is blocked too)

| Command | Why |
| --- | --- |
| `04`, `14` | clear diagnostic codes |
| `03`, `19` | read DTCs. Read-only, but blocked until there is a need (user, 2026-11-17) |
| `10` | diagnostic session control (the gate to writes) |
| `11` | ECU reset |
| `27` | security access |
| `2E`, `2F`, `31`, `34`–`37` | write data, IO control, routines, download |
| `3E`, `28`, `85` | tester present, communication control, DTC setting |
| `ATWM`, `ATSW` | make the ELM327 transmit wake-up frames by itself |
| `ATPP` | write the adapter's own EEPROM |

## VIN

- The firmware never **requests** a VIN (`09 02` and `22 F190` are blocked).
- Some ECUs store the VIN in other DIDs. On our car, ECU `718` returns it at
  `B019` (note 11). So every reply is **scrubbed before it is stored or
  shown**. Any run of 17 or more consecutive bytes from the VIN alphabet
  (`0-9`, `A-Z` except `I`, `O`, `Q`) is replaced with `*`.
- Limits: a partial VIN (for example only the last 6 characters) is not
  recognised. A long run of ID-like ASCII that is not a VIN is masked too.
  Masking a numeric field would also corrupt its value, but none of the
  report's DIDs are long enough for that to happen.

## How it is checked

- A boot-time self-test (`safety selftest: N/N PASS`) runs allowed and blocked
  commands, plus VIN-scrub cases built from a **synthetic** VIN, never a real
  one. This is a regression guard, not proof about any car.
- Any new command a future mode needs (discovery, profiles) must be added here
  **first**, with the reason, and then to `safety.c`.
