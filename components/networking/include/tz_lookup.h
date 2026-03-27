#pragma once

/**
 * @file tz_lookup.h
 * @brief IANA timezone name → POSIX TZ string lookup
 *
 * Usage:
 *   const char* posix = tz_lookup("America/Chicago");
 *   // returns "CST6CDT,M3.2.0,M11.1.0"
 *
 * Returns nullptr if the IANA name is not found in the table.
 * Covers all major world timezones (~400 entries).
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Look up the POSIX TZ string for an IANA timezone name.
 * @param iana_name  e.g. "America/Chicago", "Europe/London"
 * @return POSIX TZ string, or nullptr if not found.
 */
const char* tz_lookup(const char* iana_name);

#ifdef __cplusplus
}
#endif
