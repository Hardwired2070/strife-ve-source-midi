Strife Veteran's Edition - Ext. MIDI
=====================================

A fork of the Strife: Veteran Edition GPL source release that adds a
selectable **external MIDI hardware output**, so the game's music can be
routed via MIDI to a hardware synthesizer/sound module or software/virtual synth, 
whatever's hooked up to your audio interface instead of being limited to the 
built-in OPL and Roland SC-55 emulation.

Repository: https://github.com/Hardwired2070/strife-ve-source-midi

Features
--------

- A new **"Ext. MIDI"** option alongside the existing "Roland SC-55" and
  "OPL2/OPL3" choices under Options -> Audio -> Music Type.
- A **"MIDI Dev"** selector (shown only when Music Type is "Ext. MIDI")
  listing every MIDI output device Windows currently knows about. Switching
  devices applies immediately, no restart required, and your choice is
  saved to `strife.cfg`.

Also fixes, along the way
--------------------------

- **Fullscreen / Screen Resolution settings now actually apply.** The
  "Luna" build configuration (used here specifically to avoid requiring the
  unavailable Steamworks SDK - see below) is Night Dive's internal target
  for Amazon Luna cloud streaming, and unconditionally forced fullscreen
  plus a fixed/environment-driven resolution regardless of what was
  configured. That's correct for an actual cloud-streaming build; it isn't
  for a normal desktop one, so it's disabled here.

Installing the release
-----------------------

1. Download the latest release exe from the
   [Releases](https://github.com/Hardwired2070/strife-ve-source-midi/releases) page.
2. Drop it into your existing Strife: Veteran Edition install folder
   (Steam: `...\steamapps\common\Strife\`; GOG: wherever you installed it).
3. Run it instead of the original `strife-ve.exe`.

You need a legitimate purchased copy of Strife: Veteran Edition (Steam or
GOG) already installed - this only replaces the executable, it does not
include any game data.

Building from source
---------------------

Open `msvc2015/chocolate.sln` in Visual Studio and build the
**"Luna Release"** (or "Luna Debug" for a debug build with console/log
output) configuration. This target avoids requiring the Steamworks or GOG
Galaxy SDKs, which aren't included in this public GPL source release (see
"Steam, GOG Galaxy, and Nintendo Switch" below) - multiplayer is
unavailable in builds without them, same as it always has been in
from-source builds of this engine.

You'll need `SDL2.dll`, `SDL2_mixer.dll`, and `SDL2_net.dll` (plus
`libogg-0.dll`, `libvorbis-0.dll`, `libvorbisfile-3.dll`) that match what
Strife: Veteran Edition itself ships. The generic redistributables from
libsdl.org are built differently and will silently break MIDI/audio
playback - the simplest fix is to copy those DLLs directly out of an
existing Strife: VE install rather than downloading them fresh. For
testing locally, also copy `strife-music.cfg` and the `music/` folder from
an existing install alongside your build - the "Roland SC-55" option is
implemented as a lookup table of pre-rendered substitute tracks keyed off
those files, not real-time MIDI synthesis, and won't produce audio without
them.

Love the app? Buy me a coffee on Ko-fi! https://ko-fi.com/hardwired2070

Known limitations
------------------

- **Multiplayer** is unavailable, same as any from-source build without
  the Steamworks/GOG Galaxy SDKs - not something this fork changed.

---

The sections below are Night Dive Studios' original documentation for the
Strife: Veteran Edition GPL Source Code release, unmodified.

Strife: Veteran Edition GPL Source Code
=======================================
Copyright 2020, Night Dive Studios, Incorporated.

This file contains the following sections:

- GENERAL NOTES
- LICENSE

GENERAL NOTES
=============

Game data and patching:
-----------------------

This source release does not contain any game data, the game data is still
covered by the original EULA and must be obeyed as usual.

Strife: Veteran Edition is available from GOG at
https://www.gog.com/game/strife_veteran_edition

Strife: Veteran Edition is available from the Steam store at
http://store.steampowered.com/app/317040/

Compiling on Windows:
---------------------

A project file for Microsoft Visual Studio 2015 is provided in 
msvc2015/chocolate.sln and should compile with any edition.

Compiling on MacOS 10.x:
------------------------

A project file for XCode is provided in xcode/Chocolate/Strife-VE.xcodeproj
All dependencies and settings should already be set inside the workspace.
The XCode project may currently be out of date as of this source release.

Compiling on Linux:
-------------------

A CMake build system has been added in version 1.3, and is currently up-to-date.

A project file for Codeblocks is provided in codeblocks/chocolate.workspace
Before compiling, be sure to compile patchelf first by going into the patchelf/
directory and then running configure and then make. The project has not been
updated for 1.3, so some source files may need to be added before you can 
build successfully.

Codeblocks uses a post-build step that calls patchelf to set up runtime paths
for the output executable.

Trademark disclaimers:
----------------------

The source code may make reference to the following trademarks:

"DOOM" is a trademark of ZeniMax Media, Incorporated.
"Heretic" and "Hexen" are trademarks of Raven Software, Incorporated.
"Strife: Veteran Edition" is a trademark of Night Dive Studios, Incorporated.

No license for use or transfer of ownership in any trademark is implied or
should be construed.


Steam, GOG Galaxy, and Nintendo Switch:
---------------------------------------
The Strife: Veteran Edition GPL Source Code release does not include 
functionality for integrating with Steam, GOG Galaxy, or Nintendo Switch.
This includes roaming profiles, achievements, leaderboards, matchmaking, the
overlay, or any other Steam or Galaxy features.  It may be necessary to 
undefine the `_USE_STEAM_`, `GOG_RELEASE`, or `SVE_PLAT_SWITCH` symbols in 
build scripts or project files in order to compile.


Other platforms, updated source code, security issues:
------------------------------------------------------

If you have obtained this source code several weeks after the time of release,
it is likely that you can find modified and improved versions of the engine in
various open source projects across the Internet.

Depending on your interest with the source code, those may be a better starting
point.


LICENSE
=======

The Strife: Veteran Edition source code is available under the terms of the GNU
General Public License v2.0

Exceptions for linking with Steam, GOG Galaxy, and Nintendo Switch runtimes,
which are subject to non-disclosure agreements, were obtained by Night Dive
Studios, Inc. Code subject to NDA is not included with this public source 
release.

See COPYING.txt for the GNU GENERAL PUBLIC LICENSE


EXCLUDED CODE: The code described below and contained in the Strife: Veteran 
Edition GPL Source Code release is not part of the Program covered by the GPL 
and is expressly excluded from its terms.  You are solely responsible for 
obtaining from the copyright holder a license for such code and complying with
the applicable license terms.

libogg, libtheora, libvorbis
----------------------------
Copyright (c) 2002-2018 Xiph.org Foundation

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

- Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.

- Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.

- Neither the name of the Xiph.org Foundation nor the names of its
contributors may be used to endorse or promote products derived from
this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION
OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

libpng
-------------------------------------------------------------------------------
libpng version 1.6.12 - June 12, 2014
Copyright (c) 1998-2014 Glenn Randers-Pehrson
 (Version 0.96 Copyright (c) 1996, 1997 Andreas Dilger)
 (Version 0.88 Copyright (c) 1995, 1996 Guy Eric Schalnat, Group 42, Inc.)

The PNG Reference Library is supplied "AS IS".  The Contributing Authors
and Group 42, Inc. disclaim all warranties, expressed or implied,
including, without limitation, the warranties of merchantability and of
fitness for any purpose.  The Contributing Authors and Group 42, Inc.
assume no liability for direct, indirect, incidental, special, exemplary,
or consequential damages, which may result from the use of the PNG
Reference Library, even if advised of the possibility of such damage.

Permission is hereby granted to use, copy, modify, and distribute this
source code, or portions hereof, for any purpose, without fee, subject
to the following restrictions:

  1. The origin of this source code must not be misrepresented.

  2. Altered versions must be plainly marked as such and must not
     be misrepresented as being the original source.

  3. This Copyright notice may not be removed or altered from
     any source or altered source distribution.

The Contributing Authors and Group 42, Inc. specifically permit, without
fee, and encourage the use of this source code as a component to
supporting the PNG file format in commercial products.  If you use this
source code in a product, acknowledgment is not required but would be
appreciated.

SDL
---
SDL2, SDL2_mixer, and SDL2_net are used under the terms of SDL license:

This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.

zlib
-------------------------------------------------------------------------------
version 1.2.7, May 2nd, 2012

Copyright (C) 1995-2012 Jean-loup Gailly and Mark Adler

This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.

-------------------------------------------------------------------------------
