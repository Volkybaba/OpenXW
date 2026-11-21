/* VolkyXWCD v3: Fn (guide) screenshot for frontend, film and flight frames.
 *
 * Capture sources (all CPU-side, i.e. exactly the classic pixels the game
 * composes — no GPU readback, the swapchain has no intermediate texture):
 * - Frontend VGA scenes (intro/cutscene films, 320x200): the committed
 *   Landru presented frame via xsurface_Get_Presented_Frame.
 * - Frontend SVGA scenes (menus, rooms, briefings, 640x480): the Landru
 *   draw canvas via xcanvas_Get_Draw_Buffer (the same buffer the Dx5 path
 *   blits to the back buffer).
 * - Flight (DOS or Windows): the existing FlightScreenshot_Capture, which
 *   reads the classic flight surface / DOS published frame. The classic
 *   surface keeps being drawn even while the modern view is opaque
 *   (suppression only skips its presentation), so it is always current.
 * Output files go through File_RawOpen, which resolves bare names to the
 * user root (conf/TotallyOpen/OpenXW/).
 */
#include "xw/render/screenshot.h"

#include "xw/assets/file.h"
#include "xw/frontend/shell.h"
#include "xw/frontend/shellext.h"
#include "xw/render/flight_screenshot.h"
#include "xw/util/front_image.h"
#include "xw_runtime/runtime/presentation.h"
#include <aeron/aeron.h>
#include <aeron/log.h>
#include <aeron/scene/present.h>
#include <landru/canvas.h>
#include <landru/presentation.h>
#include <landru/surface.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define XW_SCREENSHOT_NAME_CAPACITY 64
#define XW_SCREENSHOT_PALETTE_ENTRIES 256
#define XW_SCREENSHOT_FRONT_WIDTH 640
#define XW_SCREENSHOT_FRONT_HEIGHT 480

static uint32_t prev_guide;

static uint8_t Screenshot_ExpandVga(uint8_t value) { return (uint8_t)((value << 2) | (value >> 4)); }

static void Screenshot_BuildPalette(const uint8_t* rgb6, AeronRgbQuad* palette) {
	int i;
	/* VolkyXWCD v7: brightness — same half-gamma as the live display so
	 * the screenshot matches the screen (identity at 1.0). */
	for (i = 0; i < XW_SCREENSHOT_PALETTE_ENTRIES; ++i) {
		palette[i].rgbRed = AeronScenePresent_BrightenSrgb8(Screenshot_ExpandVga(rgb6[i * 3]));
		palette[i].rgbGreen = AeronScenePresent_BrightenSrgb8(Screenshot_ExpandVga(rgb6[i * 3 + 1]));
		palette[i].rgbBlue = AeronScenePresent_BrightenSrgb8(Screenshot_ExpandVga(rgb6[i * 3 + 2]));
		palette[i].rgbReserved = 0;
	}
}

/* First unused <base>N.bmp, mirroring FlightScreenshot_Capture. */
static void Screenshot_NextFreeName(const char* base, char* out, size_t capacity) {
	int index = 0;
	for (;;) {
		XwFile* existing;
		snprintf(out, capacity, "%s%d.bmp", base, index);
		existing = File_RawOpen(out, "rb");
		if (existing == NULL)
			return;
		File_RawClose(existing);
		++index;
	}
}

static bool Screenshot_IsFilmScene(int scene) {
	/* Intro block: startup logo, title crawls, opening, bridges, hangar,
	 * dogfights, logo, credits, formation, victory credits. */
	if ((scene >= XW_SCENE_STARTUP_LOGO && scene <= XW_SCENE_INTRO_CREDITS) ||
		scene == XW_SCENE_INTRO_FORMATION || scene == XW_SCENE_VICTORY_CREDITS)
		return true;
	/* Transit / arrival / departure cutscenes. */
	if ((scene >= XW_SCENE_TRAINING_DEPART_INDEPENDENCE &&
		 scene <= XW_SCENE_TRAINING_RETURN_SHUTTLE) ||
		(scene >= XW_SCENE_COMBAT_DEPART_INDEPENDENCE && scene <= XW_SCENE_COMBAT_RETURN_SHUTTLE) ||
		(scene >= XW_SCENE_TOUR_DEPART_INDEPENDENCE && scene <= XW_SCENE_CAPTURE_ARRIVE_EXECUTOR))
		return true;
	/* Death Star destruction sequences. */
	if ((scene >= XW_SCENE_DESTROY_STAR_DESTROYER_1 &&
		 scene <= XW_SCENE_DESTROY_STAR_DESTROYER_3) ||
		(scene >= XW_SCENE_DEATH_STAR_FIRE_1 && scene <= XW_SCENE_DEATH_STAR_DESTRUCTION))
		return true;
	return false;
}

static void Screenshot_CaptureFrontend(const char* base) {
	char fileName[XW_SCREENSHOT_NAME_CAPACITY];
	AeronRgbQuad palette[XW_SCREENSHOT_PALETTE_ENTRIES];
	LandruPresentedFrame frame;
	int saved;
	Screenshot_NextFreeName(base, fileName, sizeof fileName);
	if (xsurface_Get_Presented_Frame(&frame)) {
		Screenshot_BuildPalette(frame.palette, palette);
		saved = FrontImage_SaveBmpFile(fileName, frame.pixels, frame.width, frame.height, frame.stride,
									   8, 0, palette);
		if (saved)
			Aeron_LogInfo("xw.screenshot", "Saved %s", fileName);
		return;
	}
	if (!xsurface_Uses_Native_Vga_Presentation()) {
		const uint8_t* canvas = xcanvas_Get_Draw_Buffer();
		const uint8_t* effective = xsurface_Get_Effective_Palette();
		if (canvas != NULL && effective != NULL) {
			Screenshot_BuildPalette(effective, palette);
			saved = FrontImage_SaveBmpFile(fileName, canvas, XW_SCREENSHOT_FRONT_WIDTH,
										   XW_SCREENSHOT_FRONT_HEIGHT, XW_SCREENSHOT_FRONT_WIDTH, 8, 0,
										   palette);
			if (saved)
				Aeron_LogInfo("xw.screenshot", "Saved %s", fileName);
		}
	}
}

void XwScreenshot_Capture(void) {
	XwPresentationSource source = XwPresentation_BaseSource();
	if (source == XW_PRESENT_FRONTEND) {
		const char* base =
			Screenshot_IsFilmScene(shellext_Get_Cur_Scene()) ? "moviescreen" : "frontscreen";
		Screenshot_CaptureFrontend(base);
	} else {
		Aeron_LogInfo("xw.screenshot", "Capture requested (flight)");
		FlightScreenshot_Capture();
	}
}

/* VolkyXWColl v6 pattern: watch the guide button directly on the raw
 * snapshot; SampleDigital / config bindings are never involved, so the
 * settings menu capturing input cannot swallow the press. */
void XwScreenshot_Update(const AeronInputSnapshot* input) {
	uint32_t guide = 0;
	uint32_t rising;
	int i;
	if (input == NULL)
		return;
	for (i = 0; i < AERON_CONTROLLER_MAX; ++i) {
		const AeronControllerSnapshot* d = &input->controllers[i];
		if (d->connected && (d->gamepad_buttons & (1u << AERON_GAMEPAD_BUTTON_GUIDE)))
			guide |= 1u << i;
	}
	rising = guide & ~prev_guide;
	prev_guide = guide;
	if (rising)
		XwScreenshot_Capture();
}
