// Wokwi Custom Chip - Fake GPS (ATGM336H style)

// Outputs GPRMC NMEA sentences at 9600 baud

// Static position: Morgantown, WV (WVU area)

//

// Pinout:

//   VCC - Power (3.3V or 5V)

//   GND - Ground

//   TX  - NMEA data output (connect to ESP32-S3 RX pin)

//   RX  - Not used (GPS receive, left floating)

//

// SPDX-License-Identifier: MIT


#include "wokwi-api.h"

#include <stdio.h>

#include <stdlib.h>

#include <string.h>


#define SECOND 1000000  // microseconds

#define LEN(arr) ((int)(sizeof(arr) / sizeof(arr)[0]))


// ---------------------------------------------------------------------------

// Static position: WVU Morgantown, WV

//   Lat:  39.6295 N  -> NMEA ddmm.mmmm: 3937.770,N

//   Lon:  79.9559 W  -> NMEA dddmm.mmmm: 07957.354,W

//

// Format: $GPRMC,HHMMSS.sss,A,LLLL.LLLL,N,YYYYY.YYYY,W,speed,course,DDMMYY,,,A*XX

// Status A = active/valid fix

// Speed 0.00 knots, course 0.00 (static)

// ---------------------------------------------------------------------------

const char gps_tx_data[][90] = {
  "$GPRMC,172914.000,A,3937.770,N,07957.354,W,0.00,0.00,060622,,,A*74\r\n",
  "$GPRMC,172915.000,A,3937.770,N,07957.354,W,0.00,0.00,060622,,,A*75\r\n",
  "$GPRMC,172916.000,A,3937.770,N,07957.354,W,0.00,0.00,060622,,,A*76\r\n",
  "$GPRMC,172917.000,A,3937.770,N,07957.354,W,0.00,0.00,060622,,,A*77\r\n",
  "$GPRMC,172918.000,A,3937.770,N,07957.354,W,0.00,0.00,060622,,,A*78\r\n",
  "$GPRMC,172919.000,A,3937.770,N,07957.354,W,0.00,0.00,060622,,,A*79\r\n",
  "$GPRMC,172920.000,A,3937.770,N,07957.354,W,0.00,0.00,060622,,,A*73\r\n",
  "$GPRMC,172921.000,A,3937.770,N,07957.354,W,0.00,0.00,060622,,,A*72\r\n",
  "$GPRMC,172922.000,A,3937.770,N,07957.354,W,0.00,0.00,060622,,,A*71\r\n",
  "$GPRMC,172923.000,A,3937.770,N,07957.354,W,0.00,0.00,060622,,,A*70\r\n",
};


typedef struct {

  uart_dev_t uart0;

  uint32_t   gps_tx_index;

} chip_state_t;


static void chip_timer_event(void *user_data);


void chip_init(void) {

  setvbuf(stdout, NULL, _IOLBF, 1024);


  chip_state_t *chip = malloc(sizeof(chip_state_t));


  const uart_config_t uart_config = {

    .tx        = pin_init("TX", INPUT_PULLUP),

    .rx        = pin_init("RX", INPUT),

    .baud_rate = 9600,

    .user_data = chip,

  };


  chip->uart0        = uart_init(&uart_config);

  chip->gps_tx_index = 0;


  const timer_config_t timer_config = {

    .callback  = chip_timer_event,

    .user_data = chip,

  };


  timer_t timer = timer_init(&timer_config);

  timer_start(timer, SECOND, true);  // fires every 1 second, repeating


  printf("Fake GPS initialized! Position: Morgantown, WV\n");

}


void chip_timer_event(void *user_data) {

  chip_state_t *chip = (chip_state_t *)user_data;


  const char *message = gps_tx_data[chip->gps_tx_index];

  uart_write(chip->uart0, (uint8_t *)message, strlen(message));

  printf("GPS TX [%u]: %s", chip->gps_tx_index, message);


  chip->gps_tx_index++;

  if (chip->gps_tx_index >= LEN(gps_tx_data)) {

    chip->gps_tx_index = 0;

  }

}