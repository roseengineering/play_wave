
play_wave
-------------

play_wave is a hack of play_sdr [1] (which itself is based off of
rtl_sdr [2]) for the SDRPlay [3].  Instead of outputting a
byte stream to a file, play_wave outputs the data stream
to a WAVE file.  The sample rate, the recording start time, and the
frequency for the capture are embedded in the WAVE header.

In addition, the PCM data stream is now 16 bits per sample instead of
8 bits.  The default sampling was also changed to 2MSPS from 2.048MSPS.

Motivation
-----------

My primary motivation for the hack is to support the SDRPlay
on open source operating systems, mainly using Wine, the Windows
emulator.  My ExtIO DLL for the rtl_tcp accomplishes this too 
but unfortunately it requires a dedicated networked RTL2832U device
which it does not share.

Once play_wave is running in the background, any radio 
client that supports WAVE should be able to play the streamed file.
For example, HDSDR [4] running in Wine had no problems playing
the streamed file.  HDSDR thinks the streamed file is an ordinary
file.  Of course this will not likely work if play back is set right up at
the end of the streamed file.

play_wave can also be used as the software equivalent of a lossless 
RF splitter, since files can support multiple readers.
By running play_wave on a networked file system, say on a Raspberry Pi 
hosting a SMB/CIFS file share, radio clients on any of the networked 
computers can play the file stream.  And this all can be
done at the same time up to the capacity of the network and the 
file server.

Notes
------

Since the program does not know the length of the recording at its start,
the length of the WAVE file is set in the metadata as -1.  
Thankfully HDSDR does not complain about this "bad" length.  I have
not tested HDSDR with WAVE files greater than 4GB, the putative WAVE
file size limit.



[1] https://github.com/SDRplay/examples
[2] http://sdr.osmocom.org/trac/wiki/rtl-sdr 
[3] http://www.sdrplay.com/
[4] http://www.hdsdr.de/


