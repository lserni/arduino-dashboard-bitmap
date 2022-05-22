#include <SPI.h>
#include <SD.h>
#include <MCUFRIEND_kbv.h>

// #define TIMING    1
// #define DEBUG     1

#define SD_CS   10

MCUFRIEND_kbv tft;
uint8_t   spi_save;

#define BUF_SIZE            64

#define MAX_COLORS          20
#define OUTPUT_EOF          0x00

#define OUTPUT_IMMEDIATE    0x20
#define OUTPUT_PAIR         0x40
#define OUTPUT_SHORT_CODE   0x60
#define STORE_ENTRY         0x80
#define OUTPUT_LONG_CODE    0xA0
#define LITERAL_NO_STORE    0xC0
#define MULTIPLE_NO_STORE   0xE0


#define read8(f)   ((uint8_t)f.read())
#define read16(f)  ((uint16_t)f.read() | ((uint16_t)f.read() << 8))
#define read32(f)  ((uint32_t)read16(f) | (((uint32_t)read16(f) << 16)))

// This function opens an APW file and displays it at the given coordinates.
  
int apwDraw(char *filename, int x, int y) {
  File     apwFile;
  uint16_t apwWidth, apwHeight, color, rle;
  int      w, h, row, col, bufptr;
  #ifdef TIMING
  uint32_t startTime = millis();
  #endif
  uint16_t  colors[MAX_COLORS], lcdbuffer[BUF_SIZE];
  uint8_t  op;
  boolean  first = true;
  
  if((x >= tft.width()) || (y >= tft.height())) {
    return 1;
  }
  
  // Open requested file on SD card
  SPCR = spi_save;
  if ((apwFile = SD.open(filename)) == NULL) {
    return 2;
  }
 
  // Parse BMP header
  if(read32(apwFile) != 0x534c4243 ) {
    apwFile.close();
    return 3;
  }
  apwWidth  = read16(apwFile);
  apwHeight = read16(apwFile);

  // Serial.print(F("Image size: "));
  // Serial.print(apwWidth);
  // Serial.print('x');
  // Serial.println(apwHeight);
 
  // Crop area to be loaded
  w = apwWidth;
  h = apwHeight;
  if((x+w-1) >= tft.width())  w = tft.width()  - x;
  if((y+h-1) >= tft.height()) h = tft.height() - y;

  SPCR = 0;
  // Set TFT address window to clipped image bounds
  tft.setAddrWindow(x, y, x+w-1, y+h-1);

  #ifdef DEBUG
  Serial.print(F("Window: "));
  Serial.print(w);
  Serial.print('x');
  Serial.print(h);
  Serial.print('@');
  Serial.print(x);
  Serial.print('x');
  Serial.println(y);
  #endif

  op = read8(apwFile);
  if (op > MAX_COLORS) {
    apwFile.close();
    return 4;
  }

  colors[0] = 0x0000;
  colors[1] = 0xFFFF;
  bufptr    = 0;
  
  for(;;) {
    // Read data.
    SPCR = spi_save;
    op   = read8(apwFile);
    if (OUTPUT_EOF == op) {
      break;
    }
    #ifdef DEBUG
    Serial.print(F("Got: "));
    Serial.println(op);
    #endif
    switch (op & 0xE0) {
      case OUTPUT_SHORT_CODE:
        rle   = read8(apwFile) + 3;
        color = colors[op & 0x1F];
        break;
      case OUTPUT_LONG_CODE:
        rle   = read16(apwFile) + 259;
        color = colors[op & 0x1F];
        break;
      case LITERAL_NO_STORE:
        rle   = (op & 0x1F) + 1;
        color = read16(apwFile);
        break;
      case MULTIPLE_NO_STORE:
        rle   = read8(apwFile) + 33;
        color = read16(apwFile);
        break;
      case STORE_ENTRY:
        rle   = read8(apwFile)+1;
        color = read16(apwFile);
        colors[op & 0x1F] = color;
        break;
      default:
        if (op >= 32 && op <= 52) {
          color = colors[op - 32];
          rle = 1;
          break;
        }
        if (op >= 64 && op <= 84) {
          color = colors[op - 64];
          rle = 2;
          break;
        }
        break;
    }
    // Can send. How many?
    #ifdef DEBUG
    Serial.print(F("C: "));
    Serial.print(color, HEX);
    Serial.print(F(" x "));
    Serial.println(rle);
    #endif

    // Send out 'rle' pixels.
    while (rle--) {
      lcdbuffer[bufptr++] = color;
      if (BUF_SIZE == bufptr) {
        SPCR   = 0;
        tft.pushColors(lcdbuffer, BUF_SIZE, first);
        bufptr = 0;
        first = false;
      }
    }
    #ifdef DEBUG
    Serial.println(F("OK"));
    #endif
  }
  if (bufptr) {
    SPCR = 0;
    tft.pushColors(lcdbuffer, bufptr, first);
  }
  #ifdef TIMING
  Serial.print(F("Loaded in "));
  Serial.print(millis() - startTime);
  Serial.println(" ms");
  #endif

  apwFile.close();
  return 0;
}

