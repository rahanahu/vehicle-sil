/* SPDX-License-Identifier: MIT */
/*
 * Example of an existing ECU model delivered as a shared library (DLL / .so) with a C
 * interface and a fixed-period tick(). It knows nothing about SIL Kit or vehicle-sil: the host
 * (vehicle_sil_vt_ecu) feeds inputs, calls legacy_ecu_tick() every 10 ms of simulation time and
 * forwards the outputs.
 *
 * Behaviour: every 10th tick (100 ms) it produces a J1939 CCVS (PGN 0xFEF1) payload with the
 * last wheel-based vehicle speed (SPN 84, 1/256 km/h per bit, bytes 2-3, little endian).
 */
#ifndef VEHICLE_SIL_LEGACY_ECU_H
#define VEHICLE_SIL_LEGACY_ECU_H

#include <stdint.h>

#if defined(_WIN32)
#if defined(VEHICLE_SIL_LEGACY_ECU_BUILD)
#define LEGACY_ECU_API __declspec(dllexport)
#else
#define LEGACY_ECU_API __declspec(dllimport)
#endif
#else
#define LEGACY_ECU_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed tick period the ECU was designed for. */
#define LEGACY_ECU_TICK_PERIOD_MS 10

LEGACY_ECU_API void legacy_ecu_init(void);

/* Input: wheel-based vehicle speed in mm/s. */
LEGACY_ECU_API void legacy_ecu_set_wheel_speed(uint32_t speed_mm_s);

/* One fixed-period cycle. */
LEGACY_ECU_API void legacy_ecu_tick(void);

/* Output: 1 and the 8-byte CCVS payload if this tick produced one, else 0. */
LEGACY_ECU_API int legacy_ecu_take_ccvs(uint8_t data[8]);

LEGACY_ECU_API uint32_t legacy_ecu_tick_count(void);

#ifdef __cplusplus
}
#endif

#endif /* VEHICLE_SIL_LEGACY_ECU_H */
