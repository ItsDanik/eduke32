/*
 Copyright (C) 1994-1995 Apogee Software, Ltd.
 Copyright (C) 2009 Jonathon Fowler <jf@jonof.id.au>
 Copyright (C) EDuke32 developers and contributors

 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License
 as published by the Free Software Foundation; either version 2
 of the License, or (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

 See the GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

 */
/**********************************************************************
   module: AL_MIDI.C

   author: James R. Dose
   date:   April 1, 1994

   Low level routines to support General MIDI music on AdLib compatible
   cards.

   (c) Copyright 1994 James R. Dose.  All Rights Reserved.
**********************************************************************/

/**
 * MIDI music on the FM chip (YM3812) of a MiSTer hybrid core.
 *
 * This is the AdLib driver of the Apogee Sound System, by way of EDuke32's
 * driver_adlib (and of jfSW's MiSTer core, whose driver this is), for one
 * OPL2: nine voices of two operators, no stereo. The
 * chip is the core's: its register writes go to MH_FMWrite(), each with the
 * audio frame it is due at, and the core adds the chip's sound to the audio.
 * The sequencer is clocked by the sound driver (MisterFMDrv_Advance()), so
 * the music keeps time to the sample; without a sound driver it does not play.
 *
 * While the OSD's Music says MIDI and there is something to play it
 * (MH_MidiWanted(): an MT32-pi on the core's user port, host version 15, or
 * the core's own synthesizer, host version 16), the song's MIDI goes there
 * instead, at the same audio frames, and the chip plays nothing; a song
 * started then is played as for General MIDI (MisterFMDrv_SongStart()). The
 * channels' state is kept for both, so the OSD switches between them at once.
 */

#ifdef MISTER_HYBRID

#include <pthread.h>
#include <stddef.h>
#include <string.h>
#include <mister_hybrid.h>

#include "midifuncs.h"
#include "music.h"
#include "midi.h"
#include "_midi.h"
#include "sndcards.h"
#include "al_midi.h"
#include "driver_misterfm.h"

enum {
   MisterFMErr_Warning = -2,
   MisterFMErr_Error   = -1,
   MisterFMErr_Ok      = 0,
   MisterFMErr_Uninitialised,
   MisterFMErr_NoChip
};

#define AL_MaxVolume             127
#define AL_DefaultChannelVolume  90
#define AL_DefaultPitchBendRange 200
#define AL_VoiceNotFound         -1

#define NUMADLIBVOICES   9
#define NUMADLIBCHANNELS 16
#define AL_NumChipSlots  18

#define NOTE_ON  0x2000
#define NOTE_OFF 0x0000

#define MAX_VELOCITY   0x7f
#define MAX_OCTAVE     7
#define MAX_NOTE       (MAX_OCTAVE * 12 + 11)
#define FINETUNE_MAX   31
#define FINETUNE_RANGE (FINETUNE_MAX + 1)

#define PITCHBEND_CENTER 1638400

// Registers of the chip
#define OPL_TEST                0x01
#define OPL_WAVE_SELECT_ENABLE  0x20  // in OPL_TEST
#define OPL_KBD_SPLIT           0x08
#define OPL_SAVEK               0x20  // tremolo, vibrato, sustain, KSR, multiple
#define OPL_KSL_LEVEL           0x40
#define OPL_ATTACK_DECAY        0x60
#define OPL_SUSTAIN_RELEASE     0x80
#define OPL_FNUM_LOW            0xA0
#define OPL_KEYON_BLOCK         0xB0
#define OPL_PERCUSSION          0xBD
#define OPL_FEEDBACK_CONNECTION 0xC0
#define OPL_WAVE_SELECT         0xE0

#define OPL_TOTAL_LEVEL_MASK 0x3f
#define OPL_KSL_MASK         0xc0
#define OPL_FEEDBACK_MASK    0x0e
#define OPL_CONNECTION_BIT   0x01

// MH_FMVolume() with the music at full volume: twice the chip as it comes
#define FM_FULL_VOLUME 64

typedef struct AdLibVoice
{
    struct AdLibVoice *next;
    struct AdLibVoice *prev;

    int num;
    int key;
    int velocity;
    int channel;
    int pitch;
    int timbre;
    int status;
} AdLibVoice;

typedef struct
{
    AdLibVoice *start;
    AdLibVoice *end;
} AdLibVoiceList;

typedef struct
{
    AdLibVoiceList Voices;

    int Timbre;
    int Pitchbend;
    int KeyOffset;
    int KeyDetune;
    int Volume;
    int Detune;
    int RPN;
    int PitchBendRange;
    int PitchBendSemiTones;
    int PitchBendHundreds;
} AdLibChannel;

/* Definition of octave information to be ORed onto F-Number */

static const int OctavePitch[MAX_OCTAVE+1] = {
    0x0000, 0x0400, 0x0800, 0x0C00, 0x1000, 0x1400, 0x1800, 0x1C00,
};

// Pitch table: C to B, for each 1/32 of a semitone above it

static const int NotePitch[FINETUNE_MAX+1][12] = {
    { 0x157, 0x16b, 0x181, 0x198, 0x1b0, 0x1ca, 0x1e5, 0x202, 0x220, 0x241, 0x263, 0x287 },
    { 0x157, 0x16b, 0x181, 0x198, 0x1b0, 0x1ca, 0x1e5, 0x202, 0x220, 0x242, 0x264, 0x288 },
    { 0x158, 0x16c, 0x182, 0x199, 0x1b1, 0x1cb, 0x1e6, 0x203, 0x221, 0x243, 0x265, 0x289 },
    { 0x158, 0x16c, 0x183, 0x19a, 0x1b2, 0x1cc, 0x1e7, 0x204, 0x222, 0x244, 0x266, 0x28a },
    { 0x159, 0x16d, 0x183, 0x19a, 0x1b3, 0x1cd, 0x1e8, 0x205, 0x223, 0x245, 0x267, 0x28b },
    { 0x15a, 0x16e, 0x184, 0x19b, 0x1b3, 0x1ce, 0x1e9, 0x206, 0x224, 0x246, 0x268, 0x28c },
    { 0x15a, 0x16e, 0x185, 0x19c, 0x1b4, 0x1ce, 0x1ea, 0x207, 0x225, 0x247, 0x269, 0x28e },
    { 0x15b, 0x16f, 0x185, 0x19d, 0x1b5, 0x1cf, 0x1eb, 0x208, 0x226, 0x248, 0x26a, 0x28f },
    { 0x15b, 0x170, 0x186, 0x19d, 0x1b6, 0x1d0, 0x1ec, 0x209, 0x227, 0x249, 0x26b, 0x290 },
    { 0x15c, 0x170, 0x187, 0x19e, 0x1b7, 0x1d1, 0x1ec, 0x20a, 0x228, 0x24a, 0x26d, 0x291 },
    { 0x15d, 0x171, 0x188, 0x19f, 0x1b7, 0x1d2, 0x1ed, 0x20b, 0x229, 0x24b, 0x26e, 0x292 },
    { 0x15d, 0x172, 0x188, 0x1a0, 0x1b8, 0x1d3, 0x1ee, 0x20c, 0x22a, 0x24c, 0x26f, 0x293 },
    { 0x15e, 0x172, 0x189, 0x1a0, 0x1b9, 0x1d4, 0x1ef, 0x20d, 0x22b, 0x24d, 0x270, 0x295 },
    { 0x15f, 0x173, 0x18a, 0x1a1, 0x1ba, 0x1d4, 0x1f0, 0x20e, 0x22c, 0x24e, 0x271, 0x296 },
    { 0x15f, 0x174, 0x18a, 0x1a2, 0x1bb, 0x1d5, 0x1f1, 0x20f, 0x22d, 0x24f, 0x272, 0x297 },
    { 0x160, 0x174, 0x18b, 0x1a3, 0x1bb, 0x1d6, 0x1f2, 0x210, 0x22e, 0x250, 0x273, 0x298 },
    { 0x161, 0x175, 0x18c, 0x1a3, 0x1bc, 0x1d7, 0x1f3, 0x211, 0x22f, 0x251, 0x274, 0x299 },
    { 0x161, 0x176, 0x18c, 0x1a4, 0x1bd, 0x1d8, 0x1f4, 0x212, 0x230, 0x252, 0x276, 0x29b },
    { 0x162, 0x176, 0x18d, 0x1a5, 0x1be, 0x1d9, 0x1f5, 0x212, 0x231, 0x254, 0x277, 0x29c },
    { 0x162, 0x177, 0x18e, 0x1a6, 0x1bf, 0x1d9, 0x1f5, 0x213, 0x232, 0x255, 0x278, 0x29d },
    { 0x163, 0x178, 0x18f, 0x1a6, 0x1bf, 0x1da, 0x1f6, 0x214, 0x233, 0x256, 0x279, 0x29e },
    { 0x164, 0x179, 0x18f, 0x1a7, 0x1c0, 0x1db, 0x1f7, 0x215, 0x235, 0x257, 0x27a, 0x29f },
    { 0x164, 0x179, 0x190, 0x1a8, 0x1c1, 0x1dc, 0x1f8, 0x216, 0x236, 0x258, 0x27b, 0x2a1 },
    { 0x165, 0x17a, 0x191, 0x1a9, 0x1c2, 0x1dd, 0x1f9, 0x217, 0x237, 0x259, 0x27c, 0x2a2 },
    { 0x166, 0x17b, 0x192, 0x1aa, 0x1c3, 0x1de, 0x1fa, 0x218, 0x238, 0x25a, 0x27e, 0x2a3 },
    { 0x166, 0x17b, 0x192, 0x1aa, 0x1c3, 0x1df, 0x1fb, 0x219, 0x239, 0x25b, 0x27f, 0x2a4 },
    { 0x167, 0x17c, 0x193, 0x1ab, 0x1c4, 0x1e0, 0x1fc, 0x21a, 0x23a, 0x25c, 0x280, 0x2a6 },
    { 0x168, 0x17d, 0x194, 0x1ac, 0x1c5, 0x1e0, 0x1fd, 0x21b, 0x23b, 0x25d, 0x281, 0x2a7 },
    { 0x168, 0x17d, 0x194, 0x1ad, 0x1c6, 0x1e1, 0x1fe, 0x21c, 0x23c, 0x25e, 0x282, 0x2a8 },
    { 0x169, 0x17e, 0x195, 0x1ad, 0x1c7, 0x1e2, 0x1ff, 0x21d, 0x23d, 0x260, 0x283, 0x2a9 },
    { 0x16a, 0x17f, 0x196, 0x1ae, 0x1c8, 0x1e3, 0x1ff, 0x21e, 0x23e, 0x261, 0x284, 0x2ab },
    { 0x16a, 0x17f, 0x197, 0x1af, 0x1c8, 0x1e4, 0x200, 0x21f, 0x23f, 0x262, 0x286, 0x2ac }
};

// Slot numbers as a function of the voice and the operator.

static const int slotVoice[NUMADLIBVOICES][2] = {
    { 0, 3 },    // voice 0
    { 1, 4 },    // 1
    { 2, 5 },    // 2
    { 6, 9 },    // 3
    { 7, 10 },   // 4
    { 8, 11 },   // 5
    { 12, 15 },  // 6
    { 13, 16 },  // 7
    { 14, 17 },  // 8
};

// This table gives the offset of each slot within the chip.

static const int offsetSlot[AL_NumChipSlots] = { 0, 1, 2, 3, 4, 5, 8, 9, 10, 11, 12, 13, 16, 17, 18, 19, 20, 21 };

static int VoiceLevel[AL_NumChipSlots];
static int VoiceKsl[AL_NumChipSlots];

static AdLibVoice     Voice[NUMADLIBVOICES];
static AdLibVoiceList Voice_Pool;

static AdLibChannel Channel[NUMADLIBCHANNELS];

static int ErrorCode = MisterFMErr_Ok;
static int Initialised;

// One lock for the sequencer and the chip's ring of writes, which has room
// for one writer at a time
static pthread_mutex_t Lock;
static int LockReady;

static int Playing;
static int TicksPerSecond;   // of the sequencer
static int TickTimer;        // in ticks * frames per second
static int WriteAhead;       // audio frame the chip's writes are due at
// The MT32-pi's cable is 31250 baud: the resets and settings sent before a
// song (jfaudiolib's MIDI_Reset() and MF_StateSend(), up to 1500 bytes, half
// a second) held back its first notes, and their ends came nearly on time:
// cut short (E1L04's first note is 0.24 s in). The sequencer waits for them.
static int HoldCheck;        // the settings were sent: see how long they take
static int Hold;             // the core's audio frames the sequencer waits


static void AL_AddToTail(AdLibVoiceList *list, AdLibVoice *voice)
{
    voice->next = NULL;
    voice->prev = list->end;

    if (list->end)
        list->end->next = voice;
    else
        list->start = voice;

    list->end = voice;
}


static void AL_Remove(AdLibVoiceList *list, AdLibVoice *voice)
{
    if (voice->prev == NULL)
        list->start = voice->next;
    else
        voice->prev->next = voice->next;

    if (voice->next == NULL)
        list->end = voice->prev;
    else
        voice->next->prev = voice->prev;

    voice->next = NULL;
    voice->prev = NULL;
}


static void AL_SendOutput(int reg, int data)
{
    MH_FMWrite(reg, data, WriteAhead);
}


// MIDI to the MT32-pi or the synthesizer, and the state of its channels the
// song has set: the controllers of MidiStateCC, the program and the pitch
// bend (-1: not set)
static int MidiOut;
static const unsigned char MidiStateCC[] = { 0, 1, 7, 10, 11, 64, 91, 93 };
#define MIDI_STATE_PROGRAM ((int)sizeof(MidiStateCC))
#define MIDI_STATE_BEND    ((int)sizeof(MidiStateCC) + 1)
static int MidiState[NUMADLIBCHANNELS][sizeof(MidiStateCC) + 2];

// MIDI's sound at full music volume (MH_MidiVolume(): 32 as it comes)
#define MIDI_FULL_VOLUME 32

static void MF_StateReset(void)
{
    memset(MidiState, 0xff, sizeof(MidiState));
}

static void MF_Event(int status, int data1, int data2)
{
    int *state = MidiState[status & 15];
    unsigned i;

    switch (status & 0xf0)
    {
        case 0xb0:
            if (data1 == MIDI_RESET_ALL_CONTROLLERS)
            {
                // all but the volume, pan, bank and effects' sends
                state[1] = state[4] = state[5] = state[MIDI_STATE_BEND] = -1;
            }
            for (i = 0; i < sizeof(MidiStateCC); i++)
                if (data1 == MidiStateCC[i])
                    state[i] = data2;
            break;

        case 0xc0:
            state[MIDI_STATE_PROGRAM] = data1;
            break;

        case 0xe0:
            state[MIDI_STATE_BEND] = data1 | (data2 << 7);
            break;
    }

    if (MidiOut)
        MH_MidiEvent(WriteAhead, status, data1, data2);
}

// The MT32-pi is told what the song has set so far, after a reset of all its
// sound
static void MF_StateSend(void)
{
    // General MIDI on: whatever ran before on the Pi
    static const unsigned char gm_on[] = { 0x7e, 0x7f, 0x09, 0x01 };
    int ch, i, *state;

    MH_MidiReset(WriteAhead);
    MH_MidiSysEx(WriteAhead, gm_on, sizeof(gm_on));
    HoldCheck = 1;
    for (ch = 0; ch < NUMADLIBCHANNELS; ch++)
    {
        state = MidiState[ch];
        for (i = 0; i < (int)sizeof(MidiStateCC); i++)
            if (state[i] >= 0)
                MH_MidiEvent(WriteAhead, 0xb0 | ch, MidiStateCC[i], state[i]);
        if (state[MIDI_STATE_PROGRAM] >= 0)
            MH_MidiEvent(WriteAhead, 0xc0 | ch, state[MIDI_STATE_PROGRAM], 0);
        if (state[MIDI_STATE_BEND] >= 0)
            MH_MidiEvent(WriteAhead, 0xe0 | ch, state[MIDI_STATE_BEND] & 0x7f, state[MIDI_STATE_BEND] >> 7);
    }
}

static void AL_AllNotesOff(int channel);

// Where the music goes, as the OSD and the MT32-pi say. Under the lock.
static void MF_Follow(void)
{
    int want, ch;

    if (!MH_MidiAvailable())
        return;

    // once the audio is there; nothing if it already is
    MH_MidiEnable(1);
    want = MH_MidiWanted();
    if (want == MidiOut)
    {
        // the Pi switched synth or SoundFont, or its notes were reset: it
        // has none of the song's settings
        if (want && MH_MidiSynthChanged())
            MF_StateSend();
        return;
    }

    MidiOut = want;
    if (want)
    {
        // the notes from now on on the MT32-pi
        for (ch = 0; ch < NUMADLIBCHANNELS; ch++)
            AL_AllNotesOff(ch);
        MH_MidiSynthChanged();
        MF_StateSend();
    }
    else
        MH_MidiReset(WriteAhead);
}


static void AL_SetVoiceTimbre(int voice)
{
    int channel = Voice[voice].channel;
    int patch = (channel == 9) ? Voice[voice].key + 128 : Channel[channel].Timbre;
    const AdLibTimbre *timbre;
    int slot, off;

    if (Voice[voice].timbre == patch)
        return;

    Voice[voice].timbre = patch;

    timbre = &ADLIB_TimbreBank[patch];

    slot = slotVoice[voice][0];
    off  = offsetSlot[slot];

    VoiceLevel[slot] = OPL_TOTAL_LEVEL_MASK - (timbre->Level[0] & OPL_TOTAL_LEVEL_MASK);
    VoiceKsl[slot]   = timbre->Level[0] & OPL_KSL_MASK;

    AL_SendOutput(OPL_FNUM_LOW + voice, 0);
    AL_SendOutput(OPL_KEYON_BLOCK + voice, 0);

    // Let voice clear the release
    AL_SendOutput(OPL_SUSTAIN_RELEASE + off, 0xff);

    AL_SendOutput(OPL_ATTACK_DECAY + off, timbre->Env1[0]);
    AL_SendOutput(OPL_SUSTAIN_RELEASE + off, timbre->Env2[0]);
    AL_SendOutput(OPL_SAVEK + off, timbre->SAVEK[0]);
    AL_SendOutput(OPL_WAVE_SELECT + off, timbre->Wave[0]);

    AL_SendOutput(OPL_KSL_LEVEL + off, timbre->Level[0]);

    AL_SendOutput(OPL_FEEDBACK_CONNECTION + voice,
        timbre->Feedback & (OPL_FEEDBACK_MASK | OPL_CONNECTION_BIT));

    slot = slotVoice[voice][1];
    off  = offsetSlot[slot];

    VoiceLevel[slot] = OPL_TOTAL_LEVEL_MASK - (timbre->Level[1] & OPL_TOTAL_LEVEL_MASK);
    VoiceKsl[slot]   = timbre->Level[1] & OPL_KSL_MASK;

    AL_SendOutput(OPL_KSL_LEVEL + off, OPL_TOTAL_LEVEL_MASK);

    // Let voice clear the release
    AL_SendOutput(OPL_SUSTAIN_RELEASE + off, 0xff);

    AL_SendOutput(OPL_ATTACK_DECAY + off, timbre->Env1[1]);
    AL_SendOutput(OPL_SUSTAIN_RELEASE + off, timbre->Env2[1]);
    AL_SendOutput(OPL_SAVEK + off, timbre->SAVEK[1]);
    AL_SendOutput(OPL_WAVE_SELECT + off, timbre->Wave[1]);
}


static void AL_SetVoiceVolume(int voice)
{
    int channel = Voice[voice].channel;
    const AdLibTimbre *timbre = &ADLIB_TimbreBank[Voice[voice].timbre];
    int velocity = Voice[voice].velocity + timbre->Velocity;
    int slot = slotVoice[voice][1];
    unsigned int t1, t2, volume;

    if (velocity > MAX_VELOCITY)
        velocity = MAX_VELOCITY;

    // amplitude
    t1 = (unsigned int)VoiceLevel[slot] * (velocity + 0x80);
    t1 = (Channel[channel].Volume * t1) >> 15;

    volume = t1 ^ OPL_TOTAL_LEVEL_MASK;
    volume |= (unsigned int)VoiceKsl[slot];

    AL_SendOutput(OPL_KSL_LEVEL + offsetSlot[slot], volume);

    // Check if this timbre is Additive
    if (timbre->Feedback & 0x01)
    {
        slot = slotVoice[voice][0];

        // amplitude: of the other operator, as the original driver has it
        t2 = (Channel[channel].Volume * t1) >> 15;

        volume = t2 ^ OPL_TOTAL_LEVEL_MASK;
        volume |= (unsigned int)VoiceKsl[slot];

        AL_SendOutput(OPL_KSL_LEVEL + offsetSlot[slot], volume);
    }
}


static int AL_AllocVoice(void)
{
    int voice;

    if (!Voice_Pool.start)
        return AL_VoiceNotFound;

    voice = Voice_Pool.start->num;
    AL_Remove(&Voice_Pool, &Voice[voice]);
    return voice;
}


static int AL_GetVoice(int channel, int key)
{
    const AdLibVoice *voice = Channel[channel].Voices.start;

    while (voice != NULL)
    {
        if (voice->key == key)
            return voice->num;
        voice = voice->next;
    }

    return AL_VoiceNotFound;
}


static void AL_SetVoicePitch(int voice)
{
    int channel = Voice[voice].channel;
    int patch, note, pitch;

    if (channel == 9)
    {
        patch = Voice[voice].key + 128;
        note  = ADLIB_TimbreBank[patch].Transpose;
    }
    else
    {
        patch = Channel[channel].Timbre;
        note  = Voice[voice].key + ADLIB_TimbreBank[patch].Transpose;
    }

    note += Channel[channel].KeyOffset - 12;
    if (note > MAX_NOTE)
        note = MAX_NOTE;
    if (note < 0)
        note = 0;

    pitch = OctavePitch[note / 12] | NotePitch[Channel[channel].KeyDetune][note % 12];

    Voice[voice].pitch = pitch;

    pitch |= Voice[voice].status;

    AL_SendOutput(OPL_FNUM_LOW + voice, pitch & 0xff);
    AL_SendOutput(OPL_KEYON_BLOCK + voice, pitch >> 8);
}


static void AL_SetChannelVolume(int channel, int volume)
{
    AdLibVoice *voice;

    if (volume > AL_MaxVolume)
        volume = AL_MaxVolume;
    if (volume < 0)
        volume = 0;
    Channel[channel].Volume = volume;

    voice = Channel[channel].Voices.start;
    while (voice != NULL)
    {
        AL_SetVoiceVolume(voice->num);
        voice = voice->next;
    }
}


static void AL_SetChannelDetune(int channel, int detune)
{
    Channel[channel].Detune = detune;
}


static void AL_ResetVoices(void)
{
    int index;

    Voice_Pool.start = NULL;
    Voice_Pool.end   = NULL;

    for (index = 0; index < NUMADLIBVOICES; index++)
    {
        Voice[index].num      = index;
        Voice[index].key      = 0;
        Voice[index].velocity = 0;
        Voice[index].channel  = -1;
        Voice[index].timbre   = -1;
        Voice[index].status   = NOTE_OFF;
        AL_AddToTail(&Voice_Pool, &Voice[index]);
    }

    for (index = 0; index < NUMADLIBCHANNELS; index++)
    {
        memset(&Channel[index], 0, sizeof(Channel[index]));
        Channel[index].Volume             = AL_DefaultChannelVolume;
        Channel[index].PitchBendRange     = AL_DefaultPitchBendRange;
        Channel[index].PitchBendSemiTones = AL_DefaultPitchBendRange / 100;
        Channel[index].PitchBendHundreds  = AL_DefaultPitchBendRange % 100;
    }
}


static void AL_FlushCard(void)
{
    int i, slot1, slot2;

    for (i = 0; i < NUMADLIBVOICES; i++)
    {
        slot1 = offsetSlot[slotVoice[i][0]];
        slot2 = offsetSlot[slotVoice[i][1]];

        AL_SendOutput(OPL_FNUM_LOW + i, 0);
        AL_SendOutput(OPL_KEYON_BLOCK + i, 0);

        AL_SendOutput(OPL_WAVE_SELECT + slot1, 0);
        AL_SendOutput(OPL_WAVE_SELECT + slot2, 0);

        // Set the envelope to be fast and quiet
        AL_SendOutput(OPL_ATTACK_DECAY + slot1, 0xff);
        AL_SendOutput(OPL_ATTACK_DECAY + slot2, 0xff);
        AL_SendOutput(OPL_SUSTAIN_RELEASE + slot1, 0xff);
        AL_SendOutput(OPL_SUSTAIN_RELEASE + slot2, 0xff);

        // Maximum attenuation
        AL_SendOutput(OPL_KSL_LEVEL + slot1, 0xff);
        AL_SendOutput(OPL_KSL_LEVEL + slot2, 0xff);
    }
}


static void AL_Reset(void)
{
    AL_SendOutput(OPL_TEST, OPL_WAVE_SELECT_ENABLE);
    AL_SendOutput(OPL_KBD_SPLIT, 0);

    // Set the values: AM Depth, VIB depth & Rhythm
    AL_SendOutput(OPL_PERCUSSION, 0);

    AL_FlushCard();
}


static void AL_NoteOff(int channel, int key, int velocity)
{
    int voice;

    (void)velocity;

    if (channel < 0 || channel >= NUMADLIBCHANNELS)
        return;

    voice = AL_GetVoice(channel, key);

    if (voice == AL_VoiceNotFound)
        return;

    Voice[voice].status = NOTE_OFF;

    AL_SendOutput(OPL_KEYON_BLOCK + voice, Voice[voice].pitch >> 8);

    AL_Remove(&Channel[channel].Voices, &Voice[voice]);
    AL_AddToTail(&Voice_Pool, &Voice[voice]);
}


static void AL_NoteOn(int channel, int key, int velocity)
{
    int voice;

    if (channel < 0 || channel >= NUMADLIBCHANNELS)
        return;

    if (velocity == 0)
    {
        AL_NoteOff(channel, key, velocity);
        return;
    }

    voice = AL_AllocVoice();

    if (voice == AL_VoiceNotFound)
    {
        // a drum makes way
        if (Channel[9].Voices.start)
        {
            AL_NoteOff(9, Channel[9].Voices.start->key, 0);
            voice = AL_AllocVoice();
        }

        if (voice == AL_VoiceNotFound)
            return;
    }

    Voice[voice].key      = key;
    Voice[voice].channel  = channel;
    Voice[voice].velocity = velocity;
    Voice[voice].status   = NOTE_ON;

    AL_AddToTail(&Channel[channel].Voices, &Voice[voice]);

    AL_SetVoiceTimbre(voice);
    AL_SetVoiceVolume(voice);
    AL_SetVoicePitch(voice);
}


static void AL_AllNotesOff(int channel)
{
    while (Channel[channel].Voices.start != NULL)
        AL_NoteOff(channel, Channel[channel].Voices.start->key, 0);
}


static void AL_ControlChange(int channel, int type, int data)
{
    if (channel < 0 || channel >= NUMADLIBCHANNELS)
        return;

    switch (type)
    {
        case MIDI_VOLUME:
            AL_SetChannelVolume(channel, data);
            break;

        case MIDI_DETUNE:
            AL_SetChannelDetune(channel, data);
            break;

        case MIDI_ALL_NOTES_OFF:
            AL_AllNotesOff(channel);
            break;

        case MIDI_RESET_ALL_CONTROLLERS:
            AL_ResetVoices();
            AL_SetChannelVolume(channel, AL_DefaultChannelVolume);
            AL_SetChannelDetune(channel, 0);
            break;

        case MIDI_RPN_MSB:
            Channel[channel].RPN &= 0x00FF;
            Channel[channel].RPN |= (data & 0xFF) << 8;
            break;

        case MIDI_RPN_LSB:
            Channel[channel].RPN &= 0xFF00;
            Channel[channel].RPN |= data & 0xFF;
            break;

        case MIDI_DATAENTRY_MSB:
            if (Channel[channel].RPN == MIDI_PITCHBEND_MSB)
            {
                Channel[channel].PitchBendSemiTones = data;
                Channel[channel].PitchBendRange     = Channel[channel].PitchBendSemiTones * 100 + Channel[channel].PitchBendHundreds;
            }
            break;

        case MIDI_DATAENTRY_LSB:
            if (Channel[channel].RPN == MIDI_PITCHBEND_LSB)
            {
                Channel[channel].PitchBendHundreds = data;
                Channel[channel].PitchBendRange    = Channel[channel].PitchBendSemiTones * 100 + Channel[channel].PitchBendHundreds;
            }
            break;
    }
}


static void AL_ProgramChange(int channel, int patch)
{
    if (channel < 0 || channel >= NUMADLIBCHANNELS)
        return;

    Channel[channel].Timbre = patch & 0x7f;
}


static void AL_SetPitchBend(int channel, int lsb, int msb)
{
    int pitchbend, TotalBend;
    AdLibVoice *voice;

    if (channel < 0 || channel >= NUMADLIBCHANNELS)
        return;

    pitchbend = lsb + (msb << 8);
    TotalBend = pitchbend * Channel[channel].PitchBendRange / (PITCHBEND_CENTER / FINETUNE_RANGE);

    Channel[channel].Pitchbend = pitchbend;
    Channel[channel].KeyOffset = TotalBend / FINETUNE_RANGE;
    Channel[channel].KeyOffset -= Channel[channel].PitchBendSemiTones;

    Channel[channel].KeyDetune = TotalBend % FINETUNE_RANGE;

    voice = Channel[channel].Voices.start;
    while (voice != NULL)
    {
        AL_SetVoicePitch(voice->num);
        voice = voice->next;
    }
}


// The volume of the music is the chip's in the core's mix, or the MT32-pi's
static void AL_SetVolume(int volume)
{
    if (volume > MIDI_MaxVolume)
        volume = MIDI_MaxVolume;
    if (volume < 0)
        volume = 0;

    MH_FMVolume(volume * FM_FULL_VOLUME / MIDI_MaxVolume);
    MH_MidiVolume(volume * MIDI_FULL_VOLUME / MIDI_MaxVolume);
}


// The sequencer's functions: to the MT32-pi, or to the chip
static void MF_NoteOff(int channel, int key, int velocity)
{
    if (MidiOut)
        MF_Event(0x80 | channel, key, velocity);
    else
        AL_NoteOff(channel, key, velocity);
}

static void MF_NoteOn(int channel, int key, int velocity)
{
    if (MidiOut)
        MF_Event(0x90 | channel, key, velocity);
    else
        AL_NoteOn(channel, key, velocity);
}

static void MF_PolyAftertouch(int channel, int key, int pressure)
{
    MF_Event(0xa0 | channel, key, pressure);
}

// the chip's channels follow these as well
static void MF_ControlChange(int channel, int number, int value)
{
    MF_Event(0xb0 | channel, number, value);
    AL_ControlChange(channel, number, value);
}

static void MF_ProgramChange(int channel, int program)
{
    MF_Event(0xc0 | channel, program, 0);
    AL_ProgramChange(channel, program);
}

static void MF_ChannelAftertouch(int channel, int pressure)
{
    MF_Event(0xd0 | channel, pressure, 0);
}

static void MF_PitchBend(int channel, int lsb, int msb)
{
    MF_Event(0xe0 | channel, lsb, msb);
    AL_SetPitchBend(channel, lsb, msb);
}

int MisterFMDrv_GetError(void)
{
    return ErrorCode;
}

const char *MisterFMDrv_ErrorString( int ErrorNumber )
{
    const char *ErrorString;

    switch( ErrorNumber )
    {
        case MisterFMErr_Warning :
        case MisterFMErr_Error :
            ErrorString = MisterFMDrv_ErrorString( ErrorCode );
            break;

        case MisterFMErr_Ok :
            ErrorString = "MiSTer FM ok.";
            break;

        case MisterFMErr_Uninitialised:
            ErrorString = "MiSTer FM uninitialised.";
            break;

        case MisterFMErr_NoChip:
            ErrorString = "The core has no FM chip (host version 10 is needed).";
            break;

        default:
            ErrorString = "Unknown MiSTer FM error.";
            break;
    }

    return ErrorString;
}

int MisterFMDrv_MIDI_Init(midifuncs *funcs)
{
    MisterFMDrv_MIDI_Shutdown();

    if (!MH_FMAvailable()) {
        ErrorCode = MisterFMErr_NoChip;
        return MisterFMErr_Error;
    }

    if (!LockReady) {
        pthread_mutexattr_t attr;

        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&Lock, &attr);
        pthread_mutexattr_destroy(&attr);
        LockReady = 1;
    }

    memset(funcs, 0, sizeof(midifuncs));
    funcs->NoteOff           = MF_NoteOff;
    funcs->NoteOn            = MF_NoteOn;
    funcs->PolyAftertouch    = MF_PolyAftertouch;
    funcs->ControlChange     = MF_ControlChange;
    funcs->ProgramChange     = MF_ProgramChange;
    funcs->ChannelAftertouch = MF_ChannelAftertouch;
    funcs->PitchBend         = MF_PitchBend;
    funcs->SetVolume         = AL_SetVolume;

    // the tracks and the volume and program changes a song has for this card
    // (for General MIDI when it starts on the MT32-pi, MisterFMDrv_SongStart())
    ASS_EMIDICard = EMIDI_AdLib;
    MidiOut = 0;
    MF_StateReset();

    AL_ResetVoices();

    Initialised = 1;
    ErrorCode = MisterFMErr_Ok;
    return MisterFMErr_Ok;
}

void MisterFMDrv_MIDI_Shutdown(void)
{
    if (!Initialised) {
        return;
    }

    MisterFMDrv_MIDI_HaltPlayback();
    MH_FMEnable(0);
    // the core tells the MT32-pi to stop its notes
    MH_MidiEnable(0);
    MidiOut = 0;

    Initialised = 0;
}

int MisterFMDrv_MIDI_StartPlayback(void)
{
    if (!Initialised) {
        ErrorCode = MisterFMErr_Uninitialised;
        return MisterFMErr_Error;
    }

    MisterFMDrv_MIDI_HaltPlayback();

    pthread_mutex_lock(&Lock);

    // Does nothing while the chip is on. It can only be switched on once the
    // sound driver has started the audio.
    MH_FMEnable(1);
    AL_Reset();
    AL_ResetVoices();
    MF_StateReset();
    if (MidiOut)
        MF_StateSend();

    TickTimer = 0;
    Playing = 1;

    pthread_mutex_unlock(&Lock);

    return MisterFMErr_Ok;
}

void MisterFMDrv_MIDI_HaltPlayback(void)
{
    if (!Initialised) {
        return;
    }

    pthread_mutex_lock(&Lock);
    Playing = 0;
    pthread_mutex_unlock(&Lock);
}

void MisterFMDrv_MIDI_SetTempo(int tempo, int division)
{
    TicksPerSecond = ( tempo * division ) / 60;
}

void MisterFMDrv_MIDI_Lock(void)
{
    if (LockReady) {
        pthread_mutex_lock(&Lock);
    }
}

void MisterFMDrv_MIDI_Unlock(void)
{
    if (LockReady) {
        pthread_mutex_unlock(&Lock);
    }
}

void MisterFMDrv_SongStart(void)
{
    if (!Initialised)
        return;

    pthread_mutex_lock(&Lock);
    MF_Follow();
    ASS_EMIDICard = MidiOut ? EMIDI_GeneralMIDI : EMIDI_AdLib;
    pthread_mutex_unlock(&Lock);
}

void MisterFMDrv_Advance(int frames, int rate)
{
    int i;

    if (!Playing || rate <= 0) {
        return;
    }

    pthread_mutex_lock(&Lock);

    MF_Follow();
    if (HoldCheck)
    {
        HoldCheck = 0;
        Hold = MidiOut ? MH_MidiBacklog() : 0;
    }

    for (i = 0; i < frames && Playing; i++)
    {
        if (Hold > 0 && (long long)i * MH_AUDIO_RATE / rate < Hold)
            continue;
        TickTimer += TicksPerSecond;
        while (TickTimer >= rate && Playing)
        {
            TickTimer -= rate;
            // the core plays at its own rate, whatever the game mixes at
            WriteAhead = (int)((long long)i * MH_AUDIO_RATE / rate);
            MIDI_ServiceRoutine();
        }
    }

    WriteAhead = 0;
    if (Hold > 0)
    {
        Hold -= (int)((long long)frames * MH_AUDIO_RATE / rate);
        if (Hold < 0)
            Hold = 0;
    }

    pthread_mutex_unlock(&Lock);
}

#endif
