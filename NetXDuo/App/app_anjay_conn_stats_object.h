/*
 * LwM2M Connectivity Statistics Object (/7) implementation for Anjay
 *
 * Resources:
 *   0  TX Data (bytes)
 *   1  RX Data (bytes)
 *   2  TX Packets
 *   3  RX Packets
 *   4  Signal Strength (dBm)
 *   5  Signal Quality
 *  10  Period (seconds)
 *  11  Statistics Collection Start (execute)
 */

#ifndef APP_ANJAY_CONN_STATS_OBJECT_H
#define APP_ANJAY_CONN_STATS_OBJECT_H

#include <anjay/anjay.h>

/**
 * @brief Install Connectivity Statistics Object (/7) into Anjay.
 * @return 0 on success, negative on error.
 */
int conn_stats_object_install(anjay_t *anjay);

#endif /* APP_ANJAY_CONN_STATS_OBJECT_H */
