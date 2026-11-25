// cl_capture.cpp -- capture service: target, frame grab, clock (cg_capture.h)

#include "client.h"
#include "snd_local.h"

captureState_t cl_capture;

extern cvar_t *cl_renderer;

static int captureGrabsOutstanding;	// accepted by the renderer, not yet polled
static int captureClockMsecTotal;	// integer msec handed out since SETCLOCK

qboolean CL_CaptureActive( void ) {
	return (qboolean)( cl_capture.targetActive || cl_capture.clockActive );
}

// the notify lines are not drawn over the ui: an unattended capture's window, or already in this frame's grab
qboolean CL_CaptureOwnsNotify( void ) {
	return (qboolean)( cl_capture.clockActive && ( cl_capture.clock.uiOnly || cl_capture.notifyFrame == cls.framecount + 1 ) );
}

qboolean CL_CaptureGetCaps( captureCaps_t *out ) {
	if ( !out ) {
		return qfalse;
	}

	Com_Memset( out, 0, sizeof( *out ) );
	out->supported = ( re && re->SetCaptureTarget ) ? 1 : 0;
	out->vulkanAvailable = CL_VulkanSupported() ? 1 : 0;
	if ( re && re->GetCaptureLimits ) {
		re->GetCaptureLimits( &out->maxWidth, &out->maxHeight );
	}
	out->windowWidth = CL_RealVidWidth();
	out->windowHeight = CL_RealVidHeight();
	out->deviceRate = S_GetDeviceRate();
	out->softwareMixer = 1;
#ifdef USE_OPENAL
	{
		extern int s_UseOpenAL;
		if ( s_UseOpenAL ) {
			out->softwareMixer = 0;
		}
	}
#endif
	out->demoPlaying = clc.demoplaying ? 1 : 0;
	out->hiddenWindow = cl_hiddenWindow->integer;
	Q_strncpyz( out->renderer, CL_RendererLibrary(), sizeof( out->renderer ) );
	return qtrue;
}

static void CL_CaptureReleaseTarget( void ) {
	// also when the client thinks none is active: a refused change can leave the renderer on the old one
	if ( re && re->SetCaptureTarget ) {
		re->SetCaptureTarget( 0, 0, 0 );
	}
	cl_capture.targetActive = qfalse;
	Com_Memset( &cl_capture.target, 0, sizeof( cl_capture.target ) );
	cl_capture.grabPending = qfalse;
	captureGrabsOutstanding = 0;
}

qboolean CL_CaptureSetTarget( const captureTarget_t *t ) {
	if ( !t ) {
		CL_CaptureReleaseTarget();
		return qtrue;
	}
	if ( !clc.demoplaying || !re || !re->SetCaptureTarget ) {
		return qfalse;
	}
	if ( t->width <= 0 || t->height <= 0 || ( t->ssaa != 1 && t->ssaa != 2 ) ) {
		CL_CaptureReleaseTarget();	// a refusal always means the window target
		return qfalse;
	}
	// unchanged: keeps the grabs in flight
	if ( cl_capture.targetActive && cl_capture.target.width == t->width && cl_capture.target.height == t->height
		&& cl_capture.target.ssaa == t->ssaa ) {
		return qtrue;
	}

	cl_capture.grabPending = qfalse;
	captureGrabsOutstanding = 0;
	if ( !re->SetCaptureTarget( t->width * t->ssaa, t->height * t->ssaa, t->ssaa ) ) {
		CL_CaptureReleaseTarget();	// a refusal always means the window target
		return qfalse;
	}
	cl_capture.targetActive = qtrue;
	cl_capture.target = *t;
	return qtrue;
}

static void CL_CaptureEndClock( void ) {
	if ( cl_capture.clockActive ) {
		Com_DPrintf( "capture clock: %d frames, %d ms at %d/%d fps\n", cl_capture.frameCounter, captureClockMsecTotal,
			cl_capture.clock.fpsNum, cl_capture.clock.fpsDen );
	}
	captureClockMsecTotal = 0;
}

qboolean CL_CaptureSetClock( const captureClock_t *c ) {
	if ( !c ) {
		CL_CaptureEndClock();
		cl_capture.clockActive = qfalse;
		Com_Memset( &cl_capture.clock, 0, sizeof( cl_capture.clock ) );
		cl_capture.remainder = 0.0f;
		cl_capture.frameCounter = 0;
		com_captureMinMsec = -1;
		return qtrue;
	}
	if ( !clc.demoplaying || CL_VideoRecording() ) {
		return qfalse;
	}
	if ( c->fpsNum <= 0 || c->fpsDen <= 0 || 1000.0 * c->fpsDen / c->fpsNum < 1.0 ) {
		return qfalse;
	}

	CL_CaptureEndClock();
	cl_capture.clockActive = qtrue;
	cl_capture.clock = *c;
	cl_capture.remainder = 0.0f;
	cl_capture.frameCounter = 0;
	com_captureMinMsec = ( c->throttle == CAPTURECLOCK_UNTHROTTLED ) ? 0 : 1000 * c->fpsDen / c->fpsNum;
	return qtrue;
}

// this frame's msec under the capture clock; the fraction is carried to the next frame
int CL_CaptureFrameMsec( float *exactMsec ) {
	const double	exact = 1000.0 * cl_capture.clock.fpsDen / cl_capture.clock.fpsNum;
	const double	total = exact + cl_capture.remainder;
	const int		msec = (int)total;

	cl_capture.remainder = (float)( total - msec );
	cl_capture.frameCounter++;
	captureClockMsecTotal += msec;
	// paced: the next frame waits its own carried msec, so the wall clock keeps the exact rate
	if ( cl_capture.clock.throttle != CAPTURECLOCK_UNTHROTTLED ) {
		com_captureMinMsec = (int)( exact + cl_capture.remainder );
	}
	if ( exactMsec ) {
		*exactMsec = (float)exact;
	}
	return msec;
}

qboolean CL_CaptureMuteSpeakers( void ) {
	return (qboolean)( cl_capture.speakerMute || ( cl_capture.clockActive && cl_capture.clock.muteSpeakers ) );
}

// under the capture clock, every presentInterval-th frame reaches the window
qboolean CL_CapturePresentThisFrame( void ) {
	if ( !cl_capture.clockActive || cl_capture.clock.presentInterval <= 1 ) {
		return qtrue;
	}
	return (qboolean)( ( cl_capture.frameCounter % cl_capture.clock.presentInterval ) == 0 );
}

qboolean CL_CaptureGrabFrame( byte *buffer, int bufSize, int pixelFormat ) {
	extern int cl_demoFrameNum;

	if ( !cl_capture.targetActive || !re || !re->CaptureFrame || !buffer || bufSize <= 0 ) {
		return qfalse;
	}
	if ( pixelFormat < CAPTURE_PIXFMT_ANY || pixelFormat > CAPTURE_PIXFMT_RGB24 ) {
		return qfalse;
	}
	if ( cl_capture.clockActive && cl_capture.clock.notifyLines && cls.state == CA_ACTIVE ) {
		Con_DrawNotify();
		cl_capture.notifyFrame = cls.framecount + 1;
	}

	// the frame number rides along as the grab's tag, so a poll reports which frame it returns
	// uiOnly: after the grab the window shows ui and console on black
	if ( !re->CaptureFrame( buffer, bufSize, pixelFormat, (qboolean)( cl_capture.clockActive && cl_capture.clock.uiOnly ), cl_demoFrameNum ) ) {
		return qfalse;
	}
	cl_capture.grabPending = qtrue;
	cl_capture.grabFrameNum = cl_demoFrameNum;
	cl_capture.grabFormat = pixelFormat;
	captureGrabsOutstanding++;
	return qtrue;
}

// the oldest grab; a grab whose frame never reached the GPU comes back with width 0
qboolean CL_CapturePollFrame( captureFrameInfo_t *out, qboolean wait ) {
	int width = 0, height = 0, pixelFormat = 0, tag = -1;

	if ( !out || !re || !re->CapturePoll ) {
		return qfalse;
	}
	if ( !re->CapturePoll( &width, &height, &pixelFormat, &tag, wait ) ) {
		return qfalse;
	}

	out->width = width;
	out->height = height;
	out->pixelFormat = pixelFormat;
	out->frameNum = tag;
	if ( captureGrabsOutstanding > 0 ) {
		captureGrabsOutstanding--;
	}
	cl_capture.grabPending = (qboolean)( captureGrabsOutstanding > 0 );
	return qtrue;
}

/*
	Capture frame N gets floor((N+1)*rate*den/num) - floor(N*rate*den/num) samples at the device rate, N
	counted from the value START or CLEAR set. Refused (nothing painted, N unchanged) without the capture
	clock or when the frame would not fit.
*/
qboolean CL_CaptureMixAudio( short *pcm, int maxFrames, int *outFrames, int flags ) {
	int n;

	if ( outFrames ) {
		*outFrames = 0;
	}
	if ( !cl_capture.audioActive || !cl_capture.clockActive || !pcm || maxFrames <= 0 ) {
		return qfalse;
	}
	n = S_CaptureMix( pcm, maxFrames, cl_capture.audioFrameCounter, cl_capture.clock.fpsNum, cl_capture.clock.fpsDen,
		(qboolean)( ( flags & CAPTUREMIX_SILENT ) != 0 ) );
	if ( n < 0 ) {
		return qfalse;
	}
	cl_capture.audioFrameCounter++;
	if ( outFrames ) {
		*outFrames = n;
	}
	return qtrue;
}

int CL_CaptureAudioControl( int op, int value ) {
	switch ( op ) {
	case CAPTUREAUDIO_START:
		if ( !S_CaptureStart() ) {
			cl_capture.audioActive = qfalse;
			return 0;
		}
		cl_capture.audioActive = qtrue;
		cl_capture.audioFrameCounter = value;
		return S_GetDeviceRate();
	case CAPTUREAUDIO_STOP:
		S_CaptureStop();
		cl_capture.audioActive = qfalse;
		cl_capture.audioFrameCounter = 0;
		return 1;
	case CAPTUREAUDIO_CLEAR:
		if ( !cl_capture.audioActive ) {
			return 0;
		}
		// a sound restart stopped the mirror under us
		if ( !s_captureActive && !S_CaptureStart() ) {
			return 0;
		}
		S_CaptureClear();
		cl_capture.audioFrameCounter = value;
		return 1;
	case CAPTUREAUDIO_SEED:
		return cl_capture.audioActive ? S_CaptureSeedFromSpeakers() : 0;
	case CAPTUREAUDIO_CHANNELS:
		return cl_capture.audioActive ? S_CaptureChannelCount() : 0;
	case CAPTUREAUDIO_SPEAKERMUTE:
		if ( value && !cl_capture.speakerMute ) {
			S_ClearSoundBuffer();
		}
		cl_capture.speakerMute = (qboolean)( value != 0 );
		return 1;
	default:
		return 0;
	}
}

void CL_CaptureReset( void ) {
	CL_CaptureReleaseTarget();
	CL_CaptureSetClock( NULL );
	CL_CaptureAudioControl( CAPTUREAUDIO_STOP, 0 );
	cl_capture.speakerMute = qfalse;
}
