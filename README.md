This project is to house experiments in timestamping PPS signals on a computer running macOS.

The context is that you have
- a GPS receiver with a PPS output
- a Mac such as a MacBook or a Mac mini with no inputs other than USB and possible a microphone
- you want to run a NTP server i.e. chrony on the Mac
- you therefore want to determine very precisely the time of each pulse with respect to the Mac's system clock

For more background, see Jeff Geerling's [blog post](https://www.jeffgeerling.com/blog/2025/using-gps-most-accurate-time-possible-on-mac). Jeff suggests a couple of possibilities

- don't use PPS; just rely on timing derived from NMEA messages (Jeff claims this can achieve 1ms precision, but I think that is an illusion)
- use Linux in a VM (e.g. using Docker) with USB pass through; this allows you to leverage Linux kernel support for PPS (I haven;t tried this but it's a plausible approach)

This repo is an attempt to provide some additional possibilities.

## Polling modem status lines

The first experiment, which is implemented by the `pollpps` program, explores the idea of polling the modem status lines, specifically the CTS line. This requires a USB-to-TTL adapter that supports CTS/RTS, and then connect the PPS output of the GPS receiver to the CTS pin of the adapter. A suitable adapter is the [Waveshare USB-to-TTL converter](https://www.waveshare.com/usb-to-ttl.htm), which uses the FTDI FT232RNL.

![usbttl](https://github.com/user-attachments/assets/227569bf-45b0-44bf-9d60-345059552a1c)

In fact, gpsd implements a similar approach but requires an OS that supports TIOCMIWAIT, which avoids the need to poll. Unfortunately macOS doesn't support this, so gpsd does not support PPS on macOS.

The obvious downside of polling is the CPU usage from having to poll extremely frequently. But modern CPUs have sufficient capacity to make this approach is viable. There are also some tricks (not yet implemented) that we could use to reduce CPU usage. For example, once we have detected a pulse edge, we know that the next edge will not happen for a second, so we can stop polling frequently for nearly a second.

The level of precision that can be achieved with this is limited. The timestamping is being done completely in user space and USB introduces significant extra jitter compared to a direct serial port.

This experiment has chrony refclock sock support integrated.  Run with

```
sudo ./pollpps --chrony /dev/cu.usbserial-AB0MHJAU
```

and add something like this to your chrony config file

```
refclock SOCK /var/run/chrony.pollpps.sock pps refid CTS
```

## Audio

The second experiment is more interesting. macOS has no support for precision time keeping, but it has excellent support for audio, including audio synchronization. The idea is to piggy back PPS support on top of the audio support.

The starting point is to build a simple, passive circuit to turn the PPS input pulse into something that can be fed into the LINE input of a USB audio card. Then we use the macOS CoreAudio framework to read the audio samples and detect the pulse. CoreAudio has kernel support for timestamping audio samples. Each sample packet is associated with a host time (in Linux terms, a raw monotonic time). We can map this onto a system time. USB audio uses isochronous USB which avoids much of the jitter that occurs with regular USB. This has the potential for much greater precision that the first approach.

The circuit looks like this

```
           R1 (10kΩ)
PPS+ >----/\/\/\----+---- C1 (0.1µF) ----> LINE IN (Tip)
(3.3V)              |
                 R2 (1kΩ)
                    |
PPS- >--------------+--------------------> LINE IN (Sleeve/GND)
(GND)               |
                   GND
```

The parts you need for this are:

- 10kΩ resistor
- 1kΩ resistor
- 0.1µF film capacitor
- a TRRS breakout board

I used a small breadboard to assemble it. The only soldering needed is to solder header pins onto the TRRS breakout board.

You also need a USB audio card with a LINE IN (not just a MIC IN). The one I found has multiple inputs and outputs including SPDIF. It cost about $9.

![audio](https://github.com/user-attachments/assets/4c7faf32-2eda-4b74-8cc4-1c935b6b22f3)


This needs a command like:

```
./audiopps --threshold 0.1 "AppleUSBAudioEngine:Unknown Manufacturer:USB Sound Device        :3112000:2" "External Line Connector"
```

First argument is the device; second argument is the input source. Use `./audiopps --list-devices` to get the available devices and their sources.

Here's an example of what I see (on a Mac mini with the system clock synchronized using chrony to a high quality stratum 1 NTP server on the LAN):

```
PPS detected at 1749040513.000411 (level: 0.161, sample: 97/1024)
PPS detected at 1749040514.000419 (level: 0.262, sample: 997/1024)
PPS detected at 1749040515.000429 (level: 0.186, sample: 872/1024)
PPS detected at 1749040516.000416 (level: 0.274, sample: 748/1024)
PPS detected at 1749040517.000364 (level: 0.215, sample: 623/1024)
PPS detected at 1749040518.000387 (level: 0.120, sample: 498/1024)
PPS detected at 1749040519.000383 (level: 0.235, sample: 386/1024)
PPS detected at 1749040520.000390 (level: 0.153, sample: 389/1024)
PPS detected at 1749040521.000408 (level: 0.255, sample: 393/1024)
PPS detected at 1749040522.000401 (level: 0.183, sample: 396/1024)
PPS detected at 1749040523.000389 (level: 0.266, sample: 400/1024)
PPS detected at 1749040524.000388 (level: 0.209, sample: 403/1024)
PPS detected at 1749040525.000367 (level: 0.115, sample: 406/1024)
PPS detected at 1749040526.000392 (level: 0.234, sample: 410/1024)
PPS detected at 1749040527.000382 (level: 0.143, sample: 413/1024)
PPS detected at 1749040528.000415 (level: 0.254, sample: 417/1024)
PPS detected at 1749040529.000391 (level: 0.175, sample: 420/1024)
PPS detected at 1749040530.000393 (level: 0.262, sample: 424/1024)
PPS detected at 1749040531.000393 (level: 0.203, sample: 427/1024)
PPS detected at 1749040532.000360 (level: 0.107, sample: 430/1024)
PPS detected at 1749040533.000381 (level: 0.223, sample: 434/1024)
PPS detected at 1749040534.000382 (level: 0.137, sample: 437/1024)
PPS detected at 1749040535.000398 (level: 0.247, sample: 441/1024)
PPS detected at 1749040536.000388 (level: 0.164, sample: 444/1024)
PPS detected at 1749040537.000387 (level: 0.265, sample: 448/1024)
PPS detected at 1749040538.000379 (level: 0.196, sample: 451/1024)
PPS detected at 1749040539.000389 (level: 0.270, sample: 455/1024)
PPS detected at 1749040540.000374 (level: 0.222, sample: 458/1024)
```

This is an order of magnitude better than the modem status line polling.

I haven't yet hooked this up to chrony.

