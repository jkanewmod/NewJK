// snd_capture.cpp -- the capture mixer: a mirror of the channel table, painted on the capture clock

#include "client.h"
#include "snd_local.h"

channel_t	s_captureChannels[MAX_CHANNELS];
int			s_capturePaintedtime;
qboolean	s_captureActive;
int			s_startFlags;

// the painted time at START: seeded sounds may have started up to this many samples earlier
#define CAPTURE_TIME_BASE	( 1 << 24 )

static portable_samplepair_t s_capturePaintbuffer[PAINTBUFFER_SIZE];

extern int s_soundtime;

qboolean S_CaptureStart( void ) {
	if ( !S_GetDeviceRate() ) {
		return qfalse;
	}
#ifdef USE_OPENAL
	{
		extern int s_UseOpenAL;
		if ( s_UseOpenAL ) {
			return qfalse;
		}
	}
#endif
	memset( s_captureChannels, 0, sizeof( s_captureChannels ) );
	s_capturePaintedtime = CAPTURE_TIME_BASE;
	s_captureActive = qtrue;
	s_startFlags = 0;		// an error inside a flagged start never reset it
	return qtrue;
}

void S_CaptureStop( void ) {
	if ( s_captureActive ) {
		memset( s_captureChannels, 0, sizeof( s_captureChannels ) );
	}
	s_captureActive = qfalse;
}

void S_CaptureClear( void ) {
	if ( !s_captureActive ) {
		return;
	}
	for ( int i = 0; i < MAX_CHANNELS; i++ ) {
		Channel_Clear( &s_captureChannels[i] );
	}
}

int S_CaptureChannelCount( void ) {
	int n = 0;

	if ( s_captureActive ) {
		for ( int i = 0; i < MAX_CHANNELS; i++ ) {
			n += ( s_captureChannels[i].thesfx && !s_captureChannels[i].loopSound ) ? 1 : 0;
		}
	}
	return n;
}

// a fresh decoder from the sound's own header: one channel's stream state is never shared
static void S_CaptureCopyStart( channel_t *ch, const channel_t *src ) {
	VectorCopy( src->origin, ch->origin );
	ch->fixed_origin = src->fixed_origin;
	ch->master_vol = src->master_vol;
	ch->entnum = src->entnum;
	ch->entchannel = src->entchannel;
	ch->thesfx = src->thesfx;
	ch->leftvol = src->leftvol;
	ch->rightvol = src->rightvol;
	ch->startSample = START_SAMPLE_IMMEDIATE;
	if ( src->thesfx->pMP3StreamHeader ) {
		memcpy( &ch->MP3StreamHeader, src->thesfx->pMP3StreamHeader, sizeof( ch->MP3StreamHeader ) );
	}
}

void S_CaptureOnChannelStart( const channel_t *speakerCh, int flags ) {
	channel_t *ch;

	if ( !s_captureActive || ( flags & SND_NOCAPTURE ) || !speakerCh || !speakerCh->thesfx ) {
		return;
	}
	ch = S_PickChannelIn( s_captureChannels, speakerCh->entnum, speakerCh->entchannel );
	if ( ch ) {
		S_CaptureCopyStart( ch, speakerCh );
	}
}

// the speaker channels that are playing, at the position they have reached
int S_CaptureSeedFromSpeakers( void ) {
	int n = 0;

	if ( !s_captureActive ) {
		return 0;
	}
	S_CaptureClear();
	for ( int i = 0; i < MAX_CHANNELS; i++ ) {
		const channel_t	*src = &s_channels[i];
		channel_t		*ch = &s_captureChannels[i];

		if ( !src->thesfx || src->loopSound || !src->thesfx->bInMemory ) {
			continue;
		}
		S_CaptureCopyStart( ch, src );
		if ( src->startSample != START_SAMPLE_IMMEDIATE ) {
			const int elapsed = s_soundtime - (int)src->startSample;

			if ( elapsed >= src->thesfx->iSoundLengthInSamples ) {
				Channel_Clear( ch );
				continue;
			}
			if ( elapsed > 0 && elapsed < CAPTURE_TIME_BASE ) {
				ch->startSample = (unsigned int)( s_capturePaintedtime - elapsed );
			}
		}
		n++;
	}
	return n;
}

// this frame's loops replace the last frame's; one-shots follow the listener but are never freed for silence
void S_CaptureRespatialize( void ) {
	int i;

	if ( !s_captureActive ) {
		return;
	}
	for ( i = 0; i < MAX_CHANNELS; i++ ) {
		channel_t *ch = &s_captureChannels[i];

		if ( !ch->thesfx ) {
			continue;
		}
		if ( ch->loopSound ) {
			Channel_Clear( ch );
			continue;
		}
		S_SpatializeChannel( ch );
	}
	for ( i = 0; i < MAX_CHANNELS; i++ ) {
		const channel_t	*src = &s_channels[i];
		channel_t		*ch;

		if ( !src->thesfx || !src->loopSound ) {
			continue;
		}
		ch = S_PickChannelIn( s_captureChannels, 0, 0 );
		if ( !ch ) {
			break;
		}
		ch->leftvol = src->leftvol;
		ch->rightvol = src->rightvol;
		ch->loopSound = qtrue;
		ch->thesfx = src->thesfx;
		ch->doppler = src->doppler;
		ch->dopplerScale = src->dopplerScale;
		ch->entchannel = src->entchannel;
	}
}

qboolean S_CaptureUsesSfx( const sfx_t *sfx ) {
	if ( s_captureActive ) {
		for ( int i = 0; i < MAX_CHANNELS; i++ ) {
			if ( s_captureChannels[i].thesfx == sfx ) {
				return qtrue;
			}
		}
	}
	return qfalse;
}

void S_CaptureStopSfx( const sfx_t *sfx ) {
	if ( s_captureActive ) {
		for ( int i = 0; i < MAX_CHANNELS; i++ ) {
			if ( s_captureChannels[i].thesfx == sfx ) {
				Channel_Clear( &s_captureChannels[i] );
			}
		}
	}
}

// floor( n * rate * fpsDen / fpsNum ): samples before capture frame n, n may be negative
static long long S_CaptureSamplesBefore( long long n, long long rate, long long fpsNum, long long fpsDen ) {
	const long long num = n * rate * fpsDen;

	return num >= 0 ? num / fpsNum : -( ( -num + fpsNum - 1 ) / fpsNum );
}

/*
	The samples of capture frame frameIndex at the device rate, interleaved stereo s16: painted from the
	capture table only (no music, no lip sync, nothing of the speaker clock or the DMA buffer). silent:
	zeros, but the channels play on. Returns the sample count, or -1 when it would not fit (nothing painted).
*/
int S_CaptureMix( short *pcm, int maxFrames, int frameIndex, int fpsNum, int fpsDen, qboolean silent ) {
	const int	rate = S_GetDeviceRate();
	int			n, done = 0;

	if ( !s_captureActive || !rate || fpsNum <= 0 || fpsDen <= 0 ) {
		return -1;
	}
	n = (int)( S_CaptureSamplesBefore( (long long)frameIndex + 1, rate, fpsNum, fpsDen )
		- S_CaptureSamplesBefore( frameIndex, rate, fpsNum, fpsDen ) );
	if ( n > maxFrames || n < 0 ) {
		return -1;
	}

	S_ScanChannelStartsIn( s_captureChannels, s_capturePaintedtime );
	while ( done < n ) {
		const int chunk = ( n - done > PAINTBUFFER_SIZE ) ? PAINTBUFFER_SIZE : n - done;
		short *out = pcm + done * 2;

		S_PaintChannelsCore( s_captureChannels, &s_capturePaintedtime, s_capturePaintedtime + chunk, s_capturePaintbuffer, qfalse, qfalse );
		for ( int i = 0; i < chunk; i++ ) {
			int l = s_capturePaintbuffer[i].left >> 8, r = s_capturePaintbuffer[i].right >> 8;

			out[i * 2] = (short)( l > 0x7fff ? 0x7fff : l < -0x8000 ? -0x8000 : l );
			out[i * 2 + 1] = (short)( r > 0x7fff ? 0x7fff : r < -0x8000 ? -0x8000 : r );
		}
		done += chunk;
	}
	if ( silent ) {
		memset( pcm, 0, n * 2 * sizeof( short ) );
	}
	return n;
}
