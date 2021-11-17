/*
 * libsddc.c - low level functions for wideband SDR receivers like
 *             BBRF103, RX-666, RX888, HF103, etc
 *
 * Copyright (C) 2020 by Franco Venturi
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

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "libsddc.h"
#include "logging.h"
#include "usb_device.h"
#include "streaming.h"

typedef struct sddc sddc_t;


/* internal functions */
static int sddc_set_vhf_gpios(sddc_t *this);


typedef struct sddc {
  enum SDDCStatus status;
  enum SDDCHWModel model;
  uint16_t firmware;
  enum RFMode rf_mode;
  usb_device_t *usb_device;
  streaming_t *streaming;
  int has_clock_source;
  int has_vhf_tuner;
  int hf_attenuator_levels;
  int hf_vga_levels;
  double hf_attenuation;
  int hf_vga_gain;
  double sample_rate;
  double tuner_frequency;
  double tuner_attenuation;
  double tuner_clock;
  double freq_corr_ppm;
  double frequency_range[2];
} sddc_t;


static const double DEFAULT_SAMPLE_RATE = 64e6;       /* 64Msps */
static const double DEFAULT_TUNER_FREQUENCY = 999e3;  /* MW station in Turin */
static const double DEFAULT_FREQ_CORR_PPM = 0.0;      /* frequency correction PPM */
static const double DEFAULT_HF_ATTENUATION = 0;       /* no attenuation */
static const double DEFAULT_TUNER_ATTENUATION = 0;    /* no gain */

static const double TUNER_CLOCK = 32E6;               /* tuner expects 32MHz when running */


/******************************
 * basic functions
 ******************************/

int sddc_get_device_count()
{
  return usb_device_count_devices();
}

int sddc_get_device_info(struct sddc_device_info **sddc_device_infos)
{
  int ret_val = -1; 

  /* no more info to add from usb_device_get_device_list() for now */
  struct usb_device_info *list;
  int ret = usb_device_get_device_list(&list);
  if (ret < 0) {
    goto FAIL0;
  }

  int count = ret;
  struct sddc_device_info *device_infos = (struct sddc_device_info *) malloc((count + 1) * sizeof(struct sddc_device_info));
  /* use the first element to save the pointer to the underlying list,
     so we can use it to free it later on */
  *((void **) device_infos) = list;
  device_infos++;
  for (int i = 0; i < count; ++i) {
    device_infos[i].manufacturer = list[i].manufacturer;
    device_infos[i].product = list[i].product;
    device_infos[i].serial_number = list[i].serial_number;
  }

  *sddc_device_infos = device_infos;
  ret_val = count;

FAIL0:
  return ret_val;
}

int sddc_free_device_info(struct sddc_device_info *sddc_device_infos)
{
  /* just free our structure and call usb_device_free_device_list() to free
     underlying data structure */
  /* retrieve the underlying usb_device list pointer first */
  sddc_device_infos--;
  struct usb_device_info *list = (struct usb_device_info *) *((void **) sddc_device_infos);
  free(sddc_device_infos);
  int ret = usb_device_free_device_list(list);
  return ret;
}

sddc_t *sddc_open(int index, const char* imagefile)
{
  sddc_t *ret_val = 0;

  usb_device_t *usb_device = usb_device_open(index, imagefile, 0);
  if (usb_device == 0) {
    fprintf(stderr, "ERROR - usb_device_open() failed\n");
    goto FAIL0;
  }
  uint8_t data[4];
  int ret = usb_device_control(usb_device, TESTFX3, 0, 0, data, sizeof(data));
  if (ret < 0) {
    fprintf(stderr, "ERROR - usb_device_control(TESTFX3) failed\n");
    goto FAIL1;
  }

  sddc_t *this = (sddc_t *) malloc(sizeof(sddc_t));
  this->status = SDDC_STATUS_READY;
  this->model = (enum SDDCHWModel) data[0];
  this->firmware = (data[1] << 8) | data[2];
  this->rf_mode = HF_MODE;
  this->usb_device = usb_device;
  this->streaming = 0;
  switch (this->model) {
    case HW_BBRF103:
    case HW_RX888:
      this->has_clock_source = 1;
      this->has_vhf_tuner = 1;
      this->hf_attenuator_levels = 3;
      this->hf_vga_levels = 0;
      this->frequency_range[0] = 10e3;
      this->frequency_range[1] = 1750e6;
      break;
    case HW_RX888R2:
      this->has_clock_source = 1;
      this->has_vhf_tuner = 1;
      this->hf_attenuator_levels = 64;
      this->hf_vga_levels = 127;
      this->frequency_range[0] = 10e3;
      this->frequency_range[1] = 1750e6;
      break;
    case HW_HF103:
      this->has_clock_source = 0;
      this->has_vhf_tuner = 0;
      this->hf_attenuator_levels = 32;
      this->hf_vga_levels = 0;
      this->frequency_range[0] = 0;
      this->frequency_range[1] = 32e6;
      break;
    default:
      this->has_clock_source = 0;
      this->has_vhf_tuner = 0;
      this->hf_attenuator_levels = 0;
      this->hf_vga_levels = 0;
      this->frequency_range[0] = 0;
      this->frequency_range[1] = 0;
      break;
  }
  this->sample_rate = DEFAULT_SAMPLE_RATE;             /* default sample rate */
  this->hf_attenuation = DEFAULT_HF_ATTENUATION;       /* default HF attenuation */
  this->hf_vga_gain = 0x25;			       /* default vga gain code */
  this->tuner_frequency = DEFAULT_TUNER_FREQUENCY;     /* default tuner frequency */
  this->tuner_attenuation = DEFAULT_TUNER_ATTENUATION; /* default gain */
  this->tuner_clock = 0;                               /* tuner off */
  this->freq_corr_ppm = DEFAULT_FREQ_CORR_PPM;         /* default frequency correction PPM */

  ret_val = this;
  return ret_val;

FAIL1:
  usb_device_close(usb_device);
FAIL0:
  return ret_val;
}

void sddc_close(sddc_t *this)
{
  usb_device_close(this->usb_device);
  free(this);
  return;
}

enum SDDCStatus sddc_get_status(sddc_t *this)
{
  return this->status;
}

enum SDDCHWModel sddc_get_hw_model(sddc_t *this)
{
  return this->model;
}

uint16_t sddc_get_firmware(sddc_t *this)
{
  return this->firmware;
}

const double *sddc_get_frequency_range(sddc_t *this)
{
  return this->frequency_range;
}

enum RFMode sddc_get_rf_mode(sddc_t *this)
{
  return this->rf_mode;
}

int sddc_set_rf_mode(sddc_t *this, enum RFMode rf_mode)
{
  int ret;
  switch (rf_mode) {
    case HF_MODE:
      this->rf_mode = HF_MODE;

      /* stop tuner */
      ret = usb_device_control(this->usb_device, R82XXSTDBY, 0, 0, 0, 0);
      if (ret < 0) {
        fprintf(stderr, "ERROR - usb_device_control(R82XXSTDBY) failed\n");
        return -1;
      }

      /* switch to HF input and restore hf attenuation */
      ret = sddc_set_hf_attenuation(this, this->hf_attenuation);
      if (ret < 0) {
        fprintf(stderr, "ERROR - sddc_set_hf_attenuation() failed\n");
        return -1;
      }

      /* restore hf vga gain */
      if (this->hf_vga_levels > 0) {
	ret = sddc_set_hf_vga_gain(this, this->hf_vga_gain);
	if (ret < 0) {
	  fprintf(stderr, "ERROR - sddc_set_hf_vga_gain() failed\n");
	  return -1;
	}
      }

      break;
    case VHF_MODE:
      if (!this->has_vhf_tuner) {
        fprintf(stderr, "WARNING - no VHF/UHF tuner found\n");
        return -1;
      }
      this->rf_mode = VHF_MODE;

      /* switch to VHF input */
      ret = sddc_set_vhf_gpios(this);
      if (ret < 0) {
        fprintf(stderr, "ERROR - sddc_set_vhf_gpios() failed\n");
        return -1;
      }

      /* initialize tuner */
      /* tuner reference frequency */
      double correction = 1e-6 * this->freq_corr_ppm * this->tuner_clock;
      uint32_t data = (uint32_t) (this->tuner_clock + correction);
      ret = usb_device_control(this->usb_device, R82XXINIT, 0, 0,
                               (uint8_t *) &data, sizeof(data));
      if (ret < 0) {
        fprintf(stderr, "ERROR - usb_device_control(R82XXINIT) failed\n");
        return -1;
      }

      break;
    default:
      fprintf(stderr, "WARNING - invalid RF mode: %d\n", rf_mode);
      return -1;
  }
  return 0;
}


enum GPIOBits {
  GPIO_ADC_SHDN   = 0x0020,
  GPIO_ADC_DITH   = 0x0040,
  GPIO_ADC_RAND   = 0x0080,
  GPIO_BIAS_HF    = 0x0100,
  GPIO_BIAS_VHF   = 0x0200,
  GPIO_LED_YELLOW = 0x0400,
  GPIO_LED_RED    = 0x0800,
  GPIO_LED_BLUE   = 0x1000,
  GPIO_ATT_SEL0   = 0x2000,
  GPIO_ATT_SEL1   = 0x4000,
  GPIO_VHF_EN     = 0x8000
};

static const uint16_t GPIO_LED_SHIFT = 10;


enum FWRegAddresses {
  FW_REG_R82XX_ATTENUATOR = 0x01,  /* R8xx lna/mixer gain - range: 0-29 */
  FW_REG_R82XX_VGA        = 0x02,  /* R8xx vga gain - range: 0-15 */
  FW_REG_R82XX_SIDEBAND   = 0x03,  /* R8xx sideband - {0,1} */
  FW_REG_R82XX_HARMONIC   = 0x04,  /* R8xx harmonic - {0,1} */
  FW_REG_DAT31_ATT        = 0x0a,  /* DAT-31 att - range: 0-63 */
  FW_REG_AD8370_VGA       = 0x0b,  /* AD8370 chip vga - range: 0-127 */
  FW_REG_PRESELECTOR      = 0x0c   /* preselector - range: 0-2 */
};


/*****************
 * LED functions *
 *****************/
int sddc_led_on(sddc_t *this, uint8_t led_pattern)
{
  if (led_pattern & ~(LED_YELLOW | LED_RED | LED_BLUE)) {
    fprintf(stderr, "ERROR - invalid LED pattern: 0x%02x\n", led_pattern);
    return -1;
  }
  return usb_device_gpio_on(this->usb_device, (uint16_t) led_pattern << GPIO_LED_SHIFT);
}

int sddc_led_off(sddc_t *this, uint8_t led_pattern)
{
  if (led_pattern & ~(LED_YELLOW | LED_RED | LED_BLUE)) {
    fprintf(stderr, "ERROR - invalid LED pattern: 0x%02x\n", led_pattern);
    return -1;
  }
  return usb_device_gpio_off(this->usb_device, (uint16_t) led_pattern << GPIO_LED_SHIFT);
}

int sddc_led_toggle(sddc_t *this, uint8_t led_pattern)
{
  if (led_pattern & ~(LED_YELLOW | LED_RED | LED_BLUE)) {
    fprintf(stderr, "ERROR - invalid LED pattern: 0x%02x\n", led_pattern);
    return -1;
  }
  return usb_device_gpio_toggle(this->usb_device, (uint16_t) led_pattern << GPIO_LED_SHIFT);
}


/*****************
 * ADC functions *
 *****************/
int sddc_get_adc_dither(sddc_t *this)
{
  return (usb_device_gpio_get(this->usb_device) & GPIO_ADC_DITH) != 0;
}

int sddc_set_adc_dither(sddc_t *this, int dither)
{
  if (dither) {
    return usb_device_gpio_on(this->usb_device, GPIO_ADC_DITH);
  } else {
    return usb_device_gpio_off(this->usb_device, GPIO_ADC_DITH);
  }
}

int sddc_get_adc_random(sddc_t *this)
{
  return (usb_device_gpio_get(this->usb_device) & GPIO_ADC_RAND) != 0;
}

int sddc_set_adc_random(sddc_t *this, int random)
{
  if (random) {
    return usb_device_gpio_on(this->usb_device, GPIO_ADC_RAND);
  } else {
    return usb_device_gpio_off(this->usb_device, GPIO_ADC_RAND);
  }
}


/**********************
 * HF block functions *
 **********************/
double sddc_get_hf_attenuation(sddc_t *this)
{
  return this->hf_attenuation;
}

int sddc_set_hf_attenuation(sddc_t *this, double attenuation)
{
  if (this->hf_attenuator_levels == 0) {
    /* no attenuator */
    return 0;
  } else if (this->hf_attenuator_levels == 3) {
    /* old style attenuator with just 0dB, 10dB, and 20Db */
    uint16_t bit_pattern = 0;
    switch ((int) attenuation) {
    case 0:
      bit_pattern = GPIO_ATT_SEL1;
      break;
    case 10:
      bit_pattern = GPIO_ATT_SEL0 | GPIO_ATT_SEL1;
      break;
    case 20:
      bit_pattern = GPIO_ATT_SEL0;
      break;
    default:
      fprintf(stderr, "ERROR - invalid HF attenuation: %lf\n", attenuation);
      return -1;
    }
    this->hf_attenuation = attenuation;
    return usb_device_gpio_set(this->usb_device, bit_pattern,
                               GPIO_ATT_SEL0 | GPIO_ATT_SEL1);
  } else if (this->hf_attenuator_levels == 32) {
    /* new style attenuator with 1dB increments */
    if (attenuation < 0.0 || attenuation > this->hf_attenuator_levels - 1) {
      fprintf(stderr, "ERROR - invalid HF attenuation: %lf\n", attenuation);
      return -1;
    }
    this->hf_attenuation = attenuation;
    uint16_t dat31_att = (int) attenuation;
    return usb_device_set_fw_register(this->usb_device, FW_REG_DAT31_ATT,
                                      dat31_att);
  } else if (this->hf_attenuator_levels == 64) {
    /* pe4312 (0.5dB increments) */
    if (attenuation < 0.0 || attenuation > 31.5) {
      fprintf(stderr, "ERROR - invalid HF attenuation: %lf\n", attenuation);
      return -1;
    }
    this->hf_attenuation = attenuation;
    uint16_t pe4312_att = attenuation * 2;
    return usb_device_set_fw_register(this->usb_device, FW_REG_DAT31_ATT, pe4312_att);
  }
  /* should never get here */
  fprintf(stderr, "ERROR - invalid number of HF attenuator levels: %d\n",
	  this->hf_attenuator_levels);
  return -1;
}

/* gain code (0 to 127) */
/* 0dB:0x10, 10dB:0x17, 20dB:0x25, 30dB:0x5d */
int sddc_set_hf_vga_gain(sddc_t *this, int idx)
{
  if (this->hf_vga_levels == 0) {
    /* no vga */
    return 0;
  } else if (this->hf_vga_levels == 127) {
    if (idx < 0 || idx > 127) {
      fprintf(stderr, "ERROR - invalid HF vga gain idx: %d\n", idx);
      return -1;
    }
    /* AD8370 */
    uint8_t gain_code;
    if (idx > AD8370_GAIN_SWEET_POINT)
      gain_code = AD8370_HIGH_MODE | (idx - AD8370_GAIN_SWEET_POINT + 3);
    else
      gain_code = AD8370_LOW_MODE | (idx + 1);
    this->hf_vga_gain = idx;
    return usb_device_set_fw_register(this->usb_device, FW_REG_AD8370_VGA, gain_code);
  }

  /* should never get here */
  fprintf(stderr, "ERROR - invalid number of HF vga levels: %d\n",
          this->hf_vga_levels);
  return -1;
}

int sddc_get_hf_bias(sddc_t *this)
{
  return (usb_device_gpio_get(this->usb_device) & GPIO_BIAS_HF) != 0;
}

int sddc_set_hf_bias(sddc_t *this, int bias)
{
  if (bias) {
    return usb_device_gpio_on(this->usb_device, GPIO_BIAS_HF);
  } else {
    return usb_device_gpio_off(this->usb_device, GPIO_BIAS_HF);
  }
}


/*****************************************
 * VHF block and VHF/UHF tuner functions *
 *****************************************/
double sddc_get_tuner_frequency(sddc_t *this)
{
  return this->tuner_frequency;
}

int sddc_set_tuner_frequency(sddc_t *this, double frequency)
{
  uint64_t data = (uint64_t) frequency;
  int ret = usb_device_control(this->usb_device, R82XXTUNE, 0, 0,
                               (uint8_t *) &data, sizeof(data));
  if (ret < 0) {
    fprintf(stderr, "ERROR - usb_device_control(R82XXTUNE) failed\n");
    return -1;
  }
  this->tuner_frequency = frequency;
  return 0;
}


/* tuner attenuations - LNA/mixer */
static const double tuner_rf_attenuations_table[] = {
  0.0, 0.9, 1.4, 2.7, 3.7, 7.7, 8.7, 12.5, 14.4, 15.7, 16.6, 19.7, 20.7,
  22.9, 25.4, 28.0, 29.7, 32.8, 33.8, 36.4, 37.2, 38.6, 40.2, 42.1, 43.4,
  43.9, 44.5, 48.0, 49.6
};

int sddc_get_tuner_rf_attenuations(sddc_t *this __attribute__((unused)),
                                   const double *attenuations[])
{
  *attenuations = tuner_rf_attenuations_table;
  return sizeof(tuner_rf_attenuations_table) / sizeof(tuner_rf_attenuations_table[0]);
}

double sddc_get_tuner_rf_attenuation(sddc_t *this)
{
  uint16_t r82xx_attenuator = usb_device_get_fw_register(this->usb_device,
                                                         FW_REG_R82XX_ATTENUATOR);
  return tuner_rf_attenuations_table[(int) r82xx_attenuator];
}

int sddc_set_tuner_rf_attenuation(sddc_t *this, double attenuation)
{
  int rf_attenuation_table_size = sizeof(tuner_rf_attenuations_table) /
                                  sizeof(tuner_rf_attenuations_table[0]);
  uint16_t idx = 0;
  double idx_att = fabs(attenuation - tuner_rf_attenuations_table[idx]);
  for (int i = 1; i < rf_attenuation_table_size; ++i) {
    double att = fabs(attenuation - tuner_rf_attenuations_table[i]);
    if (att < idx_att) {
      idx = i;
      idx_att = att;
    }
  }

  int ret = usb_device_set_fw_register(this->usb_device,
                                       FW_REG_R82XX_ATTENUATOR, idx);
  if (ret < 0) {
    fprintf(stderr, "ERROR - usb_device_set_fw_register(FW_REG_R82XX_ATTENUATOR) failed\n");
    return -1;
  }

  fprintf(stderr, "INFO - RF tuner attenuation set to %.1f\n",
          tuner_rf_attenuations_table[idx]);
  return 0;
}


/* tuner attenuations - VGA */
static const double tuner_if_attenuations_table[] = {
  -4.7, -2.1, 0.5, 3.5, 7.7, 11.2, 13.6, 14.9, 16.3, 19.5, 23.1, 26.5,
  30.0, 33.7, 37.2, 40.8
};

int sddc_get_tuner_if_attenuations(sddc_t *this __attribute__((unused)),
                                   const double *attenuations[])
{
  *attenuations = tuner_if_attenuations_table;
  return sizeof(tuner_if_attenuations_table) / sizeof(tuner_if_attenuations_table[0]);
}

double sddc_get_tuner_if_attenuation(sddc_t *this)
{
  uint16_t r82xx_vga = usb_device_get_fw_register(this->usb_device,
                                                  FW_REG_R82XX_VGA);
  return tuner_if_attenuations_table[(int) r82xx_vga];
}

int sddc_set_tuner_if_attenuation(sddc_t *this, double attenuation)
{
  int if_attenuation_table_size = sizeof(tuner_if_attenuations_table) /
                                  sizeof(tuner_if_attenuations_table[0]);
  uint16_t idx = 0;
  double idx_att = fabs(attenuation - tuner_if_attenuations_table[idx]);
  for (int i = 1; i < if_attenuation_table_size; ++i) {
    double att = fabs(attenuation - tuner_if_attenuations_table[i]);
    if (att < idx_att) {
      idx = i;
      idx_att = att;
    }
  }

  int ret = usb_device_set_fw_register(this->usb_device, FW_REG_R82XX_VGA, idx);
  if (ret < 0) {
    fprintf(stderr, "ERROR - usb_device_set_fw_register(FW_REG_R82XX_VGA) failed\n");
    return -1;
  }

  fprintf(stderr, "INFO - IF tuner attenuation set to %.1f\n",
          tuner_if_attenuations_table[idx]);
  return 0;
}

int sddc_get_vhf_bias(sddc_t *this)
{
  return (usb_device_gpio_get(this->usb_device) & GPIO_BIAS_VHF) != 0;
}

int sddc_set_vhf_bias(sddc_t *this, int bias)
{
  if (bias) {
    return usb_device_gpio_on(this->usb_device, GPIO_BIAS_VHF);
  } else {
    return usb_device_gpio_off(this->usb_device, GPIO_BIAS_VHF);
  }
}


/******************************
 * streaming related functions
 ******************************/
int sddc_set_sample_rate(sddc_t *this, double sample_rate)
{
  /* no checks yet */
  this->sample_rate = sample_rate;

  return 0;
}

int sddc_set_async_params(sddc_t *this, uint32_t frame_size,
                           uint32_t num_frames, sddc_read_async_cb_t callback,
                           void *callback_context)
{
  if (this->streaming) {
    fprintf(stderr, "ERROR - sddc_set_async_params() failed: streaming already configured\n");
    return -1;
  }

  this->streaming = streaming_open_async(this->usb_device, frame_size,
                                         num_frames, callback,
                                         callback_context);
  if (this->streaming == 0) {
    fprintf(stderr, "ERROR - streaming_open_async() failed\n");
    return -1;
  }

  return 0;
}

int sddc_start_streaming(sddc_t *this)
{
  if (this->status != SDDC_STATUS_READY) {
    fprintf(stderr, "ERROR - sddc_start_streaming() called with SDR status not READY: %d\n", this->status);
    return -1;
  }

  /* ADC sampling frequency */
  double correction = 1e-6 * this->freq_corr_ppm * this->sample_rate;
  uint32_t data = (uint32_t) (this->sample_rate + correction);

  int ret = usb_device_control(this->usb_device, STARTADC, 0, 0,
                               (uint8_t *) &data, sizeof(data));
  if (ret < 0) {
    fprintf(stderr, "ERROR - usb_device_control(STARTADC) failed\n");
    return -1;
  }

  /* initialize tuner */
  if (this->rf_mode == VHF_MODE) {
    /* tuner reference frequency */
    double correction = 1e-6 * this->freq_corr_ppm * TUNER_CLOCK;
    uint32_t data = (uint32_t) (TUNER_CLOCK + correction);

    int ret = usb_device_control(this->usb_device, R82XXINIT, 0, 0,
                                 (uint8_t *) &data, sizeof(data));
    if (ret < 0) {
      fprintf(stderr, "ERROR - usb_device_control(R82XXINIT) failed\n");
      return -1;
    }
  }

  /* start async streaming */
  if (this->streaming) {
    streaming_set_sample_rate(this->streaming, (uint32_t) this->sample_rate);
    int ret = streaming_start(this->streaming);
    if (ret < 0) {
      fprintf(stderr, "ERROR - streaming_start() failed\n");
      return -1;
    }
  }

  /* start the producer */
  ret = usb_device_control(this->usb_device, STARTFX3, 0, 0, 0, 0);
  if (ret < 0) {
    fprintf(stderr, "ERROR - usb_device_control(STARTFX3) failed\n");
    return -1;
  }

  /* all good */
  this->status = SDDC_STATUS_STREAMING;
  return 0;
}

int sddc_handle_events(sddc_t *this)
{
  return usb_device_handle_events(this->usb_device);
}

int sddc_stop_streaming(sddc_t *this)
{
  if (this->status != SDDC_STATUS_STREAMING) {
    fprintf(stderr, "ERROR - sddc_stop_streaming() called with SDR status not STREAMING: %d\n", this->status);
    return -1;
  }

  /* stop the producer */
  int ret = usb_device_control(this->usb_device, STOPFX3, 0, 0, 0, 0);
  if (ret < 0) {
    fprintf(stderr, "ERROR - usb_device_control(STOPFX3) failed\n");
    return -1;
  }

  /* stop async streaming */
  if (this->streaming) {
    int ret = streaming_stop(this->streaming);
    if (ret < 0) {
      fprintf(stderr, "ERROR - streaming_stop() failed\n");
      return -1;
    }

    streaming_close(this->streaming);
  }

  /* stop tuner */
  if (this->rf_mode == VHF_MODE) {
    int ret = usb_device_control(this->usb_device, R82XXSTDBY, 0, 0, 0, 0);
    if (ret < 0) {
      fprintf(stderr, "ERROR - usb_device_control(R82XXSTDBY) failed\n");
      return -1;
    }
  }

  /* stop ADC */
  ret = usb_device_gpio_on(this->usb_device, GPIO_ADC_SHDN);
  if (ret < 0) {
    fprintf(stderr, "ERROR - usb_device_gpio_on(ADC_SHDN) failed\n");
    return -1;
  }

  /* all good */
  this->status = SDDC_STATUS_READY;
  return 0;
}

int sddc_reset_status(sddc_t *this)
{
  int ret = streaming_reset_status(this->streaming);
  if (ret < 0) {
    fprintf(stderr, "ERROR - streaming_reset_status() failed\n");
    return -1;
  }
  return 0;
}

int sddc_read_sync(sddc_t *this, uint8_t *data, int length, int *transferred)
{
  return streaming_read_sync(this->streaming, data, length, transferred);
}


/******************************
 * Misc functions
 ******************************/
double sddc_get_frequency_correction(sddc_t *this)
{
  return this->freq_corr_ppm;
}

int sddc_set_frequency_correction(sddc_t *this, double correction)
{
  if (this->status == SDDC_STATUS_STREAMING) {
    fprintf(stderr, "ERROR - sddc_set_frequency_correction() failed - device is streaming\n");
    return -1;
  }
  this->freq_corr_ppm = correction;
  return 0;
}


/* internal functions */
/* helper method to configure GPIOs for VHF */
int sddc_set_vhf_gpios(sddc_t* this) {
    return usb_device_gpio_set(this->usb_device, 0, GPIO_ATT_SEL0 | GPIO_ATT_SEL1);
}
