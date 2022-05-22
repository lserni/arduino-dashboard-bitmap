#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <png.h>

#undef DEBUG

/**
 * Convert a 24-bit PNG 320x240 bitmap into APW format for Arduino RGB565 TFTs
 *
 */

int rgb_to_565(int r, int g, int b) {
    return ((r & 0xF8) << 8)
           |
           ((g & 0xFC) << 3)
           |
           ((b & 0xF8) >> 3);
}

/**
 * Determine the length of a run starting at ptr.
 *
 * @returns size_t
 */
int runLength(uint16_t *buffer, size_t ptr, size_t maxSize) {
	size_t run = 0;
	uint16_t color = buffer[ptr];
	while (ptr < maxSize) {
		if (color == buffer[ptr++]) {
			run++;
		} else {
            break;
        }
	}
	return run;
}

/**
 * Estimate the data size gain should we store color 'mine' and use it from ptr onwards
 *
 */

int calcGain(uint16_t *buffer, size_t ptr, size_t maxSize, uint16_t mine) {
	int wouldSpend  = 0;
	int canSpend    = 0;
    int r;
	while (ptr < maxSize) {
		if (buffer[ptr] != mine) {
			ptr++;
			continue;
		}
		// Is there a run here?
		r = runLength(buffer, ptr, maxSize);
        // If we did not store the color, how much would it cost?
        switch (r) {
            case 1:
            case 2:
		        wouldSpend += r*3; // LITERAL_NO_STORE|RUN + COLOR_CODE
			    canSpend += 1; // OUTPUT_IMMEDIATE od OUTPUT_PAIR
                break;
            default:
                if (r < 32) {
                    canSpend   += 2;
                    wouldSpend += 3;
                } else if (r < 258) {
                    canSpend += 2; // OUTPUT_SHORT_CODE | SHORT_LENGTH + COLOR_INDEX
                    wouldSpend += 4; // MULTIPLE_LITERAL + RUN + COLOR_CODE
                } else {
                    canSpend += 3; // OUTPUT_LONG_CODE | LONG_LENGTH + COLOR_INDEX
                    wouldSpend += 4*((r + 255 + 31 - 1)/(255 + 31));
                }
		}
		ptr += r;
	}
	return wouldSpend - canSpend;
}

#define MAX_COLORS          20
#define OUTPUT_EOF          0x00

#define OUTPUT_IMMEDIATE    0x20    // Da 32 a 52
#define OUTPUT_PAIR         0x40    // Da 64 a 84
#define OUTPUT_SHORT_CODE   0x60    
#define STORE_ENTRY         0x80
#define OUTPUT_LONG_CODE    0xA0
#define LITERAL_NO_STORE    0xC0
#define MULTIPLE_NO_STORE   0xE0


#define emit_byte(b)        putc((b) & 0xFF, outfile)
#define emit_word(w)        output_word(w, outfile)

void output_word(uint16_t w, FILE *outfile) {
    uint8_t lo, hi;
    lo  = w & 0xFF;
    hi  = (w >> 8) & 0xFF;
    #ifdef DEBUG
    printf("        EMIT_W(%04x): %02x %02x\n", w, lo, hi);
    #endif
    emit_byte(lo);
    emit_byte(hi);
}

int main(int argc, char **argv)
{
    FILE    *infile         = NULL,
            *outfile        = NULL;
    png_uint_32	w, h;
    png_struct	*png_ptr	= NULL;
    png_info	*info		= NULL;
    uint8_t		*out        = NULL;
    uint8_t		**jbuf		= NULL;
    uint16_t    *rgb565     = NULL;
    uint8_t		header[8];
    int		    color_type, dummy, bytes;
    unsigned	i, ptr;
    uint8_t		*pixel;
    uint16_t    colors[MAX_COLORS];
    int         codes  [7] = { 0, 0, 0, 0, 0, 0, 0 };
    int         read   [7] = { 0, 0, 0, 0, 0, 0, 0 };
    int         written[7] = { 0, 0, 0, 0, 0, 0, 0 };
    int         litStatus = 0;
    int         litCount  = 0;

    if (argc != 3)
    {
        fprintf(stderr, "%s: convert PNG to Arduino RGB565 APW\n", argv[0]);
        fprintf(stderr, "syntax: %s input.png output.apw\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (NULL == (infile = fopen(argv[1], "rb")))
    {
        fprintf(stderr, "cannot open '%s' for reading: %s\n", argv[1], strerror(errno));
        return EXIT_FAILURE;
    }
    if (8 != fread (header, 1, 8, infile))
    {
        fprintf(stderr, "Truncated file: '%s'\n", argv[1]);
        fclose(infile); infile = NULL;
        return EXIT_FAILURE;
    }
    if (png_sig_cmp (header, 0, 8))
    {
        fprintf(stderr, "not a PNG file: '%s'\n", argv[1]);
        fclose(infile); infile = NULL;
        return EXIT_FAILURE;
    }
    if (NULL == (png_ptr = png_create_read_struct (PNG_LIBPNG_VER_STRING, NULL, NULL, NULL)))
    {
        fprintf(stderr, "cannot create PNG struct\n");
        fclose(infile); infile = NULL;
        return EXIT_FAILURE;
    }
    if (NULL == (info = png_create_info_struct (png_ptr)))
    {
        fprintf(stderr, "cannot create INFO struct\n");
        fclose(infile); infile = NULL;
        return EXIT_FAILURE;
    }
    png_init_io (png_ptr, infile);
    png_set_sig_bytes(png_ptr, 8);
    png_set_keep_unknown_chunks(png_ptr, 1, NULL, 0);
    png_read_info (png_ptr, info);
    if (1 != png_get_IHDR(png_ptr, info, &w, &h, &dummy, &color_type, &dummy, &dummy, &dummy)) {
        fprintf(stderr, "Error reading HDR\n");
        fclose(infile); infile = NULL;
        return EXIT_FAILURE;
    }
    switch (color_type) {
        case PNG_COLOR_TYPE_RGB:
            bytes = 3;
            break;
        case PNG_COLOR_TYPE_RGB_ALPHA:
            bytes = 4;
            break;
        default:
            fprintf(stderr, "not a RGB(+Alpha) file\n");
            fclose(infile); infile = NULL;
            return EXIT_FAILURE;
    }
    if (NULL == (rgb565 = malloc(h * w * sizeof(uint16_t)))) {
        fprintf(stderr, "out of memory\n");
        return EXIT_FAILURE;
    }
    if (NULL == (out = malloc(w * h * bytes))) {
        free(rgb565); rgb565 = NULL;
        png_destroy_read_struct (&png_ptr, &info, NULL);
        fprintf(stderr, "out of memory\n");
        fclose(infile); infile = NULL;
        return EXIT_FAILURE;
    }
    png_set_strip_16	(png_ptr);
    png_set_packing		(png_ptr);
    png_set_expand		(png_ptr);

    if (NULL == (jbuf = malloc(h * sizeof(uint8_t *)))) {
        free(out); out = NULL;
        free(rgb565); rgb565 = NULL;
        png_destroy_read_struct (&png_ptr, &info, NULL);
        fprintf(stderr, "out of memory\n");
        fclose(infile); infile = NULL;
        return EXIT_FAILURE;
    }
    for (i = 0; i < h; i++) {
        jbuf[i]	= out + w * bytes * i;
    }
    png_read_image(png_ptr, jbuf);

    png_read_end (png_ptr, info);
    png_destroy_read_struct (&png_ptr, &info, NULL);
      
    free(jbuf); jbuf = NULL;
    fclose (infile); /* infile = NULL; */

    printf("w=%d, h=%d\n", w, h);

    #ifdef DEBUG
    outfile=fopen("debug.raw", "w");
    fwrite(out, h, w*bytes, outfile);
    fclose(outfile);
    #endif

    pixel = out;

    // Convert to RGB565
    for (i = 0; i < w*h; i++) {
        rgb565[i]   = rgb_to_565(pixel[0], pixel[1], pixel[2]);
        pixel	+= bytes;
    }
    free(out); out = NULL;

    #ifdef STATS
    // Statistics
    int *stats;
    stats = malloc(65536*sizeof(int));
    for (i = 0; i < 65536; i++) {
        stats[i] = 0;
    }
    for (i = 0; i < w*h; i++) {
        stats[rgb565[i]]++;
    }
    int int_compare(const void *pa, const void *pb) {
        const int *a = pa, *b = pb;
        return (*b)-(*a);
    }
    qsort(stats, 65536, sizeof(int), int_compare);
    for (i = 0; i < 65536; i++) {
        printf("Freq #%d: %d\n", i, stats[i]);
        if (0 == stats[i]) {
            break;
        }
    }
    free(stats); stats = NULL;
    #endif

    // COMPRESS
    colors[0]   = 0x0000;
    colors[1]   = 0xFFFF;
    for (i = 2; i < MAX_COLORS; i++) {
        colors[i] = 0x0000;
    }

    outfile	= fopen(argv[2], "w");
    emit_byte('C');
    emit_byte('B');
    emit_byte('L');
    emit_byte('S');
    emit_word(w);
    emit_word(h);
    emit_byte(MAX_COLORS);

    for (ptr = 0; ptr < w*h;) {
        int r, haveColor = 0;
        #ifdef DEBUG
        printf("Found color code: %04x at offset %d\n", rgb565[ptr], ptr);
        #endif
        for (i = 0; i < MAX_COLORS; i++) {
            if (colors[i] == rgb565[ptr]) {
                #ifdef DEBUG
                printf("    color is known at index %d\n", i);
                #endif
                haveColor = i+1;
                break;
            }
        }
        r = runLength(rgb565, ptr, w * h);
        #ifdef DEBUG
        printf("    run length=%d\n", r);
        #endif
        if (haveColor) {
            litStatus = 0;
            // Se ce ne è uno solo invio un codice singolo
            switch (r) {
                case 1:
                    #ifdef DEBUG
                    printf("    emit: OUTPUT_IMMEDIATE\n");
                    #endif
                    codes[0]    ++;
                    read[0]     += r;
                    written[0]  += 1;
                    emit_byte(OUTPUT_IMMEDIATE + haveColor - 1);
                    break;
                case 2:
                    codes[1]    ++;
                    read[1]     += r;
                    written[1]  += 1;
                    emit_byte(OUTPUT_PAIR + haveColor - 1);
                    break;
                default:
                    // Ce ne sono almeno 3. Se sono 3 mando 0... se sono 255+3 = 258 mando 0xFF.
                    if (r < 258) {
                        #ifdef DEBUG
                        printf("    emit: OUTPUT_SHORT %d\n", (r-3) & 0xFF);
                        #endif
                        codes[2]++;
                        read[2]     += r;
                        written[2]  += 2;
                        emit_byte(OUTPUT_SHORT_CODE + haveColor - 1);
                        emit_byte((r -3) & 0xFF);
                    } else {
                        // Ce ne sono da 259 in su. Se sono 259 mando 0.
                        if (r > 0xFFFF + 259) {
                            #ifdef DEBUG
                            printf("    r truncated from %d to %d\n", r, 0xFFFF + 259);
                            #endif
                            r = 0xFFFF + 259;
                        }
                        #ifdef DEBUG
                        printf("    emit: OUTPUT_LONG %d\n", (r - 259) & 0xFFFF);
                        #endif
                        codes[3]++;
                        read[3]     += r;
                        written[3]  += 3;
                        emit_byte(OUTPUT_LONG_CODE + haveColor - 1);
                        emit_word((r - 259) & 0xFFFF);
                    }
                    break;
            }
        } else {
            int g0, g;
            int best = -1;
            // Non ho questo colore. I colori che già ho hanno determinati guadagni
            g0   = calcGain(rgb565, ptr, w * h, rgb565[ptr]);
            #ifdef DEBUG
            printf("    gain if added: %d\n", g0);
            #endif
            if (0 == g0) {
                #ifdef DEBUG
                printf("    emit: LITERAL_NO_STORE (R) colorValue\n");
                #endif
                // 3 byte
                if (r > 32) {
                    r = 32;
                }
                codes[4]++;
                read[4]     += r;
                written[4]  += 3;
 
                emit_byte(LITERAL_NO_STORE + (r -1));
                emit_word(rgb565[ptr]);
                if (1 == r) {
                    if (litStatus) {
                        litCount++;
                    } else {
                        litStatus = 0;
                    }
                }
            } else {
                for (i = 0; i < MAX_COLORS; i++) {
                    if (i && (colors[i] == colors[0])) {
                        #ifdef DEBUG
                        printf("    color %d is available, override eviction\n", i);
                        #endif
                        best = i;
                        break;
                    }
                    g = calcGain(rgb565, ptr, w * h, colors[i]);
                    #ifdef DEBUG
                    printf("    color %d gains %d\n", i, g);
                    #endif
                    if (g < g0) {
                        g0   = g;
                        best = i;
                        #ifdef DEBUG
                        printf("    evicting color %d\n", best);
                        #endif
                    }
                }
                #ifdef DEBUG
                printf("    can use color: %d\n", best);
                #endif
                // Lo metto in gain?
                if (best >= 0) {
                    // STORE_ENTRY|best, RUN_COUNT, COLOR_CODE
                    colors[best] = rgb565[ptr];
                    if (r > 256) {
                        r = 256;
                    }
                    #ifdef DEBUG
                    printf("    emit STORE_ENTRY %d %d\n", i, (r-1) & 0xFF);
                    #endif
                    codes[5]    ++;
                    read[5]     += r;
                    written[5]  += 4;
                    emit_byte(STORE_ENTRY + best);
                    emit_byte((r-1) & 0xFF);
                    litStatus = 0;
                } else {
                    // Non mi conviene.
                    if (r <= 32) {
                        // LITERAL|RUN_COUNT, COLOR_CODE
                        codes[4]++;
                        read[4]     += r;
                        written[4]  += 3;
 
                        emit_byte(LITERAL_NO_STORE | (r - 1));
                        #ifdef DEBUG
                        printf("    emit LITERAL_NO_STORE\n");
                        #endif
                        if (1 == r) {
                            if (litStatus) {
                                litCount++;
                            } else {
                                litStatus = 0;
                            }
                        }
                    } else {
                        litStatus = 0;
                        // MULTIPLE, RUN_COUNT, COLOR_CODE
                        emit_byte(MULTIPLE_NO_STORE);
                        if (r > (255+32)) {
                            r = 255+32;
                        }
                        emit_byte((r-32) & 0xFF);
                        #ifdef DEBUG
                        printf("    emit MULTIPLE_NO_STORE %d\n", (r-32) & 0xFF);
                        #endif
                        codes[6]++;
                        read[6]     += r;
                        written[6]  += 4;
                    }
                }
                #ifdef DEBUG
                printf("    emit colorValue %04x\n", rgb565[ptr]);
                #endif
                emit_word(rgb565[ptr]);
            }
        }
        ptr += r;
    }
    emit_byte(OUTPUT_EOF);
    fclose(outfile);

    printf("Immediate codes.... %d (%d -> %d)\n", codes[0], read[0]*2, written[0]);
    printf("Immediate pairs.... %d (%d -> %d)\n", codes[1], read[1]*2, written[1]);
    printf("Short runs......... %d (%d -> %d)\n", codes[2], read[2]*2, written[2]);
    printf("Long runs.......... %d (%d -> %d)\n", codes[3], read[3]*2, written[3]);
    printf("Literal codes...... %d (%d -> %d)\n", codes[4], read[4]*2, written[4]);
    printf("Storage codes...... %d (%d -> %d)\n", codes[5], read[5]*2, written[5]);
    printf("Multiple literal... %d (%d -> %d)\n", codes[6], read[6]*2, written[6]);
    printf("---------------------------------------\n");
    printf("Total..................(%d -> %d)\n",
                (read[0]+read[1]+read[2]+read[3]+read[4]+read[5]+read[6])*2,
                written[0]+written[1]+written[2]+written[3]+written[4]+written[5]+written[6]
    ); 
    printf("Repeated literals: %d\n", litCount);

    return EXIT_SUCCESS;
}
