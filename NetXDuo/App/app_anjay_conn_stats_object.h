/*
 * LwM2M Connectivity Statistics Object (/7) implementation for Anjay
 *
 * Resources (per OMA LwM2M TS):
 *   0  Tx Data (KB)
 *   1  Rx Data (KB)
 *   2  Tx Packets
 *   3  Rx Packets
 *   4  Max Message Size (bytes, approximated)
 *   5  Average Message Size (bytes)
 *   6  Start collection (execute)
 *   7  Stop collection (execute)
 *   8  Collection Period (seconds)
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
