/*
 * GoldHEN v2.4b18.9 - BD-App AutoKill + Disc AutoEject Header
 */

#ifndef GOLDHEN_BD_PATCH_H
#define GOLDHEN_BD_PATCH_H

/* Start the BD/disc monitor background thread. */
void bd_patch_start(void);

/* Stop the monitor thread. */
void bd_patch_stop(void);

#endif /* GOLDHEN_BD_PATCH_H */
