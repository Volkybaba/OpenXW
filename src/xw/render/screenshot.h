#ifndef XW_RENDER_SCREENSHOT_H
#define XW_RENDER_SCREENSHOT_H
#include <aeron/input.h>
#ifdef __cplusplus
extern "C" {
#endif

/* VolkyXWCD v3: Fn (guide) screenshot. The gamepad guide button is watched
 * directly on the raw input snapshot (no config binding, no action mapping),
 * so it works on every screen, including while the settings menu is open.
 * Call once per host frame, after the frame is composed and before present. */
void XwScreenshot_Update(const AeronInputSnapshot* input);
/* Capture the current frame now: frontend -> frontscreenN.bmp (menus, rooms,
 * briefings) or moviescreenN.bmp (intro/cutscene/transit films), flight ->
 * flightscreenN.bmp. Files land in the user root (conf/TotallyOpen/OpenXW/). */
void XwScreenshot_Capture(void);
#ifdef __cplusplus
}
#endif
#endif
