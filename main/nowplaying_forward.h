#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * Now-playing forwarder: sends playback state, track info and album art to an
 * HTTP receiver (e.g. Signinator: POST <url>/api/nowplaying and
 * <url>/api/nowplaying/art). The URL is set on the device's web page and kept
 * in NVS (settings_get_nowplaying_url); empty = off.
 *
 * All HTTP work happens on the forwarder's own task, never on the RTSP task.
 */

/** Start the forwarder task and register for RTSP events. Safe to call once. */
void nowplaying_forward_init(void);

/**
 * Hand over a cover-art image received from the AirPlay sender. The bytes are
 * copied (PSRAM when available); larger than CONFIG_NOWPLAYING_ART_MAX_KB is
 * dropped. Does nothing when no URL is set.
 */
void nowplaying_forward_art(const char *content_type, const uint8_t *data,
                            size_t len);
