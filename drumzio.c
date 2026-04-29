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
#include "pico/time.h"
#include "bsp/board_api.h"
#include "tusb.h"
#include "hardware/adc.h"

#include "usb_descriptors.h"
#include "drum_trigger.h"

#define HIT_QUEUE_SIZE 64
#define INTERVAL_MS 10

static drum_trigger_state_t trigger_state;
static drum_trigger_cfg_t trigger_cfg = {
    // Seuils plus élevés pour ignorer les petites vibrations parasites
    .th_high_head = 800, 
    .th_low_head  = 700, 
    
    .th_high_rim  = 900, 
    .th_low_rim   = 800,

    // Fenêtre de capture (très courte pour minimiser le lag)
    .scan_min_us  = 1500, // 1.5 ms

    // Retrigger (le secret contre les frappes multiples)
    // 30ms permet de jouer jusqu'à 2000 BPM, tout en supprimant les rebonds du piezo.
    .retrigger_us = 30000, 

    // Exclusion mutuelle (Crosstalk)
    // Si on tape le Rim, on ignore le Head pendant 20ms de plus.
    .crosstalk_min_us = 20000 
};

static volatile drum_hit_kind_t hit_queue[HIT_QUEUE_SIZE];
static volatile uint8_t hit_queue_head = 0;
static volatile uint8_t hit_queue_tail = 0;

static inline void enqueue_hit_event(drum_hit_kind_t kind)
{
	uint8_t next_tail = (hit_queue_tail + 1) % HIT_QUEUE_SIZE;
	if (next_tail != hit_queue_head) {
		hit_queue[hit_queue_tail] = kind;
		hit_queue_tail = next_tail;
	}
}

static bool sample_timer_callback(struct repeating_timer *rt)
{
	(void) rt;
	static uint32_t hid_last_sent_ms = 0;

	adc_select_input(0);
	int32_t signal0 = (int32_t)adc_read() - 2048;
	adc_select_input(1);
	int32_t signal1 = (int32_t)adc_read() - 2048;

	uint16_t rim = (uint16_t)abs(signal0);
	uint16_t head = (uint16_t)abs(signal1);

	static bool hit_pending_release = false;
	static bool button_pressed = false;

	// process button event
	bool btn = board_button_read();
	if (btn && !button_pressed) {
		enqueue_hit_event(DRUM_HIT_BUTTON);
		enqueue_hit_event(DRUM_HIT_NONE);
		button_pressed = true;
	}
	else if (!btn && button_pressed) {
		button_pressed = false;
	}

	// process drum hit event
	drum_hit_t hit = drum_trigger_update(&trigger_state, &trigger_cfg, head, rim, time_us_32());
	if (hit.kind != DRUM_HIT_NONE) {
		enqueue_hit_event(hit.kind);		// we rely on tiny USB to send HID events every 1ms
		enqueue_hit_event(DRUM_HIT_NONE);	// therefore we can enqueue 2 events at once, they should be sent with 1ms between them
	}

	// process 10ms HID reportq
    if ((board_millis() - hid_last_sent_ms) >= INTERVAL_MS) {
		enqueue_hit_event(DRUM_HIT_NONE);   
		hid_last_sent_ms = board_millis();
	}

	return true;
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
bool hid_task(drum_hit_kind_t);


//--------------------------------------------------------------------+
// MAIN
//--------------------------------------------------------------------+

int main(void)
{
	// Initialize the trigger state once for timer sampling.
	drum_trigger_init(&trigger_state);

	// 50kHz target sampling, handled by a hardware timer callback

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

	struct repeating_timer sample_timer;
	add_repeating_timer_us(20, sample_timer_callback, NULL, &sample_timer);	// 50kHz sampling rate 

	while (1)
	{
		// tinyusb device task
        tud_task();
		led_blinking_task();
		
		while (hit_queue_head != hit_queue_tail) {
			drum_hit_kind_t kind = hit_queue[hit_queue_head];
			if (hid_task(kind)) hit_queue_head = (hit_queue_head + 1) % HIT_QUEUE_SIZE;
			else break;		// if hid_task returns false, it means USB is not ready, so we keep the event in queue and try again later
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

bool send_hid_report(uint8_t report_id, drum_hit_kind_t kind)
{
	// skip if hid is not ready yet
	if (!tud_hid_ready()) return false;

	switch(report_id) {
		case REPORT_ID_KEYBOARD:
		{
			uint8_t keycode[6] = { 0 };
			if (kind == DRUM_HIT_NONE) keycode[0] = 0;
			if (kind == DRUM_HIT_BUTTON) keycode[0] = HID_KEY_A;
			if (kind == DRUM_HIT_HEAD) keycode[0] = HID_KEY_J;
			if (kind == DRUM_HIT_RIM) keycode[0] = HID_KEY_K;


			// if there is something to send, send a keypress; otherwise send a null report to indicate a key-release
			if (keycode[0] != 0) {
				return tud_hid_keyboard_report(REPORT_ID_KEYBOARD, 0, keycode);
			}
			else {
				return tud_hid_keyboard_report(REPORT_ID_KEYBOARD, 0, NULL);	// corresponds to sending keycode to 0x00 (6 bytes set to 0)
			}
		}
		break;

		default: break;
	}
	return true;
}

// We will send 1 report for HID keyboard: every 10ms, or when we receive a drum event, or when there is a buffered hit pending.
// We fully rely on tinyUSB to manage the 1ms timer between sending of 2 USB reports
// Buffer hits so no event is dropped while USB is not ready.
bool hid_task(drum_hit_kind_t kind)
{
        if (!tud_hid_ready()) return false;
		return send_hid_report(REPORT_ID_KEYBOARD, kind);
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
