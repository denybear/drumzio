/*
Electronic diagram

Piezo + o---R1--+---+---+---+---+--------------------> GPIO26 (ADC0)
            	|   |   |   |   |
			    R3  D1  R2  C1  D2
			    |   |   |   |   |
                |   |   +---+---+--------------------> GPIO28 (AGND)
				+---+--------------------------------> 3.3V OUT (PIN 36)
Piezo - o--------------------------------------------> GPIO28 (AGND)

R1 = 22 kOhm
R2 = 22 kOhm
R3 = 22 kOhm
C1 = 2.2 nF
D1 = diode Shottky BAT85 (barre - cathode- du côté du +3.3V, anode du côté du piezo)
D2 = diode Shottky BAT85 (barre - cathode- du côté du GPIO26, anode du côté du AGND)

*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "bsp/board_api.h"
#include "tusb.h"
#include "hardware/adc.h"
#include "hardware/dma.h"
#include "hardware/irq.h"

#include "usb_descriptors.h"
#include "drum_trigger.h"

#define ADC_DMA_BUFFER_WORDS 64

static uint16_t adc_dma_buf[ADC_DMA_BUFFER_WORDS];
static int adc_dma_chan;
static volatile uint32_t adc_dma_ready_count = 0;

static void adc_dma_handler(void)
{
    dma_hw->ints0 = 1u << adc_dma_chan;
    adc_dma_ready_count++;
}

static void adc_dma_init(void)
{
    adc_fifo_drain();
    adc_set_round_robin((1u << 0) | (1u << 1));
    adc_fifo_setup(true, true, 1, true, false);
    adc_set_clkdiv(130.0f); // about 5kHz pair sample rate for two channels
    adc_run(true);

    adc_dma_chan = dma_claim_unused_channel(true);
    dma_channel_config cfg = dma_channel_get_default_config(adc_dma_chan);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_16);
    channel_config_set_read_increment(&cfg, false);
    channel_config_set_write_increment(&cfg, true);
    channel_config_set_dreq(&cfg, DREQ_ADC);

    dma_channel_configure(adc_dma_chan, &cfg, adc_dma_buf, &adc_hw->fifo, ADC_DMA_BUFFER_WORDS, false);
    dma_channel_set_irq0_enabled(adc_dma_chan, true);
    irq_set_exclusive_handler(DMA_IRQ_0, adc_dma_handler);
    irq_set_enabled(DMA_IRQ_0, true);
    dma_channel_start(adc_dma_chan);
}

/* Blink pattern
 * - 250 ms	: device not mounted
 * - 1000 ms : device mounted
 * - 2500 ms : device is suspended
 */
enum	{
	BLINK_NOT_MOUNTED = 250,
	BLINK_MOUNTED = 1000,
	BLINK_SUSPENDED = 2500,
};

static uint32_t blink_interval_ms = BLINK_NOT_MOUNTED;

void led_blinking_task(void);
void hid_task(drum_hit_kind_t);


//--------------------------------------------------------------------+
// MAIN
//--------------------------------------------------------------------+

int main(void)
{
	// variables
	int i;
	uint16_t head;
	uint16_t rim;
	uint16_t result;
	drum_trigger_state_t st;
	drum_trigger_init(&st);
	int32_t signal;

	// 5kHz target sampling, handled in hardware via ADC DMA
	drum_trigger_cfg_t cfg = {
		.th_high_head = 426, .th_low_head = 316,
		.th_high_rim  = 507, .th_low_rim  = 305,

		.scan_min_ms = 10,
		.release_ms  = 50,
		.max_group_ms = 250,

		.retrigger_head_ms = 50,
		.retrigger_rim_ms  = 50,

		.both_ratio_q15 = (uint32_t)(39321),
		.min_secondary_for_both = 528
	};

	// Initialize the standard I/O
	board_init();
	stdio_init_all();

	// Select ADC0 and ADC1
	adc_init();
	adc_gpio_init(26); // GPIO 26 corresponds to ADC0
	adc_gpio_init(27); // GPIO 27 corresponds to ADC1
	adc_set_temp_sensor_enabled(false);

	// init device stack on configured roothub port
	tud_init(BOARD_TUD_RHPORT);

	if (board_init_after_tusb) {
		board_init_after_tusb();
	}

	adc_select_input(0);
	adc_dma_init();

	while (1)
	{
		// tinyusb device task
		tud_task();
		led_blinking_task();

		if (adc_dma_ready_count > 0) {
			adc_dma_ready_count--;
			for (int i = 0; i < ADC_DMA_BUFFER_WORDS; i += 2) {
				int32_t signal0 = (int32_t)adc_dma_buf[i] - 2048;
				int32_t signal1 = (int32_t)adc_dma_buf[i + 1] - 2048;
				rim = (uint16_t)abs(signal0);
				head = (uint16_t)abs(signal1);

				drum_hit_t hit = drum_trigger_update(&st, &cfg, head, rim, board_millis());
				hid_task(hit.kind);

				if ((i & 0x0F) == 0) {
					tud_task();
					led_blinking_task();
				}
			}
			dma_channel_set_read_addr(adc_dma_chan, &adc_hw->fifo, true);
			dma_channel_start(adc_dma_chan);
		}
	}
}


//--------------------------------------------------------------------+
// Device callbacks
//--------------------------------------------------------------------+

// Invoked when device is mounted
void tud_mount_cb(void)
{
	blink_interval_ms = BLINK_MOUNTED;
}

// Invoked when device is unmounted
void tud_umount_cb(void)
{
	blink_interval_ms = BLINK_NOT_MOUNTED;
}

// Invoked when usb bus is suspended
// remote_wakeup_en : if host allow us	to perform remote wakeup
// Within 7ms, device must draw an average of current less than 2.5 mA from bus
void tud_suspend_cb(bool remote_wakeup_en)
{
	(void) remote_wakeup_en;
	blink_interval_ms = BLINK_SUSPENDED;
}

// Invoked when usb bus is resumed
void tud_resume_cb(void)
{
	blink_interval_ms = tud_mounted() ? BLINK_MOUNTED : BLINK_NOT_MOUNTED;
}

//--------------------------------------------------------------------+
// USB HID
//--------------------------------------------------------------------+

static void send_hid_report(uint8_t report_id, uint32_t btn, drum_hit_kind_t kind)
{
	// use to avoid send multiple consecutive zero report for keyboard
	//static bool null_report_was_sent_before = false;
	
	// skip if hid is not ready yet
	if ( !tud_hid_ready() ) return;

	switch(report_id) {
		case REPORT_ID_KEYBOARD:
		{
			uint8_t keycode[6] = { 0 };
			if (btn) keycode[0] = HID_KEY_A;
			if (kind == DRUM_HIT_HEAD) keycode[0] = HID_KEY_J;
			if (kind == DRUM_HIT_RIM) keycode[0] = HID_KEY_K;
			if (kind == DRUM_HIT_BOTH) {
				keycode[0] = HID_KEY_J;
				keycode[1] = HID_KEY_K;
			}

			// if there is something to send, send a keypress; otherwise send a null report to indicate a key-depress
			if (keycode[0] != 0) {
				tud_hid_keyboard_report(REPORT_ID_KEYBOARD, 0, keycode);
				//null_report_was_sent_before = false;
			}
			else {
				tud_hid_keyboard_report(REPORT_ID_KEYBOARD, 0, NULL);

				//if (!null_report_was_sent_before) tud_hid_keyboard_report(REPORT_ID_KEYBOARD, 0, NULL);
				//null_report_was_sent_before = true;
			}
		}
		break;

		default: break;
	}
}

// We will send 1 report for HID keyboard: every 10ms, or when we receive a drum event, or when there is a buffered hit pending.
// Buffer hits so no event is dropped while USB is not ready.
void hid_task(drum_hit_kind_t kind)
{
        static drum_hit_kind_t pending_hits[16];
        static uint8_t pending_head = 0;
        static uint8_t pending_tail = 0;
        static bool waiting_release = false;
        static drum_hit_kind_t previous_kind = DRUM_HIT_NONE;        // previous report kind
        const uint32_t interval_ms = 10;                            // Poll every 10ms
        static uint32_t start_ms = 0;

        // Buffer any new drum hit event so it can be sent later when USB is ready.
        if (kind != DRUM_HIT_NONE) {
                uint8_t next_tail = (pending_tail + 1) % sizeof(pending_hits);
                if (next_tail != pending_head) {
                        pending_hits[pending_tail] = kind;
                        pending_tail = next_tail;
                }
        }

        if ( !tud_hid_ready() ) return;

        uint32_t const btn = board_button_read();                    // read button

        // Remote wakeup
        if ( tud_suspended() && (btn || (kind != DRUM_HIT_NONE)) ) {
                tud_remote_wakeup();
        }

        bool should_send = false;
        drum_hit_kind_t report_kind = DRUM_HIT_NONE;

        // Prioritize releasing the previous key before sending a new hit.
        if (waiting_release) {
                should_send = true;
                report_kind = DRUM_HIT_NONE;
                waiting_release = false;
        }
        else if (pending_head != pending_tail) {
                should_send = true;
                report_kind = pending_hits[pending_head];
                pending_head = (pending_head + 1) % sizeof(pending_hits);
                waiting_release = true;
        }
        else if (btn || previous_kind != DRUM_HIT_NONE) {
                if ((board_millis() - start_ms) >= interval_ms) {
                        should_send = true;
                        report_kind = DRUM_HIT_NONE;
                }
        }
        else {
                if ((board_millis() - start_ms) < interval_ms) return;
                should_send = true;
                report_kind = DRUM_HIT_NONE;
        }

        if (!should_send) return;

        start_ms = board_millis();                                    // here: either 10ms have elapsed, or a report is due, or a buffered hit is being sent
        send_hid_report(REPORT_ID_KEYBOARD, btn, report_kind);

        previous_kind = report_kind;
}

// Invoked when sent REPORT successfully to host
// Application can use this to send the next report
// Note: For composite reports, report[0] is report ID
void tud_hid_report_complete_cb(uint8_t instance, uint8_t const* report, uint16_t len)
{
	return;
}

// Invoked when received GET_REPORT control request
// Application must fill buffer report's content and return its length.
// Return zero will cause the stack to STALL request
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer, uint16_t reqlen)
{
	// TODO not Implemented
	(void) instance;
	(void) report_id;
	(void) report_type;
	(void) buffer;
	(void) reqlen;

	return 0;
}

// Invoked when received SET_REPORT control request or
// received data on OUT endpoint ( Report ID = 0, Type = 0 )
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsize)
{
	(void) instance;

	if (report_type == HID_REPORT_TYPE_OUTPUT)
	{
		// Set keyboard LED e.g Capslock, Numlock etc...
		if (report_id == REPORT_ID_KEYBOARD) {
			// bufsize should be (at least) 1
			if ( bufsize < 1 ) return;

			uint8_t const kbd_leds = buffer[0];

			if (kbd_leds & KEYBOARD_LED_CAPSLOCK) {
				// Capslock On: disable blink, turn led on
				blink_interval_ms = 0;
				board_led_write(true);
			}
			else {
				// Caplocks Off: back to normal blink
				board_led_write(false);
				blink_interval_ms = BLINK_MOUNTED;
			}
		}
	}
}

//--------------------------------------------------------------------+
// BLINKING TASK
//--------------------------------------------------------------------+
void led_blinking_task(void)
{
	static uint32_t start_ms = 0;
	static bool led_state = false;

	// blink is disabled
	if (!blink_interval_ms) return;

	// Blink every interval ms
	if ( board_millis() - start_ms < blink_interval_ms) return; // not enough time
	start_ms += blink_interval_ms;

	board_led_write(led_state);
	led_state = 1 - led_state; // toggle
}
