/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *  Copyright (C) 2011-2017 - Higor Euripedes
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <string.h>

#include <SDL/SDL.h>
#include <SDL/SDL_video.h>

#include <retro_assert.h>
#include <gfx/video_frame.h>
#include <retro_assert.h>
#include <string/stdstring.h>
#include <encodings/utf.h>
#include <features/features_cpu.h>

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

#ifdef HAVE_MENU
#include "../../menu/menu_driver.h"
#endif

#include "../../dingux/dingux_utils.h"

#include "../../verbosity.h"
#include "../../gfx/drivers_font_renderer/bitmap.h"
#include "../../configuration.h"
#include "../../retroarch.h"
#if defined(DINGUX_BETA)
#include "../../driver.h"
#endif

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define SDL_RS90_WIDTH 240
#define SDL_RS90_HEIGHT 160
// pitch = SDL_RS90_WIDTH * sizeof(uint16_t)
// define SDL_RS90_PITCH SDL_RS90_WIDTH * 2

#define SDL_DINGUX_NUM_FONT_GLYPHS 256

typedef struct sdl_rs90_video
{
   retro_time_t last_frame_time;
   retro_time_t ff_frame_time_min;
   SDL_Surface *screen;
   bitmapfont_lut_t *osd_font;
   unsigned frame_width;
   unsigned frame_height;
   unsigned frame_padding_x;
   unsigned frame_padding_y;
#if defined(DINGUX_BETA)
   enum dingux_refresh_rate refresh_rate;
#endif
   uint32_t font_colour32;
   uint16_t font_colour16;
   uint16_t menu_texture[SDL_RS90_WIDTH * SDL_RS90_HEIGHT];
   bool rgb32;
   bool vsync;
   bool keep_aspect;
   bool scale_integer;
   bool menu_active;
   bool was_in_menu;
   bool quitting;
   bool mode_valid;
   unsigned content_width;
   unsigned content_height;
   unsigned content_pitch;
   unsigned* scaling_table;
} sdl_rs90_video_t;

static void sdl_rs90_init_font_color(sdl_rs90_video_t *vid)
{
   settings_t *settings = config_get_ptr();
   uint32_t red         = 0xFF;
   uint32_t green       = 0xFF;
   uint32_t blue        = 0xFF;

   if (settings)
   {
      red   = (uint32_t)((settings->floats.video_msg_color_r * 255.0f) + 0.5f) & 0xFF;
      green = (uint32_t)((settings->floats.video_msg_color_g * 255.0f) + 0.5f) & 0xFF;
      blue  = (uint32_t)((settings->floats.video_msg_color_b * 255.0f) + 0.5f) & 0xFF;
   }

   /* Convert to XRGB8888 */
   vid->font_colour32 = (red << 16) | (green << 8) | blue;

   /* Convert to RGB565 */
   red   = red   >> 3;
   green = green >> 3;
   blue  = blue  >> 3;

   vid->font_colour16 = (red << 11) | (green << 6) | blue;
}

static void sdl_rs90_blit_text16(
      sdl_rs90_video_t *vid,
      unsigned x, unsigned y,
      const char *str)
{
   /* Note: Cannot draw text in padding region
    * (padding region is never cleared, so
    * any text pixels would remain as garbage) */
   uint16_t *screen_buf         = (uint16_t*)vid->screen->pixels;
   bool **font_lut              = vid->osd_font->lut;
   /* 16 bit - divide pitch by 2 */
   uint16_t screen_stride       = (uint16_t)(vid->screen->pitch >> 1);
   uint16_t screen_width        = vid->screen->w;
   uint16_t screen_height       = vid->screen->h;
   unsigned x_pos               = x + vid->frame_padding_x;
   unsigned y_pos               = (y > (screen_height >> 1)) ?
         (y - vid->frame_padding_y) : (y + vid->frame_padding_y);
   uint16_t shadow_color_buf[2] = {0};
   uint16_t color_buf[2];

   color_buf[0] = vid->font_colour16;
   color_buf[1] = 0;

   /* Check for out of bounds y coordinates */
   if (y_pos + FONT_HEIGHT + 1 >=
         screen_height - vid->frame_padding_y)
      return;

   while (!string_is_empty(str))
   {
      /* Check for out of bounds x coordinates */
      if (x_pos + FONT_WIDTH_STRIDE + 1 >=
            screen_width - vid->frame_padding_x)
         return;

      /* Deal with spaces first, for efficiency */
      if (*str == ' ')
         str++;
      else
      {
         uint16_t i, j;
         bool *symbol_lut;
         uint32_t symbol = utf8_walk(&str);

         /* Stupid hack: 'oe' ligatures are not really
          * standard extended ASCII, so we have to waste
          * CPU cycles performing a conversion from the
          * unicode values... */
         if (symbol == 339) /* Latin small ligature oe */
            symbol = 156;
         if (symbol == 338) /* Latin capital ligature oe */
            symbol = 140;

         if (symbol >= SDL_DINGUX_NUM_FONT_GLYPHS)
            continue;

         symbol_lut = font_lut[symbol];

         for (j = 0; j < FONT_HEIGHT; j++)
         {
            uint32_t buff_offset = ((y_pos + j) * screen_stride) + x_pos;

            for (i = 0; i < FONT_WIDTH; i++)
            {
               if (*(symbol_lut + i + (j * FONT_WIDTH)))
               {
                  uint16_t *screen_buf_ptr = screen_buf + buff_offset + i;

                  /* Text pixel + right shadow */
                  memcpy(screen_buf_ptr, color_buf, sizeof(uint16_t));

                  /* Bottom shadow */
                  screen_buf_ptr += screen_stride;
                  memcpy(screen_buf_ptr, shadow_color_buf, sizeof(uint16_t));
               }
            }
         }
      }

      x_pos += FONT_WIDTH_STRIDE;
   }
}

static void sdl_rs90_blit_text32(
      sdl_rs90_video_t *vid,
      unsigned x, unsigned y,
      const char *str)
{
   /* Note: Cannot draw text in padding region
    * (padding region is never cleared, so
    * any text pixels would remain as garbage) */
   uint32_t *screen_buf         = (uint32_t*)vid->screen->pixels;
   bool **font_lut              = vid->osd_font->lut;
   /* 32 bit - divide pitch by 4 */
   uint32_t screen_stride       = (uint32_t)(vid->screen->pitch >> 2);
   uint32_t screen_width        = vid->screen->w;
   uint32_t screen_height       = vid->screen->h;
   unsigned x_pos               = x + vid->frame_padding_x;
   unsigned y_pos               = (y > (screen_height >> 1)) ?
         (y - vid->frame_padding_y) : (y + vid->frame_padding_y);
   uint32_t shadow_color_buf[2] = {0};
   uint32_t color_buf[2];

   color_buf[0] = vid->font_colour32;
   color_buf[1] = 0;

   /* Check for out of bounds y coordinates */
   if (y_pos + FONT_HEIGHT + 1 >=
         screen_height - vid->frame_padding_y)
      return;

   while (!string_is_empty(str))
   {
      /* Check for out of bounds x coordinates */
      if (x_pos + FONT_WIDTH_STRIDE + 1 >=
            screen_width - vid->frame_padding_x)
         return;

      /* Deal with spaces first, for efficiency */
      if (*str == ' ')
         str++;
      else
      {
         uint32_t i, j;
         bool *symbol_lut;
         uint32_t symbol = utf8_walk(&str);

         /* Stupid hack: 'oe' ligatures are not really
          * standard extended ASCII, so we have to waste
          * CPU cycles performing a conversion from the
          * unicode values... */
         if (symbol == 339) /* Latin small ligature oe */
            symbol = 156;
         if (symbol == 338) /* Latin capital ligature oe */
            symbol = 140;

         if (symbol >= SDL_DINGUX_NUM_FONT_GLYPHS)
            continue;

         symbol_lut = font_lut[symbol];

         for (j = 0; j < FONT_HEIGHT; j++)
         {
            uint32_t buff_offset = ((y_pos + j) * screen_stride) + x_pos;

            for (i = 0; i < FONT_WIDTH; i++)
            {
               if (*(symbol_lut + i + (j * FONT_WIDTH)))
               {
                  uint32_t *screen_buf_ptr = screen_buf + buff_offset + i;

                  /* Text pixel + right shadow */
                  memcpy(screen_buf_ptr, color_buf, sizeof(uint32_t));

                  /* Bottom shadow */
                  screen_buf_ptr += screen_stride;
                  memcpy(screen_buf_ptr, shadow_color_buf, sizeof(uint32_t));
               }
            }
         }
      }

      x_pos += FONT_WIDTH_STRIDE;
   }
}

static void sdl_rs90_blit_video_mode_error_msg(sdl_rs90_video_t *vid)
{
   const char *error_msg = msg_hash_to_str(MSG_UNSUPPORTED_VIDEO_MODE);
   char display_mode[64];

   display_mode[0] = '\0';

   /* Zero out pixel buffer */
   memset(vid->screen->pixels, 0,
         vid->screen->pitch * vid->screen->h);

   /* Generate display mode string */
   snprintf(display_mode, sizeof(display_mode), "> %ux%u, %s",
         vid->frame_width, vid->frame_height,
         vid->rgb32 ? "XRGB8888" : "RGB565");

   /* Print error message */
   if (vid->rgb32)
   {
      sdl_rs90_blit_text32(vid,
            FONT_WIDTH_STRIDE, FONT_WIDTH_STRIDE,
            error_msg);

      sdl_rs90_blit_text32(vid,
            FONT_WIDTH_STRIDE, FONT_WIDTH_STRIDE + FONT_HEIGHT_STRIDE,
            display_mode);
   }
   else
   {
      sdl_rs90_blit_text16(vid,
            FONT_WIDTH_STRIDE, FONT_WIDTH_STRIDE,
            error_msg);

      sdl_rs90_blit_text16(vid,
            FONT_WIDTH_STRIDE, FONT_WIDTH_STRIDE + FONT_HEIGHT_STRIDE,
            display_mode);
   }
}

static void sdl_rs90_gfx_free(void *data)
{
   sdl_rs90_video_t *vid = (sdl_rs90_video_t*)data;

   if (!vid)
      return;

   if (vid->osd_font)
      bitmapfont_free_lut(vid->osd_font);

   free(vid->scaling_table);
   free(vid);
}

static void sdl_rs90_input_driver_init(
      const char *input_driver_name, const char *joypad_driver_name,
      input_driver_t **input, void **input_data)
{
   /* Sanity check */
   if (!input || !input_data)
      return;

   *input      = NULL;
   *input_data = NULL;

   /* If input driver name is empty, cannot
    * initialise anything... */
   if (string_is_empty(input_driver_name))
      return;

   if (string_is_equal(input_driver_name, "sdl_dingux"))
   {
      *input_data = input_driver_init_wrap(&input_sdl_dingux,
            joypad_driver_name);

      if (*input_data)
         *input = &input_sdl_dingux;

      return;
   }

#if defined(HAVE_SDL) || defined(HAVE_SDL2)
   if (string_is_equal(input_driver_name, "sdl"))
   {
      *input_data = input_driver_init_wrap(&input_sdl,
            joypad_driver_name);

      if (*input_data)
         *input = &input_sdl;

      return;
   }
#endif

#if defined(HAVE_UDEV)
   if (string_is_equal(input_driver_name, "udev"))
   {
      *input_data = input_driver_init_wrap(&input_udev,
            joypad_driver_name);

      if (*input_data)
         *input = &input_udev;

      return;
   }
#endif

#if defined(__linux__)
   if (string_is_equal(input_driver_name, "linuxraw"))
   {
      *input_data = input_driver_init_wrap(&input_linuxraw,
            joypad_driver_name);

      if (*input_data)
         *input = &input_linuxraw;

      return;
   }
#endif
}

static void *sdl_rs90_gfx_init(const video_info_t *video,
      input_driver_t **input, void **input_data)
{
   sdl_rs90_video_t *vid                       = NULL;
   uint32_t sdl_subsystem_flags                  = SDL_WasInit(0);
   settings_t *settings                          = config_get_ptr();
#if defined(DINGUX_BETA)
   enum dingux_refresh_rate current_refresh_rate = DINGUX_REFRESH_RATE_60HZ;
   enum dingux_refresh_rate target_refresh_rate  = (enum dingux_refresh_rate)
         settings->uints.video_dingux_refresh_rate;
   bool refresh_rate_valid                       = false;
   float hw_refresh_rate                         = 0.0f;
#endif
   const char *input_driver_name                 = settings->arrays.input_driver;
   const char *joypad_driver_name                = settings->arrays.input_joypad_driver;
   uint32_t surface_flags                        = (video->vsync) ?
         (SDL_HWSURFACE | SDL_TRIPLEBUF | SDL_FULLSCREEN) :
         (SDL_HWSURFACE | SDL_FULLSCREEN);

   /* Initialise graphics subsystem, if required */
   if (sdl_subsystem_flags == 0)
   {
      if (SDL_Init(SDL_INIT_VIDEO) < 0)
         return NULL;
   }
   else if ((sdl_subsystem_flags & SDL_INIT_VIDEO) == 0)
   {
      if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0)
         return NULL;
   }

   vid = (sdl_rs90_video_t*)calloc(1, sizeof(*vid));
   if (!vid)
      return NULL;

#if defined(DINGUX_BETA)
   /* Get current refresh rate */
   refresh_rate_valid = dingux_get_video_refresh_rate(&current_refresh_rate);

   /* Check if refresh rate needs to be updated */
   if (!refresh_rate_valid ||
       (current_refresh_rate != target_refresh_rate))
      hw_refresh_rate = dingux_set_video_refresh_rate(target_refresh_rate);
   else
   {
      /* Correct refresh rate is already set,
       * just convert to float */
      switch (current_refresh_rate)
      {
         case DINGUX_REFRESH_RATE_50HZ:
            hw_refresh_rate = 50.0f;
            break;
         default:
            hw_refresh_rate = 60.0f;
            break;
      }
   }

   if (hw_refresh_rate == 0.0f)
   {
      RARCH_ERR("[SDL1]: Failed to set video refresh rate\n");
      goto error;
   }

   vid->refresh_rate = target_refresh_rate;
   switch (target_refresh_rate)
   {
      case DINGUX_REFRESH_RATE_50HZ:
         vid->ff_frame_time_min = 20000;
         break;
      default:
         vid->ff_frame_time_min = 16667;
         break;
   }

   driver_ctl(RARCH_DRIVER_CTL_SET_REFRESH_RATE, &hw_refresh_rate);
#else
   vid->ff_frame_time_min = 16667;
#endif

   vid->screen = SDL_SetVideoMode(
      SDL_RS90_WIDTH, SDL_RS90_HEIGHT,
      video->rgb32 ? 32 : 16,
      surface_flags
   );

   if (!vid->screen)
   {
      RARCH_ERR("[SDL1]: Failed to init SDL surface: %s\n", SDL_GetError());
      goto error;
   }

   vid->frame_width     = SDL_RS90_WIDTH;
   vid->frame_height    = SDL_RS90_HEIGHT;
   vid->rgb32           = video->rgb32;
   vid->vsync           = video->vsync;
   vid->keep_aspect     = settings->bools.video_dingux_ipu_keep_aspect;
   vid->scale_integer   = settings->bools.video_scale_integer;
   vid->menu_active     = false;
   vid->was_in_menu     = false;
   vid->quitting        = false;
   vid->mode_valid      = true;
   vid->last_frame_time = 0;
   vid->content_width   = 0;
   vid->content_height  = 0;
   vid->content_pitch   = 0;
   vid->scaling_table = calloc(0, sizeof(unsigned));

   SDL_ShowCursor(SDL_DISABLE);

   sdl_rs90_input_driver_init(input_driver_name,
         joypad_driver_name, input, input_data);

   /* Initialise OSD font */
   sdl_rs90_init_font_color(vid);

   vid->osd_font = bitmapfont_get_lut();

   if (!vid->osd_font ||
       vid->osd_font->glyph_max <
            (SDL_DINGUX_NUM_FONT_GLYPHS - 1))
   {
      RARCH_ERR("[SDL1]: Failed to init OSD font\n");
      goto error;
   }

   return vid;

error:
   sdl_rs90_gfx_free(vid);
   return NULL;
}

static void sdl_rs90_set_output(
      sdl_rs90_video_t* vid,
      unsigned width, unsigned height, bool rgb32)
{
   uint32_t surface_flags = (vid->vsync) ?
         (SDL_HWSURFACE | SDL_TRIPLEBUF | SDL_FULLSCREEN) :
         (SDL_HWSURFACE | SDL_FULLSCREEN);

   /* Cache set parameters */
   vid->frame_width  = width;
   vid->frame_height = height;

   /* Reset frame padding */
   vid->frame_padding_x = 0;
   vid->frame_padding_y = 0;

   /* Attempt to change video mode */
   vid->screen = SDL_SetVideoMode(
         SDL_RS90_WIDTH, SDL_RS90_HEIGHT,
         rgb32 ? 32 : 16,
         surface_flags);

   /* Check whether selected display mode is valid */
   if (unlikely(!vid->screen))
   {
      RARCH_ERR("[SDL1]: Failed to init SDL surface: %s\n", SDL_GetError());
      vid->mode_valid = false;
   }
   else
   {
      /* Determine whether frame padding is required */
      if ((SDL_RS90_WIDTH  != width) ||
          (SDL_RS90_HEIGHT != height))
      {
         // No negative paddings!
         vid->frame_padding_x = width < SDL_RS90_WIDTH ? (SDL_RS90_WIDTH  - width) >> 1 : 0;
         vid->frame_padding_y = height < SDL_RS90_HEIGHT ? (SDL_RS90_HEIGHT - height) >> 1 : 0;

         /* To prevent garbage pixels in the padding
          * region, must zero out pixel buffer */
         if (SDL_MUSTLOCK(vid->screen))
            SDL_LockSurface(vid->screen);

         memset(vid->screen->pixels, 0,
               vid->screen->pitch * vid->screen->h);

         if (SDL_MUSTLOCK(vid->screen))
            SDL_UnlockSurface(vid->screen);
      }

      vid->mode_valid = true;
   }
}


static void sdl_rs90_generate_scaling_table(sdl_rs90_video_t *vid,
   unsigned width, unsigned height, unsigned src_pitch)
{
   free(vid->scaling_table);

   vid->content_width = width;
   vid->content_height = height;
   vid->content_pitch = src_pitch;

   unsigned* scaling_table = calloc(SDL_RS90_WIDTH * SDL_RS90_HEIGHT, sizeof(unsigned));

   vid->scaling_table = scaling_table;
   /* I'm not sure which of these need to be uin32_t and which can be 16
   bit for performance */
   /* approximate nearest neighbor scale with integer math */
   uint32_t x_step = (uint32_t)((width << 16) / SDL_RS90_WIDTH); // + 1;
   uint32_t y_step = (uint32_t)((height << 16) / SDL_RS90_HEIGHT); //  + 1;

   uint32_t row;
   uint32_t col;
   uint32_t i = 0;
   unsigned idx;

   /* 16 bit - divide pitch by 2 */
   uint16_t in_stride  = (uint16_t)(src_pitch >> 1);

   for (row = 0; row < SDL_RS90_HEIGHT; row++) {
      idx = ((row * y_step) >> 16) * in_stride;
      for (col = 0; col < SDL_RS90_WIDTH; col++) {
         scaling_table[i++] = idx + ((x_step * col) >> 16);
      }
   }
}

// stretches
// static void sdl_rs90_blit_frame16_nearest_neighbor(sdl_rs90_video_t *vid,
//       uint16_t* src, unsigned width, unsigned height,
//       unsigned src_pitch)
// {
//    /* I'm not sure which of these need to be uin32_t and which can be 16
//    bit for performance */
//    /* approximate nearest neighbor scale with integer math */
//    uint32_t x_step = (uint32_t)((width << 16) / SDL_RS90_WIDTH); // + 1;
//    uint32_t y_step = (uint32_t)((height << 16) / SDL_RS90_HEIGHT); //  + 1;
//
//    uint32_t row;
//    uint32_t col;
//    uint32_t idx;
//
//    uint16_t *in_ptr;
//    uint16_t *out_ptr;
//    /* 16 bit - divide pitch by 2 */
//    uint16_t in_stride  = (uint16_t)(src_pitch >> 1);
//    uint16_t out_stride = (uint16_t)(vid->screen->pitch >> 1);
//
//    uint16_t* table = calloc(SDL_RS90_WIDTH * SDL_RS90_HEIGHT, sizeof(uint16_t));
//
//    for (row = 0; row < SDL_RS90_HEIGHT; row++) {
//       idx = ((row * y_step) >> 16) * in_stride;
//       for (col = 0, pos=row; col < SDL_RS90_WIDTH; col++, pos++) {
//          table[pos] = idx + ((x_step * col) >> 16);
//       }
//    }
//
//    free(table);
//
// }

static void sdl_rs90_blit_frame16_scale_precomputed(sdl_rs90_video_t *vid, uint16_t* src)
{

   uint32_t row;
   uint32_t col;

   size_t i = 0;

   uint16_t *out_ptr;
   /* 16 bit - divide pitch by 2 */
   uint16_t out_stride = (uint16_t)(vid->screen->pitch >> 1);

   uint16_t* scaling_table = vid->scaling_table;

   for (row = 0; row < SDL_RS90_HEIGHT; row++) {
      out_ptr = (uint16_t*)(vid->screen->pixels) + out_stride * row;
      for (col = 0; col < SDL_RS90_WIDTH; col++) {
         // Why the extra shift??? / ???? sizeof(uint16_t)
        *out_ptr = src[scaling_table[i++]];
        out_ptr++; // ???? sizeof(uint16_t)
      }
   }

}


static void sdl_rs90_blit_frame16(sdl_rs90_video_t *vid,
      uint16_t* src, unsigned width, unsigned height,
      unsigned src_pitch)
{
   unsigned dst_pitch = vid->screen->pitch;
   uint16_t *in_ptr   = src;
   uint16_t *out_ptr  = (uint16_t*)(vid->screen->pixels);

   /* Just copy the upper left hand rectangle for now if the
   screen sizes don't match up */
   /* Might be slightly nicer to do a center crop */
   /* Scaling should be done in shaders, I guess */
   unsigned width_trunc = width > SDL_RS90_WIDTH ? SDL_RS90_WIDTH : width;
   unsigned height_trunc = height > SDL_RS90_HEIGHT ? SDL_RS90_HEIGHT : height;

   /* If source and destination buffers have the
    * same pitch, perform fast copy of raw pixel data */
   if (src_pitch == dst_pitch && height == height_trunc)
      memcpy(out_ptr, in_ptr, src_pitch * height);
   else
   {

      if (vid->content_width != width || vid->content_height != height || vid->content_pitch != src_pitch) {
         sdl_rs90_generate_scaling_table(
            vid, width, height, src_pitch
         );
      }

      // always use nearest neighbor scale for now
      sdl_rs90_blit_frame16_scale_precomputed(
         vid, src
      );

      // /* Otherwise copy pixel data line-by-line */
      //
      // /* 16 bit - divide pitch by 2 */
      // uint16_t in_stride  = (uint16_t)(src_pitch >> 1);
      // uint16_t out_stride = (uint16_t)(dst_pitch >> 1);
      // size_t y;
      //
      //
      // /* Might work, don't know. Need to test. */
      // if (width >= SDL_RS90_WIDTH) {
      //    // Crop left 1/2 excess width
      //    in_ptr += (width - SDL_RS90_WIDTH) >> 1;
      // } else {
      //    // Pad left 1/2 remaining width
      //    out_ptr += (SDL_RS90_WIDTH - width) >> 1;
      // }
      //
      // if (height >= SDL_RS90_HEIGHT) {
      //    // Crop top 1/2 excess height
      //    in_ptr += in_stride * ((height - SDL_RS90_HEIGHT) >> 1);
      // } else {
      //    // Pad top 1/2 remaining height
      //    out_ptr += out_stride * ((SDL_RS90_HEIGHT - height) >> 1);
      // }
      //
      // for (y = 0; y < height_trunc; y++)
      // {
      //    memcpy(out_ptr, in_ptr, width_trunc * sizeof(uint16_t));
      //    in_ptr  += in_stride;
      //    out_ptr += out_stride;
      // }
   }
}

static void sdl_rs90_blit_frame32(sdl_rs90_video_t *vid,
      uint32_t* src, unsigned width, unsigned height,
      unsigned src_pitch)
{
   unsigned dst_pitch = vid->screen->pitch;
   uint32_t *in_ptr   = src;
   uint32_t *out_ptr  = (uint32_t*)(vid->screen->pixels +
         (vid->frame_padding_y * dst_pitch));
   /* Just copy the upper left hand rectangle for now if the
   screen sizes don't match up */
   /* Might be slightly nicer to do a center crop */
   /* Scaling should be done in shaders, I guess */
   unsigned width_trunc = width > SDL_RS90_WIDTH ? SDL_RS90_WIDTH : width;
   unsigned height_trunc = height > SDL_RS90_HEIGHT ? SDL_RS90_HEIGHT : height;

   /* If source and destination buffers have the
    * same pitch, perform fast copy of raw pixel data */
   if (src_pitch == dst_pitch && height == height_trunc)
      memcpy(out_ptr, in_ptr, src_pitch * height);
   else
   {
      /* Otherwise copy pixel data line-by-line */

      /* 32 bit - divide pitch by 4 */
      uint32_t in_stride  = (uint32_t)(src_pitch >> 2);
      uint32_t out_stride = (uint32_t)(dst_pitch >> 2);
      size_t y;

      /* If SDL surface has horizontal padding,
       * shift output image to the right */
      out_ptr += vid->frame_padding_x;

      for (y = 0; y < height_trunc; y++)
      {
         memcpy(out_ptr, in_ptr, width_trunc * sizeof(uint32_t));
         in_ptr  += in_stride;
         out_ptr += out_stride;
      }
   }
}

static bool sdl_rs90_gfx_frame(void *data, const void *frame,
      unsigned width, unsigned height, uint64_t frame_count,
      unsigned pitch, const char *msg, video_frame_info_t *video_info)
{
   sdl_rs90_video_t* vid = (sdl_rs90_video_t*)data;

   if (unlikely(!vid))
      return true;

   /* If fast forward is currently active, we may
    * push frames at an 'unlimited' rate. Since the
    * display has a fixed refresh rate of 60 Hz (or
    * potentially 50 Hz on OpenDingux Beta), this
    * represents wasted effort. We therefore drop any
    * 'excess' frames in this case.
    * (Note that we *only* do this when fast forwarding.
    * Attempting this trick while running content normally
    * will cause bad frame pacing) */
   if (unlikely(video_info->input_driver_nonblock_state))
   {
      retro_time_t current_time = cpu_features_get_time_usec();

      if ((current_time - vid->last_frame_time) <
            vid->ff_frame_time_min)
         return true;

      vid->last_frame_time = current_time;
   }

#ifdef HAVE_MENU
   menu_driver_frame(video_info->menu_is_alive, video_info);
#endif

   if (likely(!vid->menu_active))
   {
      /* Update video mode if we were in the menu on
       * the previous frame, or width/height have changed */
      if (unlikely(
            vid->was_in_menu ||
            (vid->frame_width  != width) ||
            (vid->frame_height != height)))
         sdl_rs90_set_output(vid, width, height, vid->rgb32);

      /* Must always lock SDL surface before
       * manipulating raw pixel buffer */
      if (SDL_MUSTLOCK(vid->screen))
         SDL_LockSurface(vid->screen);

      if (likely(vid->mode_valid))
      {
         if (likely(frame))
         {
            /* Blit frame to SDL surface */
            if (vid->rgb32)
               sdl_rs90_blit_frame32(vid, (uint32_t*)frame,
                     width, height, pitch);
            else
               sdl_rs90_blit_frame16(vid, (uint16_t*)frame,
                     width, height, pitch);
         }
      }
      /* If current display mode is invalid,
       * just display an error message */
      else
         sdl_rs90_blit_video_mode_error_msg(vid);

      vid->was_in_menu = false;
   }
   else
   {
      /* If this is the first frame that the menu
       * is active, update video mode */
      if (!vid->was_in_menu)
      {
         sdl_rs90_set_output(vid,
               SDL_RS90_WIDTH, SDL_RS90_HEIGHT, false);

         vid->was_in_menu = true;
      }

      if (SDL_MUSTLOCK(vid->screen))
         SDL_LockSurface(vid->screen);

      /* Blit menu texture to SDL surface */
      sdl_rs90_blit_frame16(vid, vid->menu_texture,
            SDL_RS90_WIDTH, SDL_RS90_HEIGHT,
            SDL_RS90_WIDTH * sizeof(uint16_t));
   }

   /* Print OSD text, if required */
   if (msg)
   {
      /* If menu is active, colour depth is overridden
       * to 16 bit */
      if (vid->rgb32 && !vid->menu_active)
         sdl_rs90_blit_text32(vid, FONT_WIDTH_STRIDE,
               vid->screen->h - (FONT_HEIGHT + FONT_WIDTH_STRIDE), msg);
      else
         sdl_rs90_blit_text16(vid, FONT_WIDTH_STRIDE,
               vid->screen->h - (FONT_HEIGHT + FONT_WIDTH_STRIDE), msg);
   }

   /* Pixel manipulation complete - unlock
    * SDL surface */
   if (SDL_MUSTLOCK(vid->screen))
      SDL_UnlockSurface(vid->screen);

   SDL_Flip(vid->screen);

   return true;
}

static void sdl_rs90_set_texture_enable(void *data, bool state, bool full_screen)
{
   sdl_rs90_video_t *vid = (sdl_rs90_video_t*)data;

   if (unlikely(!vid))
      return;

   vid->menu_active = state;
}

static void sdl_rs90_set_texture_frame(void *data, const void *frame, bool rgb32,
      unsigned width, unsigned height, float alpha)
{
   sdl_rs90_video_t *vid = (sdl_rs90_video_t*)data;

   if (unlikely(
         !vid ||
         rgb32 ||
         (width > SDL_RS90_WIDTH) ||
         (height > SDL_RS90_HEIGHT)))
      return;

   memcpy(vid->menu_texture, frame, width * height * sizeof(uint16_t));
}

static void sdl_rs90_gfx_set_nonblock_state(void *data, bool toggle,
      bool adaptive_vsync_enabled, unsigned swap_interval)
{
   sdl_rs90_video_t *vid = (sdl_rs90_video_t*)data;
   bool vsync              = !toggle;

   if (unlikely(!vid))
      return;

   /* Check whether vsync status has changed */
   if (vid->vsync != vsync)
   {
      unsigned current_width  = vid->frame_width;
      unsigned current_height = vid->frame_height;
      vid->vsync              = vsync;

      /* Update video mode */

      /* I'm not sure if this is the case on the rs90, but it was on dingux
      /* Note that a tedious workaround is required...
       * - Calling SDL_SetVideoMode() with the currently
       *   set width, height and pixel format can randomly
       *   become a noop even if the surface flags change.
       * - Since all we are doing here is changing the VSYNC
       *   parameter (which just modifies surface flags), this
       *   means the VSYNC toggle may not be registered...
       * - This is a huge problem when enabling fast forward,
       *   because VSYNC ON effectively limits maximum frame
       *   rate - if we push frames too rapidly, the OS chokes
       *   and the display freezes.
       * We have to ensure that the VSYNC state change is
       * applied in all cases. We can only do this by forcing
       * a 'real' video mode update, which means adjusting the
       * video resolution. We therefore end up calling
       * sdl_rs90_set_output() *twice*, setting the dimensions
       * to an arbitrary value before restoring the actual
       * desired width/height */
      sdl_rs90_set_output(vid,
            current_width,
            (current_height > 4) ? (current_height - 2) : 16,
            vid->rgb32);

      sdl_rs90_set_output(vid,
            current_width, current_height, vid->rgb32);
   }
}

static void sdl_rs90_gfx_check_window(sdl_rs90_video_t *vid)
{
   SDL_Event event;

   SDL_PumpEvents();
   while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_QUITMASK))
   {
      if (event.type != SDL_QUIT)
         continue;

      vid->quitting = true;
      break;
   }
}

static bool sdl_rs90_gfx_alive(void *data)
{
   sdl_rs90_video_t *vid = (sdl_rs90_video_t*)data;

   if (unlikely(!vid))
      return false;

   sdl_rs90_gfx_check_window(vid);
   return !vid->quitting;
}

static bool sdl_rs90_gfx_focus(void *data)
{
   return true;
}

static bool sdl_rs90_gfx_suppress_screensaver(void *data, bool enable)
{
   return false;
}

static bool sdl_rs90_gfx_has_windowed(void *data)
{
   return false;
}

static void sdl_rs90_gfx_viewport_info(void *data, struct video_viewport *vp)
{
   sdl_rs90_video_t *vid = (sdl_rs90_video_t*)data;

   if (unlikely(!vid))
      return;

   vp->x      = 0;
   vp->y      = 0;
   vp->width  = vp->full_width  = vid->frame_width;
   vp->height = vp->full_height = vid->frame_height;
}

static float sdl_rs90_get_refresh_rate(void *data)
{
#if defined(DINGUX_BETA)
   sdl_rs90_video_t *vid = (sdl_rs90_video_t*)data;

   if (!vid)
      return 0.0f;

   switch (vid->refresh_rate)
   {
      case DINGUX_REFRESH_RATE_50HZ:
         return 50.0f;
      default:
         break;
   }
#endif

   return 60.0f;
}

static void sdl_rs90_apply_state_changes(void *data)
{
   sdl_rs90_video_t *vid  = (sdl_rs90_video_t*)data;
   settings_t *settings     = config_get_ptr();

   if (!vid || !settings)
      return;

   vid->keep_aspect = settings->bools.video_dingux_ipu_keep_aspect;
   vid->scale_integer = settings->bools.video_scale_integer;

   /* I believe we need to black out the frame buffer
   because our frame size changed.
   Probably just re-init entire display
   */

}

static uint32_t sdl_rs90_get_flags(void *data)
{
   return 0;
}

static const video_poke_interface_t sdl_rs90_poke_interface = {
   sdl_rs90_get_flags,
   NULL,
   NULL,
   NULL,
   sdl_rs90_get_refresh_rate,
   NULL, /* filtering */
   NULL, /* get_video_output_size */
   NULL, /* get_video_output_prev */
   NULL, /* get_video_output_next */
   NULL, /* get_current_framebuffer */
   NULL, /* get_proc_address */
   NULL,
   sdl_rs90_apply_state_changes,
   sdl_rs90_set_texture_frame,
   sdl_rs90_set_texture_enable,
   NULL,
   NULL, /* sdl_show_mouse */
   NULL, /* sdl_grab_mouse_toggle */
   NULL, /* get_current_shader */
   NULL, /* get_current_software_framebuffer */
   NULL  /* get_hw_render_interface */
};

static void sdl_rs90_get_poke_interface(void *data, const video_poke_interface_t **iface)
{
   *iface = &sdl_rs90_poke_interface;
}

static bool sdl_rs90_gfx_set_shader(void *data,
      enum rarch_shader_type type, const char *path)
{
   return false;
}

video_driver_t video_sdl_dingux = {
   sdl_rs90_gfx_init,
   sdl_rs90_gfx_frame,
   sdl_rs90_gfx_set_nonblock_state,
   sdl_rs90_gfx_alive,
   sdl_rs90_gfx_focus,
   sdl_rs90_gfx_suppress_screensaver,
   sdl_rs90_gfx_has_windowed,
   sdl_rs90_gfx_set_shader,
   sdl_rs90_gfx_free,
   "sdl_dingux",
   NULL,
   NULL, /* set_rotation */
   sdl_rs90_gfx_viewport_info,
   NULL, /* read_viewport  */
   NULL, /* read_frame_raw */
#ifdef HAVE_OVERLAY
   NULL,
#endif
#ifdef HAVE_VIDEO_LAYOUT
  NULL,
#endif
   sdl_rs90_get_poke_interface
};
