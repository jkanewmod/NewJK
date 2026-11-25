// cg_capture.h -- the capture service: target, frame grab, clock, audio. Shared verbatim with the game module.

#pragma once

// CG_CAPTURE_GRABFRAME / CG_CAPTURE_POLLFRAME pixel formats
#define CAPTURE_PIXFMT_ANY			0
#define CAPTURE_PIXFMT_RGBA32		1
#define CAPTURE_PIXFMT_BGRA32		2
#define CAPTURE_PIXFMT_RGB24		3

// CG_CAPTURE_GETCAPS
typedef struct captureCaps_s {
	int		supported;			// the renderer has a capture target
	int		vulkanAvailable;
	int		maxWidth, maxHeight;	// render size limits
	int		windowWidth, windowHeight;
	int		deviceRate;			// sound device rate, 0 = no sound
	int		softwareMixer;
	int		demoPlaying;
	int		hiddenWindow;
	char	renderer[64];
} captureCaps_t;

// CG_CAPTURE_SETTARGET: output size; render size = width*ssaa x height*ssaa; ssaa 1 or 2
typedef struct captureTarget_s {
	int		width, height;
	int		ssaa;
} captureTarget_t;

// CG_CAPTURE_SETCLOCK
#define CAPTURECLOCK_PACED			0
#define CAPTURECLOCK_UNTHROTTLED	1

typedef struct captureClock_s {
	int		fpsNum, fpsDen;		// demo time per frame = 1000*fpsDen/fpsNum ms
	int		throttle;			// CAPTURECLOCK_*
	int		presentInterval;	// present every Nth frame, <= 1 = every frame
	int		uiOnly;				// clear the target after each grab
	int		muteSpeakers;
	int		ignoreUnfocused;
	int		notifyLines;		// the console notify lines go into each grabbed frame
} captureClock_t;

// CG_CAPTURE_POLLFRAME
typedef struct captureFrameInfo_s {
	int		width, height;
	int		pixelFormat;
	int		frameNum;
} captureFrameInfo_t;

#define SND_NOCAPTURE				1	// CG_S_STARTLOCALSOUND_EX flags

/*
	CG_CAPTURE_AUDIOCONTROL ops. START: the mirror starts empty, value = the index of the next capture frame
	mixed; returns the device rate, 0 = no capture audio. CLEAR: every capture channel stops, value = the
	next frame's index (a mirror a sound restart stopped starts again). SEED: the speakers' playing sounds are copied in; returns how many. CHANNELS:
	returns how many one-shot sounds the capture channels play (loops are taken anew every frame). STOP returns 1.
	SPEAKERMUTE: value 1 silences the speakers with or without the clock (what was mixed ahead is dropped), 0 ends
	that; every demo load, disconnect and vid_restart ends it. Returns 1.
*/
#define CAPTUREAUDIO_START			1
#define CAPTUREAUDIO_STOP			2
#define CAPTUREAUDIO_CLEAR			3
#define CAPTUREAUDIO_SEED			4
#define CAPTUREAUDIO_CHANNELS		5
#define CAPTUREAUDIO_SPEAKERMUTE	6

#define CAPTUREMIX_SILENT			1	// CG_CAPTURE_MIXAUDIO flags
