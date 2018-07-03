# WebSid (WebAudio version of Tiny'R'Sid)

This is a JavaScript/WebAudio based C64 emulator version of Tiny'R'Sid. This plugin is designed to work with version 1.0 of my 
generic WebAudio ScriptProcessor music player (see separate project). 

It allows to play .mus and RSID/PSID format *.sid music files. (Respective music files can be found here: http://www.hvsc.c64.org/)

There also is a WordPress plugin that allows to easily integrate the player: https://wordpress.org/plugins/tinyrsid-adapter/


The original goal was to design an emulator fast enough to be run within a Web browser on the widest possible range of devices 
(accepting the trade-off that there may be traditional desktop emulators which produce more perfect SID emulation results). 
The implementation approach makes it rather inexpensive in terms of CPU load: CPU load of the Flash version was about 75% less 
than that of a native emulator like sidplay26. The Flash version even runs on my year 2011 "HTC HD2" smartphone.

The emulator logic is written in C/C++. This code is then cross-compiled into a JavaScript or Flash (no longer updated) library 
suitable for the Web.

Eventhough this emulator is taking shortcuts (as compared to a correct cycle by cycle emulator) the player seems to cope pretty 
well with most real world C64 music files (including 2SID & 3SID).

Known limitations: Emulator does not include the Commodore ROM code and music programs written in BASIC therefore are not supported.



![alt text](https://github.com/wothke/websid/raw/master/tinyrsid.jpg "Tiny'R'Sid HVSC Explorer")

An online demo of the emulator can be accessed using: http://www.wothke.ch/tinyrsid/index.php/explorer?playlistId=103 or http://www.wothke.ch/websid/


## Credits

* original TinySid PSID emulator code (there is not much left of it..) - Copyright (C) 1999-2012 T. Hinrichs, R. Sinsch
* "combined waveform" generation, "waveform anti-aliasing" and "filter" implementation by Hermit (see http://hermit.sidrip.com)
* various Tiny'R'Sid PSID & RSID emulator extensions - Copyright (C) 2017 Juergen Wothke 


## Howto build

You'll need Emscripten; see http://kripken.github.io/emscripten-site/docs/getting_started/downloads.html . In case you want to 
compile to WebAssembly (see respective "WASM" switch in the make-scripts) then you'll need at least emscripten 1.37.29 (for
use of WASM you'll then also need a recent version of my respective "WebAudio ScriptProcessor music player").

The below instructions assume a command prompt has been opened within the "websid" folder, and that the Emscripten environment 
vars have been set (run emsdk_env.bat).

Running the makeEmscripten.bat will generate a JavaScript 'Tiny'R'Sid' library (backend_tinyrsid.js) including 
necessary interface APIs. This lib is directly written into the web-page example in the "htdocs" sub-folder. (This 
generated lib is used from some manually written JavaScript/WebAudio code, see htdocs/stdlib/scriptprocessor_player.min.js). 
Study the example in "htdocs" for how the player is used. (disclaimer: the .sh version of the make-script has been contributed
by somebody else and I am not maintaing it or verifying that it still works)

## Depencencies

The current version requires version 1.02 (older versions will not
support WebAssembly) of my https://github.com/wothke/webaudio-player.

## License
Terms of Use: This software is licensed under a CC BY-NC-SA (http://creativecommons.org/licenses/by-nc-sa/4.0/)
	