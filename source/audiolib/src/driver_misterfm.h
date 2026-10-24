/*
 Copyright (C) 1994-1995 Apogee Software, Ltd.

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

/**
 * MIDI music on the FM chip (YM3812) of a MiSTer hybrid core
 */

#ifndef __DRIVER_MISTERFM_H
#define __DRIVER_MISTERFM_H

#include "midifuncs.h"

int  MisterFMDrv_GetError(void);
const char *MisterFMDrv_ErrorString( int ErrorNumber );

int  MisterFMDrv_MIDI_Init(midifuncs *);
void MisterFMDrv_MIDI_Shutdown(void);
int  MisterFMDrv_MIDI_StartPlayback(void);
void MisterFMDrv_MIDI_HaltPlayback(void);
void MisterFMDrv_MIDI_SetTempo(int tempo, int division);
void MisterFMDrv_MIDI_Lock(void);
void MisterFMDrv_MIDI_Unlock(void);

// Before a song starts: where it goes (the chip or the MT32-pi), and so the
// tracks of it that are played (EMIDI)
void MisterFMDrv_SongStart(void);

// The clock of the music: called by the sound driver with every block of
// `frames` frames at `rate` Hz it is about to fill, from the thread that
// queues the block.
void MisterFMDrv_Advance(int frames, int rate);

#endif
