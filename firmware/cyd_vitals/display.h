#pragma once

// Panel driver configuration and the drawing context shared by the *_render.h headers.

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <stdint.h>

#include "config.h"

class CydPanel : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9342 panel_;
  lgfx::Bus_SPI bus_;
  lgfx::Light_PWM light_;

 public:
  CydPanel() {
    {
      auto c = bus_.config();
      c.spi_host = HSPI_HOST;
      c.spi_mode = 0;
      c.freq_write = LCD_SPI_WRITE_HZ;
      c.freq_read = LCD_SPI_READ_HZ;
      c.spi_3wire = false;
      c.use_lock = true;
      c.dma_channel = SPI_DMA_CH_AUTO;
      c.pin_sclk = LCD_PIN_SCLK;
      c.pin_mosi = LCD_PIN_MOSI;
      c.pin_miso = LCD_PIN_MISO;
      c.pin_dc = LCD_PIN_DC;
      bus_.config(c);
      panel_.setBus(&bus_);
    }
    {
      auto c = panel_.config();
      c.pin_cs = LCD_PIN_CS;
      c.pin_rst = -1;
      c.pin_busy = -1;
      c.panel_width = LCD_WIDTH;
      c.panel_height = LCD_HEIGHT;
      c.offset_x = 0;
      c.offset_y = 0;
      c.offset_rotation = 0;
      c.readable = true;
      c.invert = false;
      c.rgb_order = true;   // BGR panel: without this red and blue are swapped
      c.dlen_16bit = false;
      c.bus_shared = true;
      panel_.config(c);
    }
    {
      auto c = light_.config();
      c.pin_bl = LCD_PIN_BACKLIGHT;
      c.invert = false;
      c.freq = LCD_BACKLIGHT_PWM_HZ;
      c.pwm_channel = LCD_BACKLIGHT_PWM_CHANNEL;
      light_.config(c);
      panel_.setLight(&light_);
    }
    setPanel(&panel_);
  }
};

// Screen geometry: a header strip over a two-by-two grid of cells.
struct Layout {
  int w = LCD_WIDTH;
  int h = LCD_HEIGHT;
  int headerH = 28;
  int colW = LCD_WIDTH / 2;
  int rowH = (LCD_HEIGHT - 28) / 2;
};

inline Layout layoutFor(int w, int h) {
  Layout l;
  l.w = w;
  l.h = h;
  l.colW = w / 2;
  l.rowH = (h - l.headerH) / 2;
  return l;
}

// ---- colours ------------------------------------------------------------------------------------
//
// What this panel shows for a written value is its 16-bit complement with red and blue exchanged:
// on the device TFT_BLACK displays as white, TFT_RED as yellow and TFT_YELLOW as red. Every colour
// below is written as the panel needs it. The PANEL_* names say what actually appears; the other
// constants were tuned by eye on the device.

constexpr uint16_t PANEL_WHITE = TFT_BLACK;
constexpr uint16_t PANEL_BLACK = TFT_WHITE;
constexpr uint16_t PANEL_RED = TFT_YELLOW;
constexpr uint16_t PANEL_YELLOW = TFT_RED;

// Value colours, the same day and night.
constexpr uint16_t COLOUR_HEART = 0x6E6C;
constexpr uint16_t COLOUR_OXYGEN = 0x74FF;
constexpr uint16_t COLOUR_SKIN = 0xFE79;
constexpr uint16_t COLOUR_STATUS_WARN = 0xEB44;
// A rate that came from the beat interval, so it never looks like an ordinary reading.
constexpr uint16_t COLOUR_CORRECTED = TFT_ORANGE;

// Chrome colours. The daytime set reads as a white background with dark text on the device. At
// night each one is replaced by its complement, which flips the panel to a black background with
// light text; the value colours above are deliberately not flipped.
struct Palette {
  uint16_t bg;
  uint16_t grey;
  uint16_t dim;
  uint16_t line;
  uint16_t grid;
};

constexpr Palette DAY_PALETTE{PANEL_WHITE, 0x9CD3, 0x52AA, 0x2965, 0x1082};

constexpr uint16_t invert565(uint16_t c) { return static_cast<uint16_t>(~c); }

inline Palette paletteFor(bool night) {
  if (!night) return DAY_PALETTE;
  return Palette{invert565(DAY_PALETTE.bg), invert565(DAY_PALETTE.grey), invert565(DAY_PALETTE.dim),
                 invert565(DAY_PALETTE.line), invert565(DAY_PALETTE.grid)};
}

// Halves every channel of a 565 colour.
constexpr uint16_t dim565(uint16_t c) { return static_cast<uint16_t>(c & 0x7BEF); }

// Everything a render function needs: the panel, its geometry and the current chrome palette.
struct Screen {
  lgfx::LGFX_Device& tft;
  Layout layout;
  Palette palette;
};
