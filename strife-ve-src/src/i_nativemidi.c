//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
// Copyright(C) 2014 Night Dive Studios, Inc.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//   Native MIDI output module.  Routes game music directly to a hardware
//   MIDI port selected by the "midi_out_device" config variable so that
//   external sound modules (e.g. Roland SD-50, Focusrite + GM device) can
//   render the music instead of a software synthesiser.
//
//   Platform backends:
//     Windows  – Win32 Multimedia (winmm): midiOutOpen / midiOutShortMsg
//     Linux    – ALSA raw-MIDI  (/dev/snd/midiC*D*)
//     macOS    – Core MIDI      (MIDIOutputPortCreate / MIDISend)
//
//   All three backends share the same SDL-thread playback loop and the same
//   music_module_t interface so the rest of the engine is unaware of which
//   backend is active.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "SDL.h"
#include "SDL_thread.h"

#include "config.h"
#include "doomtype.h"
#include "i_sound.h"
#include "i_system.h"
#include "m_misc.h"
#include "memio.h"
#include "midifile.h"
#include "mus2mid.h"
#include "z_zone.h"

// ---------------------------------------------------------------------------
// Platform detection
// ---------------------------------------------------------------------------

#if defined(_WIN32)
#  define NM_BACKEND_WIN32
#elif defined(__APPLE__)
#  define NM_BACKEND_COREMIDI
#elif defined(__linux__)
#  define NM_BACKEND_ALSA
#endif

// ---------------------------------------------------------------------------
// Platform-specific includes / types
// ---------------------------------------------------------------------------

#if defined(NM_BACKEND_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <mmsystem.h>
#  ifdef _MSC_VER
#    pragma comment(lib, "winmm.lib")
#  endif

typedef struct {
    HMIDIOUT handle;
    int      num_devices;
} nm_platform_t;

#elif defined(NM_BACKEND_COREMIDI)
#  include <CoreMIDI/CoreMIDI.h>
#  include <CoreFoundation/CoreFoundation.h>

typedef struct {
    MIDIClientRef    client;
    MIDIPortRef      out_port;
    MIDIEndpointRef  endpoint;
    int              num_devices;
} nm_platform_t;

#elif defined(NM_BACKEND_ALSA)
#  include <fcntl.h>
#  include <unistd.h>
#  include <glob.h>

typedef struct {
    int fd;           // raw MIDI file descriptor
    int num_devices;
} nm_platform_t;

#else
// Stub for unsupported platforms – Init() will fail gracefully.
typedef struct { int num_devices; } nm_platform_t;
#endif

// ---------------------------------------------------------------------------
// Internal types
// ---------------------------------------------------------------------------

// One parsed MIDI event kept in a flat array for the playback thread
typedef struct
{
    unsigned int delta_time; // ticks since previous event
    midi_event_t event;
} nm_event_t;

typedef struct
{
    nm_event_t  *events;
    int          num_events;
    unsigned int time_division;   // ticks per quarter-note (from MIDI header)
} nm_song_t;

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------

int midi_out_device = 0; // config variable: hardware port index (0 = default)

static nm_platform_t plat;
static boolean       midi_initialized = false;

// Current volume (0-127), applied as CC7 on all channels
static int current_volume = 100;

// Playback thread state
static SDL_Thread  *player_thread    = NULL;
static SDL_mutex   *player_mutex     = NULL;
static nm_song_t   *current_song     = NULL;
static boolean      song_playing     = false;
static boolean      song_looping     = false;
static boolean      song_paused      = false;
static boolean      thread_running   = false;
// True (mutex-protected) only while the player thread is actually inside
// the event-dispatch loop below, reading from `current_song`. Callers that
// are about to free that song (StopSong, ahead of UnRegisterSong) must wait
// for this to go false first - otherwise the thread can dereference freed
// memory mid-song, since it only re-checks song_playing/thread_running
// between individual events, not continuously.
static boolean      thread_active    = false;

// ---------------------------------------------------------------------------
// Platform-specific: open the MIDI output device
// ---------------------------------------------------------------------------

#if defined(NM_BACKEND_WIN32)

static int NM_PlatformInit(void)
{
    UINT n = midiOutGetNumDevs();
    plat.num_devices = (int)n;

    printf("I_NativeMidi: %u MIDI output device(s):\n", n);
    for (UINT i = 0; i < n; i++)
    {
        MIDIOUTCAPS caps;
        if (midiOutGetDevCaps(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
            printf("  Device %u: %s\n", i, caps.szPname);
    }

    UINT dev_id;
    if (midi_out_device < 0 || midi_out_device >= (int)n)
    {
        fprintf(stderr, "I_NativeMidi: midi_out_device %d out of range, "
                        "falling back to MIDI_MAPPER\n", midi_out_device);
        dev_id = MIDI_MAPPER;
    }
    else
    {
        dev_id = (UINT)midi_out_device;
    }

    MMRESULT res = midiOutOpen(&plat.handle, dev_id, 0, 0, CALLBACK_NULL);
    if (res != MMSYSERR_NOERROR)
    {
        fprintf(stderr, "I_NativeMidi: midiOutOpen failed (error %u)\n", res);
        return 0;
    }
    return 1;
}

static void NM_PlatformShutdown(void)
{
    midiOutClose(plat.handle);
}

// Send a short (1-3 byte) MIDI message
static void NM_SendShortMsg(byte status, byte data1, byte data2)
{
    DWORD msg = status | ((DWORD)data1 << 8) | ((DWORD)data2 << 16);
    midiOutShortMsg(plat.handle, msg);
}

// Send a SysEx message
static void NM_SendSysEx(const byte *data, unsigned int len)
{
    MIDIHDR hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.lpData         = (LPSTR)data;
    hdr.dwBufferLength = len;
    hdr.dwBytesRecorded = len;
    midiOutPrepareHeader(plat.handle, &hdr, sizeof(hdr));
    midiOutLongMsg(plat.handle, &hdr, sizeof(hdr));
    // Poll until done (simple spin; SysEx is rare/infrequent)
    while (!(hdr.dwFlags & MHDR_DONE))
        SDL_Delay(1);
    midiOutUnprepareHeader(plat.handle, &hdr, sizeof(hdr));
}

#elif defined(NM_BACKEND_COREMIDI)

static int NM_PlatformInit(void)
{
    ItemCount n = MIDIGetNumberOfDestinations();
    plat.num_devices = (int)n;

    printf("I_NativeMidi: %lu MIDI output destination(s):\n", (unsigned long)n);
    for (ItemCount i = 0; i < n; i++)
    {
        MIDIEndpointRef ep = MIDIGetDestination(i);
        CFStringRef name = NULL;
        MIDIObjectGetStringProperty(ep, kMIDIPropertyName, &name);
        if (name)
        {
            char buf[256];
            CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
            printf("  Device %lu: %s\n", (unsigned long)i, buf);
            CFRelease(name);
        }
    }

    if (n == 0)
    {
        fprintf(stderr, "I_NativeMidi: no MIDI destinations found\n");
        return 0;
    }

    int idx = midi_out_device;
    if (idx < 0 || idx >= (int)n)
    {
        fprintf(stderr, "I_NativeMidi: midi_out_device %d out of range, "
                        "using device 0\n", idx);
        idx = 0;
    }

    plat.endpoint = MIDIGetDestination((ItemCount)idx);

    OSStatus st = MIDIClientCreate(CFSTR("StrifeVE"), NULL, NULL, &plat.client);
    if (st != noErr)
    {
        fprintf(stderr, "I_NativeMidi: MIDIClientCreate failed (%d)\n", (int)st);
        return 0;
    }

    st = MIDIOutputPortCreate(plat.client, CFSTR("StrifeVE Out"), &plat.out_port);
    if (st != noErr)
    {
        fprintf(stderr, "I_NativeMidi: MIDIOutputPortCreate failed (%d)\n", (int)st);
        MIDIClientDispose(plat.client);
        return 0;
    }

    return 1;
}

static void NM_PlatformShutdown(void)
{
    MIDIPortDispose(plat.out_port);
    MIDIClientDispose(plat.client);
}

static void NM_SendShortMsg(byte status, byte data1, byte data2)
{
    // Stack-allocate a MIDIPacketList for a single 3-byte packet
    Byte buf[sizeof(MIDIPacketList) + 64];
    MIDIPacketList *pktlist = (MIDIPacketList *)buf;
    MIDIPacket     *pkt     = MIDIPacketListInit(pktlist);
    Byte            msg[3]  = { status, data1, data2 };
    // Determine real length
    int len = 3;
    if ((status & 0xf0) == 0xc0 || (status & 0xf0) == 0xd0) len = 2;
    pkt = MIDIPacketListAdd(pktlist, sizeof(buf), pkt, 0, len, msg);
    if (pkt)
        MIDISend(plat.out_port, plat.endpoint, pktlist);
}

static void NM_SendSysEx(const byte *data, unsigned int len)
{
    // Allocate dynamically for variable-length SysEx
    size_t bufsize = sizeof(MIDIPacketList) + len + 64;
    Byte *buf = malloc(bufsize);
    if (!buf) return;
    MIDIPacketList *pktlist = (MIDIPacketList *)buf;
    MIDIPacket     *pkt     = MIDIPacketListInit(pktlist);
    pkt = MIDIPacketListAdd(pktlist, bufsize, pkt, 0, len, data);
    if (pkt)
        MIDISend(plat.out_port, plat.endpoint, pktlist);
    free(buf);
}

#elif defined(NM_BACKEND_ALSA)

static int NM_PlatformInit(void)
{
    glob_t gl;
    int    idx = 0;

    // Enumerate /dev/snd/midiC*D* raw MIDI nodes
    memset(&gl, 0, sizeof(gl));
    glob("/dev/snd/midiC*D*", GLOB_NOSORT, NULL, &gl);
    plat.num_devices = (int)gl.gl_pathc;

    printf("I_NativeMidi: %d raw MIDI device(s):\n", plat.num_devices);
    for (size_t i = 0; i < gl.gl_pathc; i++)
        printf("  Device %zu: %s\n", i, gl.gl_pathv[i]);

    if (plat.num_devices == 0)
    {
        fprintf(stderr, "I_NativeMidi: no raw MIDI devices found in "
                        "/dev/snd/midiC*D*\n");
        globfree(&gl);
        return 0;
    }

    if (midi_out_device < 0 || midi_out_device >= plat.num_devices)
    {
        fprintf(stderr, "I_NativeMidi: midi_out_device %d out of range, "
                        "using device 0\n", midi_out_device);
        idx = 0;
    }
    else
    {
        idx = midi_out_device;
    }

    plat.fd = open(gl.gl_pathv[idx], O_WRONLY | O_NONBLOCK);
    globfree(&gl);

    if (plat.fd < 0)
    {
        perror("I_NativeMidi: open");
        return 0;
    }
    return 1;
}

static void NM_PlatformShutdown(void)
{
    if (plat.fd >= 0)
        close(plat.fd);
    plat.fd = -1;
}

static void NM_SendShortMsg(byte status, byte data1, byte data2)
{
    byte buf[3];
    int  len = 3;
    buf[0] = status;
    buf[1] = data1;
    buf[2] = data2;
    if ((status & 0xf0) == 0xc0 || (status & 0xf0) == 0xd0)
        len = 2;
    write(plat.fd, buf, len);
}

static void NM_SendSysEx(const byte *data, unsigned int len)
{
    write(plat.fd, data, len);
}

#else // Unsupported platform stubs

static int  NM_PlatformInit(void)                              { return 0; }
static void NM_PlatformShutdown(void)                          {}
static void NM_SendShortMsg(byte s, byte d1, byte d2)          { (void)s; (void)d1; (void)d2; }
static void NM_SendSysEx(const byte *d, unsigned int l)        { (void)d; (void)l; }

#endif // platform backend

// ---------------------------------------------------------------------------
// Platform-agnostic helpers
// ---------------------------------------------------------------------------

// Send CC7 (Main Volume) to all 16 MIDI channels
static void NM_SetAllChannelVolume(int vol)
{
    int ch;
    if (vol < 0)   vol = 0;
    if (vol > 127) vol = 127;
    for (ch = 0; ch < 16; ch++)
        NM_SendShortMsg(0xB0 | ch, 0x07, (byte)vol);
}

// All Notes Off + All Sound Off on every channel
static void NM_SilenceAllChannels(void)
{
    int ch;
    for (ch = 0; ch < 16; ch++)
    {
        NM_SendShortMsg(0xB0 | ch, 0x7B, 0); // All Notes Off
        NM_SendShortMsg(0xB0 | ch, 0x78, 0); // All Sound Off
    }
}

// ---------------------------------------------------------------------------
// Device enumeration (used by the options menu to build a device list, and
// by the frontend to let the player hot-swap the output device)
// ---------------------------------------------------------------------------

int I_NativeMidi_GetNumDevices(void)
{
#if defined(NM_BACKEND_WIN32)
    return (int)midiOutGetNumDevs();
#elif defined(NM_BACKEND_COREMIDI)
    return (int)MIDIGetNumberOfDestinations();
#elif defined(NM_BACKEND_ALSA)
    glob_t gl;
    int n;

    memset(&gl, 0, sizeof(gl));
    glob("/dev/snd/midiC*D*", GLOB_NOSORT, NULL, &gl);
    n = (int)gl.gl_pathc;
    globfree(&gl);
    return n;
#else
    return 0;
#endif
}

const char *I_NativeMidi_GetDeviceName(int idx)
{
#if defined(NM_BACKEND_WIN32)
    static char namebuf[MAXPNAMELEN];
    MIDIOUTCAPS caps;

    if (midiOutGetDevCaps((UINT)idx, &caps, sizeof(caps)) != MMSYSERR_NOERROR)
        return NULL;

    strncpy(namebuf, caps.szPname, sizeof(namebuf) - 1);
    namebuf[sizeof(namebuf) - 1] = '\0';
    return namebuf;
#elif defined(NM_BACKEND_COREMIDI)
    static char namebuf[256];
    MIDIEndpointRef ep;
    CFStringRef name = NULL;

    if (idx < 0 || idx >= (int)MIDIGetNumberOfDestinations())
        return NULL;

    ep = MIDIGetDestination((ItemCount)idx);
    if (!ep)
        return NULL;

    MIDIObjectGetStringProperty(ep, kMIDIPropertyName, &name);
    if (!name)
        return NULL;

    CFStringGetCString(name, namebuf, sizeof(namebuf), kCFStringEncodingUTF8);
    CFRelease(name);
    return namebuf;
#elif defined(NM_BACKEND_ALSA)
    static char namebuf[64];
    glob_t gl;
    const char *result = NULL;

    memset(&gl, 0, sizeof(gl));
    glob("/dev/snd/midiC*D*", GLOB_NOSORT, NULL, &gl);
    if (idx >= 0 && (size_t)idx < gl.gl_pathc)
    {
        strncpy(namebuf, gl.gl_pathv[idx], sizeof(namebuf) - 1);
        namebuf[sizeof(namebuf) - 1] = '\0';
        result = namebuf;
    }
    globfree(&gl);
    return result;
#else
    (void)idx;
    return NULL;
#endif
}

// True once the native MIDI module has successfully opened a device and is
// the game's active music module.
boolean I_NativeMidi_IsActive(void)
{
    return midi_initialized;
}

// ---------------------------------------------------------------------------
// MIDI song loading
// ---------------------------------------------------------------------------

// Parse a raw MIDI byte buffer (Type 0 or Type 1) into a flat event list.
// Returns NULL on failure.
static nm_song_t *NM_LoadMIDIFromBuffer(const byte *buf, int len)
{
    char        *tmpname;
    midi_file_t *mf;
    nm_song_t   *song;
    int          total_events = 0;
    unsigned int t, num_tracks;

    // Write to a temp file so MIDI_LoadFile can read it
    tmpname = M_TempFile("strife-ve-nativemidi.mid");
    if (tmpname == NULL)
    {
        fprintf(stderr, "I_NativeMidi: M_TempFile failed\n");
        return NULL;
    }
    if (!M_WriteFile(tmpname, (void *)buf, len))
    {
        fprintf(stderr, "I_NativeMidi: failed to write temp MIDI file\n");
        free(tmpname);
        return NULL;
    }

    mf = MIDI_LoadFile(tmpname);
    remove(tmpname);
    free(tmpname);
    if (!mf)
    {
        fprintf(stderr, "I_NativeMidi: MIDI_LoadFile failed\n");
        return NULL;
    }

    // First pass: count all events across all tracks
    num_tracks = MIDI_NumTracks(mf);
    for (t = 0; t < num_tracks; t++)
    {
        midi_track_iter_t *iter = MIDI_IterateTrack(mf, t);
        midi_event_t      *ev;
        while (MIDI_GetNextEvent(iter, &ev))
            total_events++;
        MIDI_FreeIterator(iter);
    }

    song = (nm_song_t *)Z_Malloc(sizeof(nm_song_t), PU_STATIC, NULL);
    song->events       = (nm_event_t *)Z_Malloc(total_events * sizeof(nm_event_t),
                                                PU_STATIC, NULL);
    song->num_events   = 0;

    // MIDI_GetFileTimeDivision() (midifile.c) reads this 16-bit header field
    // via SHORT(), which expands to SDL_SwapLE16 - a no-op on this
    // little-endian machine - but Standard MIDI Files store it big-endian,
    // like every other multi-byte field in the same header (see the correct
    // SDL_SwapBE32 used for chunk sizes a few lines above it in midifile.c).
    // The value comes back byte-swapped: e.g. mus2mid's 70 ticks/quarter
    // reads back as 17920. i_oplmusic.c has been silently compensating for
    // exactly this with its unexplained "TEMPO_FUDGE_FACTOR = 260" constant
    // (17920 / 70 = 256, suspiciously close). Rather than touch that
    // already-working, unrelated code path, undo the same swap here.
    {
        unsigned int raw = MIDI_GetFileTimeDivision(mf);
        song->time_division = ((raw << 8) | (raw >> 8)) & 0xFFFF;
    }

    // Second pass: collect events with delta times.
    // For multi-track (Type 1), merge all tracks maintaining delta_time
    // relative to the start of each track.  A proper Type-1 merge would
    // need a priority queue; for the game's Type-0 output from mus2mid()
    // there is only ever one track so this is correct.  For safety we still
    // iterate all tracks sequentially (each resets its own time counter).
    for (t = 0; t < num_tracks; t++)
    {
        midi_track_iter_t *iter = MIDI_IterateTrack(mf, t);
        midi_event_t      *ev;
        unsigned int       delta_time;

        // MIDI_GetDeltaTime() reports the delta time of whatever event is at
        // the iterator's *current* position - it must be read before
        // MIDI_GetNextEvent() advances that position, exactly like
        // ScheduleTrack()/TrackTimerCallback() do in i_oplmusic.c. Calling it
        // afterward (as this did) attaches every event's delta time to the
        // *following* event instead, scrambling playback into rapid bursts.
        while ((delta_time = MIDI_GetDeltaTime(iter), MIDI_GetNextEvent(iter, &ev)))
        {
            nm_event_t *ne = &song->events[song->num_events++];
            ne->delta_time = delta_time;
            ne->event      = *ev;
            // Deep-copy variable-length data pointers (meta / sysex data)
            // so they survive after MIDI_FreeFile.
            if (ev->event_type == MIDI_EVENT_META && ev->data.meta.length > 0)
            {
                byte *copy = Z_Malloc(ev->data.meta.length, PU_STATIC, NULL);
                memcpy(copy, ev->data.meta.data, ev->data.meta.length);
                ne->event.data.meta.data = copy;
            }
            else if ((ev->event_type == MIDI_EVENT_SYSEX ||
                      ev->event_type == MIDI_EVENT_SYSEX_SPLIT) &&
                     ev->data.sysex.length > 0)
            {
                byte *copy = Z_Malloc(ev->data.sysex.length, PU_STATIC, NULL);
                memcpy(copy, ev->data.sysex.data, ev->data.sysex.length);
                ne->event.data.sysex.data = copy;
            }
        }
        MIDI_FreeIterator(iter);
    }

    MIDI_FreeFile(mf);
    return song;
}

static void NM_FreeSong(nm_song_t *song)
{
    int i;
    if (!song) return;
    for (i = 0; i < song->num_events; i++)
    {
        nm_event_t *ne = &song->events[i];
        // Only free data pointers that were actually deep-copied with
        // Z_Malloc in NM_LoadMIDIFromBuffer (length > 0 there too) - a
        // zero-length meta event (e.g. every track's End-of-Track event)
        // still carries the *original* pointer into memory owned by the
        // midifile.c parser, freed independently by MIDI_FreeFile(). Freeing
        // that through Z_Free() crashes the zone allocator's sanity check.
        if (ne->event.event_type == MIDI_EVENT_META &&
            ne->event.data.meta.length > 0)
            Z_Free(ne->event.data.meta.data);
        else if ((ne->event.event_type == MIDI_EVENT_SYSEX ||
                  ne->event.event_type == MIDI_EVENT_SYSEX_SPLIT) &&
                 ne->event.data.sysex.length > 0)
            Z_Free(ne->event.data.sysex.data);
    }
    Z_Free(song->events);
    Z_Free(song);
}

// ---------------------------------------------------------------------------
// Playback thread
// ---------------------------------------------------------------------------

// Convert MIDI ticks to microseconds given a tempo (us per quarter-note)
static Uint32 TicksToMs(unsigned int ticks, unsigned int tempo_us,
                         unsigned int time_division)
{
    if (time_division == 0) return 0;
    // tempo_us / time_division gives microseconds per tick
    Uint64 us = (Uint64)ticks * tempo_us / time_division;
    return (Uint32)(us / 1000);
}

static int NM_PlayerThread(void *unused)
{
    (void)unused;

    while (thread_running)
    {
        nm_song_t *song;
        int        i;
        unsigned int tempo_us = 500000; // default 120 BPM

        SDL_LockMutex(player_mutex);
        song = current_song;
        SDL_UnlockMutex(player_mutex);

        if (!song || !song_playing || song_paused)
        {
            SDL_Delay(10);
            continue;
        }

        SDL_LockMutex(player_mutex);
        thread_active = true;
        SDL_UnlockMutex(player_mutex);

        // Play one full pass through all events
        for (i = 0; i < song->num_events && thread_running; i++)
        {
            nm_event_t   *ne = &song->events[i];
            midi_event_t *ev = &ne->event;

            // Wait for the delta time
            if (ne->delta_time > 0)
            {
                Uint32 delay_ms = TicksToMs(ne->delta_time, tempo_us,
                                            song->time_division);
                if (delay_ms > 0)
                {
                    // Break the sleep into small slices so we can react to
                    // pause / stop without a long latency.
                    Uint32 elapsed = 0;
                    while (elapsed < delay_ms && thread_running)
                    {
                        // Re-check pause state each slice
                        if (song_paused)
                        {
                            SDL_Delay(10);
                            // Don't advance time while paused; restart the
                            // wait so timing stays correct after resume.
                            elapsed = 0;
                            continue;
                        }
                        if (!song_playing)
                            break;
                        Uint32 slice = delay_ms - elapsed;
                        if (slice > 10) slice = 10;
                        SDL_Delay(slice);
                        elapsed += slice;
                    }
                }
            }

            if (!song_playing || !thread_running)
                break;

            // Dispatch the event to the hardware
            switch (ev->event_type)
            {
                case MIDI_EVENT_NOTE_OFF:
                case MIDI_EVENT_NOTE_ON:
                case MIDI_EVENT_AFTERTOUCH:
                case MIDI_EVENT_CONTROLLER:
                case MIDI_EVENT_PROGRAM_CHANGE:
                case MIDI_EVENT_CHAN_AFTERTOUCH:
                case MIDI_EVENT_PITCH_BEND:
                    NM_SendShortMsg((byte)(ev->event_type | ev->data.channel.channel),
                                    (byte)ev->data.channel.param1,
                                    (byte)ev->data.channel.param2);
                    break;

                case MIDI_EVENT_SYSEX:
                case MIDI_EVENT_SYSEX_SPLIT:
                    NM_SendSysEx(ev->data.sysex.data, ev->data.sysex.length);
                    break;

                case MIDI_EVENT_META:
                    if (ev->data.meta.type == MIDI_META_SET_TEMPO &&
                        ev->data.meta.length >= 3)
                    {
                        // 3-byte big-endian tempo value
                        const byte *d = ev->data.meta.data;
                        tempo_us = ((unsigned int)d[0] << 16) |
                                   ((unsigned int)d[1] << 8)  |
                                    (unsigned int)d[2];
                    }
                    // End-of-track handled below after the loop
                    break;

                default:
                    break;
            }
        }

        // Done touching `song` for now - safe for a caller waiting in
        // I_NativeMidi_StopSong() to free it as soon as this clears.
        SDL_LockMutex(player_mutex);
        thread_active = false;
        SDL_UnlockMutex(player_mutex);

        // End of song reached
        if (thread_running)
        {
            SDL_LockMutex(player_mutex);
            boolean loop = song_looping;
            SDL_UnlockMutex(player_mutex);

            if (loop)
            {
                // Loop: restart from beginning (tempo resets too)
                continue;
            }
            else
            {
                NM_SilenceAllChannels();
                SDL_LockMutex(player_mutex);
                song_playing = false;
                SDL_UnlockMutex(player_mutex);
            }
        }
    }

    return 0;
}

// Close and reopen the output device using the current value of
// midi_out_device, so the player can switch hardware without restarting the
// game.  Callers should stop any playing song first (e.g. via S_StopMusic())
// and restart it afterward.
//
// The player thread calls NM_SendShortMsg()/NM_SendSysEx() on plat.handle
// with no locking (by design, for tight MIDI timing), so it is not safe to
// call NM_PlatformShutdown()/NM_PlatformInit() while that thread is still
// running - doing so let the main thread close/reopen the handle out from
// under a concurrent midiOutShortMsg() call, which reliably crashed on real
// hardware. Stop and rejoin the thread first, exactly as ShutdownMusic does,
// then start a fresh one once the new device is open.
void I_NativeMidi_ReopenDevice(void)
{
    if (!midi_initialized)
        return;

    thread_running = false;
    song_playing   = false;
    if (player_thread)
    {
        SDL_WaitThread(player_thread, NULL);
        player_thread = NULL;
    }

    NM_SilenceAllChannels();
    NM_PlatformShutdown();

    if (!NM_PlatformInit())
    {
        fprintf(stderr, "I_NativeMidi: failed to reopen MIDI output device %d\n",
                midi_out_device);
        midi_initialized = false;
        return;
    }

    NM_SetAllChannelVolume(current_volume);

    thread_running = true;
    player_thread = SDL_CreateThread(NM_PlayerThread, "NativeMidi", NULL);
    if (!player_thread)
    {
        fprintf(stderr, "I_NativeMidi: SDL_CreateThread failed on reopen: %s\n",
                SDL_GetError());
        thread_running = false;
        NM_PlatformShutdown();
        midi_initialized = false;
    }
}

// ---------------------------------------------------------------------------
// music_module_t callbacks
// ---------------------------------------------------------------------------

static boolean I_NativeMidi_InitMusic(void)
{
    if (midi_initialized)
        return true;

    memset(&plat, 0, sizeof(plat));

    if (!NM_PlatformInit())
        return false;

    player_mutex = SDL_CreateMutex();
    if (!player_mutex)
    {
        fprintf(stderr, "I_NativeMidi: SDL_CreateMutex failed: %s\n",
                SDL_GetError());
        NM_PlatformShutdown();
        return false;
    }

    thread_running = true;
    player_thread = SDL_CreateThread(NM_PlayerThread, "NativeMidi", NULL);
    if (!player_thread)
    {
        fprintf(stderr, "I_NativeMidi: SDL_CreateThread failed: %s\n",
                SDL_GetError());
        thread_running = false;
        SDL_DestroyMutex(player_mutex);
        player_mutex = NULL;
        NM_PlatformShutdown();
        return false;
    }

    midi_initialized = true;
    printf("I_NativeMidi: Native MIDI output initialized (device index %d)\n",
           midi_out_device);
    return true;
}

static void I_NativeMidi_ShutdownMusic(void)
{
    if (!midi_initialized)
        return;

    // Stop the playback thread
    thread_running = false;
    song_playing   = false;
    if (player_thread)
    {
        SDL_WaitThread(player_thread, NULL);
        player_thread = NULL;
    }
    if (player_mutex)
    {
        SDL_DestroyMutex(player_mutex);
        player_mutex = NULL;
    }

    NM_SilenceAllChannels();
    NM_PlatformShutdown();

    midi_initialized = false;
}

static void I_NativeMidi_SetMusicVolume(int volume)
{
    current_volume = volume;
    if (midi_initialized)
        NM_SetAllChannelVolume(volume);
}

static void I_NativeMidi_PauseMusic(void)
{
    SDL_LockMutex(player_mutex);
    song_paused = true;
    SDL_UnlockMutex(player_mutex);
    NM_SilenceAllChannels();
}

static void I_NativeMidi_ResumeMusic(void)
{
    SDL_LockMutex(player_mutex);
    song_paused = false;
    SDL_UnlockMutex(player_mutex);
    NM_SetAllChannelVolume(current_volume);
}

// RegisterSong: called with the raw lump data (MUS or MIDI format).
// Returns a pointer to the nm_song_t, or NULL on failure.
static void *I_NativeMidi_RegisterSong(void *data, int len)
{
    const byte *buf    = (const byte *)data;
    nm_song_t  *song   = NULL;
    byte       *midibuf= NULL;
    int         midilen= 0;

    if (!midi_initialized)
        return NULL;

    // Check for MUS format and convert to MIDI in memory
    if (len > 4 && memcmp(buf, "MUS\x1a", 4) == 0)
    {
        MEMFILE *in  = mem_fopen_read(data, len);
        MEMFILE *out = mem_fopen_write();
        if (mus2mid(in, out))
        {
            fprintf(stderr, "I_NativeMidi: mus2mid conversion failed\n");
            mem_fclose(in);
            mem_fclose(out);
            return NULL;
        }
        void   *outbuf;
        size_t  outlen;
        mem_get_buf(out, &outbuf, &outlen);
        midibuf = Z_Malloc((int)outlen, PU_STATIC, NULL);
        memcpy(midibuf, outbuf, outlen);
        midilen = (int)outlen;
        mem_fclose(in);
        mem_fclose(out);
        buf = midibuf;
        len = midilen;
    }
    else if (len > 4 && memcmp(buf, "MThd", 4) != 0)
    {
        fprintf(stderr, "I_NativeMidi: unknown music format\n");
        return NULL;
    }

    song = NM_LoadMIDIFromBuffer(buf, len);

    if (midibuf)
        Z_Free(midibuf);

    return song;
}

static void I_NativeMidi_UnRegisterSong(void *handle)
{
    NM_FreeSong((nm_song_t *)handle);
}

static void I_NativeMidi_PlaySong(void *handle, boolean looping)
{
    if (!midi_initialized || !handle)
        return;

    SDL_LockMutex(player_mutex);
    // Stop any currently playing song first
    song_playing  = false;
    current_song  = (nm_song_t *)handle;
    song_looping  = looping;
    song_paused   = false;
    song_playing  = true;
    SDL_UnlockMutex(player_mutex);

    NM_SetAllChannelVolume(current_volume);
}

static void I_NativeMidi_StopSong(void)
{
    if (!midi_initialized)
        return;

    SDL_LockMutex(player_mutex);
    song_playing = false;
    song_paused  = false;
    SDL_UnlockMutex(player_mutex);

    // The caller (S_StopMusic) frees the song object right after this call
    // returns (via UnRegisterSong). Make sure the player thread is not still
    // mid-dispatch on it first, or the free races a use of freed memory.
    for (;;)
    {
        boolean active;
        SDL_LockMutex(player_mutex);
        active = thread_active;
        SDL_UnlockMutex(player_mutex);
        if (!active)
            break;
        SDL_Delay(1);
    }

    NM_SilenceAllChannels();
}

static boolean I_NativeMidi_MusicIsPlaying(void)
{
    return song_playing;
}

static void I_NativeMidi_Poll(void)
{
    // Nothing needed; the playback thread drives itself.
}

// ---------------------------------------------------------------------------
// Module descriptor
// ---------------------------------------------------------------------------

static snddevice_t music_native_midi_devices[] =
{
    SNDDEVICE_NATIVE_MIDI,
};

music_module_t music_nativemidi_module =
{
    music_native_midi_devices,
    arrlen(music_native_midi_devices),
    I_NativeMidi_InitMusic,
    I_NativeMidi_ShutdownMusic,
    I_NativeMidi_SetMusicVolume,
    I_NativeMidi_PauseMusic,
    I_NativeMidi_ResumeMusic,
    I_NativeMidi_RegisterSong,
    I_NativeMidi_UnRegisterSong,
    I_NativeMidi_PlaySong,
    I_NativeMidi_StopSong,
    I_NativeMidi_MusicIsPlaying,
    I_NativeMidi_Poll,
};
