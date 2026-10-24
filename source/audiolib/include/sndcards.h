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
/**********************************************************************
   module: SNDCARDS.H

   author: James R. Dose
   date:   March 31, 1994

   Contains enumerated type definitions for sound cards.

   (c) Copyright 1994 James R. Dose.  All Rights Reserved.
**********************************************************************/

#ifndef __SNDCARDS_H
#define __SNDCARDS_H

// ALSA MIDI output: Linux, except the MiSTer hybrid core, whose Linux has no
// ALSA (its music goes to the core: driver_misterfm)
#if defined __linux__ && !defined MISTER_HYBRID
# define AUDIOLIB_HAVE_ALSA
#endif

extern int ASS_PCMSoundDriver;
extern int ASS_MIDISoundDriver;
extern int ASS_EMIDICard;

typedef enum
{
    ASS_SDL,
    ASS_DirectSound,
    ASS_OPL3,
    ASS_WinMM,
    ASS_SF2,
    ASS_ALSA,
#ifdef MISTER_HYBRID
    ASS_MisterFM,   // the core's FM chip, or its MIDI (driver_misterfm)
#endif
    ASS_NumSoundCards,
    ASS_AutoDetect = -2
} soundcardnames;

#endif
