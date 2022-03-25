/*
Copyright (c) 2014-2021 NicoHood
See the readme for credit to other people.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include "Gamecube_N64.h"

bool callonce = true;
size_t ring_buffer_size = 3000;
int tx_channel = 0; // for ESP32-C3, 0-1 are valid TX channels
int rx_channel = 2; // for ESP32-C3, 2-3 are valid RX channels
rmt_config_t rmt_tx;
rmt_config_t rmt_rx;
RingbufHandle_t rx_ring_buffer = NULL;
#define RMT_CLOCK_DIVIDER 80

static const rmt_item32_t zero_bit = {
    .duration0 = 3, // 3 microseconds low
    .level0 = 0,
    .duration1 = 1, // 1 microsecond high
    .level1 = 1,
};

static const rmt_item32_t one_bit = {
    .duration0 = 1, // 1 microseocond low
    .level0 = 0,
    .duration1 = 3, // 3 microseconds high
    .level1 = 1,
};

static const rmt_item32_t stop_bit = {
    .duration0 = 2, // 2 microsecond low
    .level0 = 0,
    .duration1 = 2, // 2 microseconds high
    .level1 = 1,
};

//================================================================================
// Gamecube/N64 I/O functions
//================================================================================

uint8_t gc_n64_send_get(const gpio_num_t pin, uint8_t* command, const uint8_t commandLen,
    uint8_t* report){

    // send the command
    gc_n64_send(command, commandLen, pin);

    // read in data
    uint8_t receivedBytes = gc_n64_get(report, pin);

    // end of time sensitive code
    // return received length
    return receivedBytes;
}

void gpiosetup(gpio_num_t pin) {
    rmt_tx.rmt_mode = RMT_MODE_TX;
    rmt_tx.channel = tx_channel;
    rmt_tx.gpio_num = pin;
    rmt_tx.clk_div = RMT_CLOCK_DIVIDER;
    rmt_tx.mem_block_num = 1;
    rmt_tx.flags = 0;

    rmt_tx.tx_config.idle_level = 1;
    rmt_tx.tx_config.carrier_en = false;
    rmt_tx.tx_config.loop_en = false;
    rmt_tx.tx_config.idle_output_en = true;

    rmt_config(&rmt_tx);
    rmt_driver_install(tx_channel, 0, 0);
    // End TX Setup

    // RX Setup
    rmt_rx.rmt_mode = RMT_MODE_RX;
    rmt_rx.channel = rx_channel;
    rmt_rx.gpio_num = pin;
    rmt_rx.clk_div = RMT_CLOCK_DIVIDER;
    rmt_rx.mem_block_num = 2;
    rmt_rx.flags = 0;

    rmt_rx.rx_config.idle_threshold = 6;
    rmt_rx.rx_config.filter_ticks_thresh = 0;
    rmt_rx.rx_config.filter_en = false;

    rmt_config(&rmt_rx);
    rmt_driver_install(rmt_rx.channel, ring_buffer_size, 0);

    rmt_get_ringbuf_handle(rx_channel, &rx_ring_buffer);
    rmt_rx_start(rx_channel, true);
    // End RX Setup

    // Enable open drain on the pin.
    gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_matrix_out(pin, RMT_SIG_OUT0_IDX + tx_channel, 0, 0);
    gpio_matrix_in(pin, RMT_SIG_IN0_IDX + rx_channel, 0);

}

static inline void IRAM_ATTR writeByte(uint8_t b, rmt_item32_t* items)
{
    for (int i = 0; i < 8; i++) {
        bool is_one = ((b >> (7 - i)) & 1) == 1;
        items[i] = is_one ? one_bit : zero_bit;
    }
}

void IRAM_ATTR gc_n64_send(const uint8_t* buff, uint8_t len, const gpio_num_t pin)
{
    if (callonce) {
        gpiosetup(pin);
        callonce = false;
    }
    rmt_item32_t out_pulses[len * 8 + 1];
    for (int i = 0; i < len; i++) {
        writeByte(buff[i], &out_pulses[i * 8]);
    }
    out_pulses[len * 8] = stop_bit;
    rmt_write_items(tx_channel, out_pulses, (len * 8 + 1), true);
}

static inline uint8_t IRAM_ATTR readByte(rmt_item32_t* items) {
    uint8_t val = 0;

    for (int i = 0; i < 8; i++) {
        bool is_one = items[i].duration0 < items[i].duration1;
        val |= (is_one << (7 - i));
    }
    return val;
}

uint8_t gc_n64_get(uint8_t* buff, const gpio_num_t pin)
{
    if (callonce) {
        gpiosetup(pin);
        callonce = false;
    }
    size_t rx_size = 0;
    rmt_item32_t* item = (rmt_item32_t*)xRingbufferReceive(rx_ring_buffer, &rx_size, 100);
    uint8_t reclen = 0;
    if (item) {
        size_t num_items = rx_size / sizeof(rmt_item32_t);
        if (num_items == 9) {
            buff[0] = readByte(&item[0]);
            reclen = 1;
        } else if (num_items == 25) {
            buff[0] = readByte(&item[0]);
            buff[1] = readByte(&item[8]);
            buff[2] = readByte(&item[16]);
            reclen = 3;
        } else if (num_items >= 90) {
            buff[0] = readByte(&item[25]);
            buff[1] = readByte(&item[33]);
            buff[2] = readByte(&item[41]);
            buff[3] = readByte(&item[49]);
            buff[4] = readByte(&item[57]);
            buff[5] = readByte(&item[65]);
            buff[6] = readByte(&item[73]);
            buff[7] = readByte(&item[81]);
            reclen = 8;
        }
        vRingbufferReturnItem(rx_ring_buffer, (void*)item);
    }
    return reclen;
}
