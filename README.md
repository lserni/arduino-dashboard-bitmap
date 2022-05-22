# arduino-dashboard-bitmap
A simple algorithm to compress (and display) RGB565 dashboard backgrounds on Arduino Uno

## Rationale

I am building a gadget which includes a dashboard-style sensors panel. Of course I *could* just
display the numbers in green over a black background, but I'd prefer something livelier.

Now there are great BMP display routines for Arduino, but they need to
- read 24-bit BMP format data off a SD card.
- convert it to RGB565 16-bit format and endianess.
- push it to the TFT matrix sequentially.

After some tests I have concluded that the most severe bottleneck is bitmap reading. A single 320x240 frame takes about 2700
milliseconds to display on my Arduino Uno, of which about 600 are required by conversion and TFT writing code and 2100 by the SD reading.

There is little I can do for point #3, but conversion could be done in the file; also, some compression techniques are available to
reduce the number of bytes read in point #1.

By storing an array of 320x240 RGB565 values, and pushing them in chunks, I was able to reduce the frame time to about 2000 milliseconds,
so I estimate that the conversion weighs for about 200 ms. This means I need 400 ms to write, 2000 to read, 100 to convert and I can
get rid of those first 200 ms, easy.

I have found a compression library - FastLZ - that promises to compress maybe as much as PNG; for my test dashboards
that would be in the 7-12 Kb range. If reading 150K of data takes 2000ms, that's about 12ms per K plus probably some 100ms fixed time
to decode a FAT32 file system, so I think I could go down to 6-700ms per frame which would be great.

On the other hand, a naive modified run-length encoder is, I believe, simpler to manage and yields files in the 10-16 Kb range, with
load times of about 7-800 ms. The code is easier to understand and the time is good enough for me.

## Tweaks and musings

This code is VERY CRUDE. There are several small tweaks that are possible and are left as an exercise:

- the number of colors can probably be safely increased to 31, maybe even 32 with almost no effort at all.
- if the image has a border the window can be shrunk and the related data can be ignored. This can be done by including an eight-byte
  field specifying x, y, w and h for the bitmap.
- use an impossible combination of codes (e.g. 2x 2x instead of 4x) to indicate "to the end of the current row". This often allows a
  sequence of more than 258 pixels to be coded with two bytes, more compactly than the three bytes Ax yy zz.
- use another impossible combination (2x 6x yy) to mean "process again the last yy +1 bytes from the stream code".
  For example when drawing a grid, 20-pixels wide in color y over x, a lot of it will be like 6x 13 2y 6x 13 2y 6x 13 2y. Before
  outputting the sequence 6x 13 2y 6x 13 2y 6x 13 2y 6x 13 2y 6x 13 2y 6x 13 2y to the file, it can be reviewed and rewritten like
  6x 13 2y 6x 13 2y 6x 13 2y 20 60 08 - from 18 bytes to 12.
  Or: 2x Ax yy zz, means "copy yy+1 bytes from offset zz in the buffer, and do this x+1 times. With a large enough buffer - 30 bytes -
  that often allows coding vertical structures up to 31 rows deep in just four bytes.
  In compression, you'd simply create the file in memory, then parse it again to a more compressed form before outputting.
- a larger tweak, made possibly redundant by the previous one: stop RLE codes at the 320 pixel boundary. This leads to worse results,
  apparently. But now enlarge the write buffer to 640 bytes
  (assuming there's enough RAM to allow the FAT reading code to work; if there isn't, the file won't be found, and you'll know). Now,
  instead of encoding each pixel line, encode the XOR of the new pixel line and the new, and each color XORed with the previous one.
  This means that when two rows contain large similarities, you'll get long swaths of zeroes, which compress better. Now let us consider
  four all-black lines:

    00 00 00 00 00 .. 00 00
    00 00 00 00 00 .. 00 00
    00 00 00 00 00 .. 00 00
    00 00 00 00 00 .. 00 00

  Before the tweak they would have become A0 FD 04. After the tweak it becomes A0 3D 00 ("Output 320 zeroes") four times, so 12 bytes
  instead of 3. But now say we have four lines half black, half white. Those would have been 60 8D 61 8D four times (16 bytes) and are
  now 160 blacks, 1 white, 159 blacks; 320, 320, 320 blacks: 60 8D, 21, 60 8C; A0 3D 00, A0 3D 00, A0 3D 00. 14 bytes. Lines with some
  more transitions show the same effect but stronger.

## Encoder algorithm

The gain estimate uses a very naive, static estimator that assumes no further color table changes. This means that if, at say half image,
I can gain 1000 bytes by storing a color, and color in slot 7 from that point onwards only gathers me a 999 gain, I will evict the color
currently in slot 7. After some more pixels, the relationship might reverse and eviction go the other way. The cost of the two evictions
is issuance of two 4-byte sequences; if I had foregone the first replacement, I would have needed only a 3-byte output literal token,
thus saving one byte. I have calculated that such a pathological sequence can waste 1 byte out of every 16 (or in other words, a more
far-sighted algorithm would squeeze a 12% improvement out of pathological sequences.

In the worst case, 2 consecutive non-storable colors (4 bytes) become 3 bytes (0xC1 0x0000).

A lot of non-consecutive, non-storable colors are a problem because each 2-byte sequence grows to 3 bytes (0xC0 0x0000). 

## Decoder algorithm

I use 40 bytes of RAM to store up to 20 16-bit color codes. The APW file is a stream of tokens organized like this:

- 0x00              EOF
- 0x20 | (00..13)   Output immediate. The color code (from 0 to 20) is retrieved. Single pixels in known colors are encoded as single bytes.
- 0x40 | (00..13)   Output pair of immediate. Same as above, but for two pixels (this is an obvious hack, tailored on my dashboard images).
- 0x60 | (00..13)   Followed by a byte (0 to 255), emits from 3 to 258 copies of the specified color.
- 0x80 | (00..13)   Followed by a byte (0 to 255) and a word (color code), sets the specified color slot to a color and outputs from 1 to 256 pixels in that color.
- 0xA0 | (00..13)   Followed by a word (0 to 65535), emits from 259 to 65794 copies of the specified color.
- 0xC0 | (00..1F)   Output from 1 to 32 copies of the next word as pixels. 0xC7 0xF8F0 means output 8 pixels of color 0xF8F0.
- 0xE0              Followed by a byte and a color code, output from 33 to 288 copies of a given color. 
- 0x01-0x1F         reserved
- 0xE1-0xFF         reserved

Some combinations that make no sense (e.g. the same colour being encoded repeatedly separately instead of together, e.g. 2x 2x instead of 4x,
or 2x Ay instead of A(y+1) ) can be used to encode extra information.

## Warning

This algorithm is designed **for dashboard bitmaps**. Definitely **not** for shaded bitmaps. Generally speaking, if it compresses well in PNG,
then it ought to compress reasonably in APW. If it compresses best using JPEG, then APW is *definitely* not for you.

## Features

- Smaller memory footprint than BMP libraries.
- A 3x-5x speed improvement in generic **dashboard-style** bitmaps.
- A 1.5x improvement in baroque dashboard bitmaps with lots of fine details.
- Tolerable slowdown (0.9x) in really unsuitable bitmaps.
- The simplest (or smaller) bitmaps are small enough that they can fit into PROGMEM. Just sayin'.

## License

- This code is free for personal and commercial use per GNU General Public License version 3.0 (GPLv3)

## Build

- Linux compressor
gcc -o png2apw png2apw.c -lpng

- Arduino library
just copy and paste apwdraw.c code in your .ino file
