/*
 * sddc_stream - command line streaming program for libsddc
 *
 * Copyright (C) 2020 by Franco Venturi
 * Copyright (C) 2021 by Tatu Peltola
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "libsddc.h"


static void stream_callback(uint32_t data_size, uint8_t *data,
                            void *context);

static int stop_reception = 0;
static FILE *output_file;

#define SDDC_CHECK(function, ...)\
if(function(__VA_ARGS__) < 0) {\
  fprintf(stderr, "ERROR - " #function "() failed\n");\
  goto DONE;\
}

int main(int argc, char **argv)
{
  if (argc < 3) {
    fprintf(stderr, "usage: %s <image file> <sample rate>\n", argv[0]);
    return -1;
  }
  output_file = stdout;
  char *imagefile = argv[1];
  double sample_rate = 0.0;
  sscanf(argv[2], "%lf", &sample_rate);

  int ret_val = -1;

  sddc_t *sddc = sddc_open(0, imagefile);
  if (sddc == NULL) {
    fprintf(stderr, "ERROR - sddc_open() failed\n");
    goto DONE;
  }

  /* TODO: all parameters as command line arguments */
  SDDC_CHECK(sddc_set_sample_rate, sddc, sample_rate);
  SDDC_CHECK(sddc_set_async_params, sddc, 0, 0, stream_callback, sddc);
  SDDC_CHECK(sddc_set_rf_mode, sddc, HF_MODE);
  SDDC_CHECK(sddc_set_hf_attenuation, sddc, 0);
  /* 1 disables the bias-T on RX888, 0 enables */
  SDDC_CHECK(sddc_set_hf_bias, sddc, 1);

  SDDC_CHECK(sddc_start_streaming, sddc);
  fprintf(stderr, "started streaming ..\n");

  /* TODO: handle signals to stop reception */
  /* TODO: handle errors from sddc_handle_events */
  stop_reception = 0;
  while (!stop_reception)
    sddc_handle_events(sddc);

  fprintf(stderr, "finished. now stop streaming ..\n");
  SDDC_CHECK(sddc_stop_streaming, sddc);

  /* done - all good */
  ret_val = 0;

DONE:
  if (sddc != NULL)
    sddc_close(sddc);

  return ret_val;
}

static void stream_callback(uint32_t data_size,
                            uint8_t *data,
                            void *context __attribute__((unused)) )
{
  if (stop_reception)
    return;
  fwrite(data, data_size, 1, output_file);
}
