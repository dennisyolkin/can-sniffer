# UDS services that change the ECU (not implemented)

Reference for later. None of these is in `uds_client`'s allowlist; adding one is a deliberate change, made only when
the user asks for that specific action (CLAUDE.md, "Hard rules"). Sources: PSA-RE `diag/protocols/UDS.yml` and
`diag/AEE2010.ev/TBMU/BMU_CTE1/ecu.yml` (the BMS lists services `10 11 14 19 22 27 2E 31`), ISO 14229-1.

What is implemented and read-only: `3E` TesterPresent, `22` ReadDataByIdentifier, `19` ReadDTCInformation
(`01`/`02`/`04`/`06`), and `10` DiagnosticSessionControl limited to `01` default / `03` extended (red buttons).

## Clear fault codes: `14` ClearDiagnosticInformation

| | |
|---|---|
| Request | `04 14 FF FF FF` (group `FFFFFF` = all faults) |
| Reply | `01 54`, or `03 7F 14 <NRC>` |
| Session | Often accepted in the default session; some ECUs want extended (`10 03`) first |
| Security | Usually not needed |

The most likely one to be wanted (after a repair, or to see whether a fault comes back). Costs:
- The fault history goes, with its snapshot and extended data (occurrence counters, conditions). A dealer or a
  warranty claim may need it. Read and save everything first: `19 02 2D`, then `19 04` / `19 06` per fault.
- A fault that is still present comes back as soon as the ECU tests it again; clearing does not repair anything.
- On some ECUs it also resets learned values or readiness flags.

To add it: an allowlist case for `0x14` with exactly `FF FF FF`, a red button, and a confirmation on the page.

## Write a DID: `2E` WriteDataByIdentifier

| | |
|---|---|
| Request | `2E <DID hi> <DID lo> <data…>` |
| Reply | `03 6E <DID hi> <DID lo>` |
| Session | Extended (`10 03`) |
| Security | Yes: `27 03` / `27 04` first (configuration level, PSA_SEED_KEY) |

The BMS's writable DIDs are its configuration (PSA-RE `access: read-write`): charge window `D862` / `D863`, SoC
sensor calibration `D410`, the unnamed `D416`-`D423`, `D900`, `E082`, `D86A`, `D901`, `D465`. Wrong values in a
traction battery controller can damage the battery, disable charging, or void the warranty. Not worth it for
curiosity; if ever, read and save the original value first.

## Unlock: `27` SecurityAccess

| | |
|---|---|
| Request seed | `02 27 03` → `06 67 03 <4-byte seed>` |
| Send key | `06 27 04 <4-byte key>` → `02 67 04` |
| Session | Extended |

The key is computed from the seed with PSA's algorithm (PSA-RE names it PSA_SEED_KEY; the per-ECU key is not in
PSA-RE). Wrong keys cost an attempt; after a few, the ECU refuses for a while (NRC `36` exceeded attempts, `37`
time delay not expired). Only a prerequisite for `2E`/`31`/programming; no use on its own.

## Routines: `31` RoutineControl

| | |
|---|---|
| Request | `31 <01 start / 02 stop / 03 results> <RID hi> <RID lo> [options]` |
| Reply | `71 <type> <RID hi> <RID lo> [status]` |

PSA-RE lists three routines for this BMS with no names (`routines/01_LID.yml` … `03_LID.yml`). PSA uses routine
`FF00` to erase flash before a software download: running an unknown routine could brick the ECU or open the
contactors. Do not try unknown routines on the car.

## Reset: `11` ECUReset

| | |
|---|---|
| Request | `02 11 <01 hard / 02 key off-on / 03 soft>` (the BMS lists `02`) |
| Reply | `02 51 <type>` |

Restarting the BMS opens the high-voltage contactors: the car loses propulsion if it is driving and may log faults
in other ECUs. Only parked and off, and only with a reason.

## Programming session: `10 02`

The start of a software download (`10 02`, `27 01`/`02`, `31 01 FF 00`, `34`/`36`/`37`). Never; a failed download
can leave the BMS unusable. `uds_client` refuses `10 02` explicitly.
