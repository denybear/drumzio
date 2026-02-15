#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "bsp/board_api.h"
#include "tusb.h"
#include "hardware/adc.h"

#include "usb_descriptors.h"
#include "drum_trigger.h"


//--------------------------------------------------------------------+
// MACRO CONSTANT TYPEDEF PROTOTYPES
//--------------------------------------------------------------------+

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

	// 5kz sampling
	drum_trigger_cfg_t cfg = {
		.th_high_head = 250, .th_low_head = 120,
		.th_high_rim = 250,	.th_low_rim	= 120,

		.scan_min_ms = 2,		// proche “scan time” des modules
		.release_ms	 = 4,		// fin de frappe rapide -> bon pour roulements
		.max_group_ms = 30,		// sécurité

		.retrigger_head_ms = 18,
		.retrigger_rim_ms = 18,

		.both_ratio_q15 = (uint32_t)(1.50f * 32768.0f),		// si pics à moins de 50% -> BOTH
		.min_secondary_for_both = 300						// évite faux BOTH sur crosstalk faible
	};

	/*
	// 10kz sampling
	drum_trigger_cfg_t cfg = {
		.th_high_head = 250, .th_low_head = 120,
		.th_high_rim = 250,	.th_low_rim	= 120,

		.scan_min_ms = 2,		// proche “scan time” des modules
		.release_ms	 = 3,		// fin de frappe rapide -> bon pour roulements
		.max_group_ms = 30,		// sécurité

		.retrigger_head_ms = 15,
		.retrigger_rim_ms = 15,

		.both_ratio_q15 = (uint32_t)(1.50f * 32768.0f),		// si pics à moins de 50% -> BOTH
		.min_secondary_for_both = 300						// évite faux BOTH sur crosstalk faible
	};
	*/

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

	while (1)
	{
		// tinyusb device task
		tud_task();
		led_blinking_task();

		// read ADC
		adc_select_input(0);
		head = adc_read(); // 0..4095
		adc_select_input(1);
		rim = adc_read();

		// determine if drum was hit
		drum_hit_t hit = drum_trigger_update (&st, &cfg, head, rim, board_millis());
		hid_task (hit.kind);
		sleep_us (200); // ~5 kHz
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
			if (kind == DRUM_HIT_HEAD) keycode[0] = HID_KEY_B;
			if (kind == DRUM_HIT_RIM) keycode[0] = HID_KEY_C;
			if (kind == DRUM_HIT_BOTH) keycode[0] = HID_KEY_D;

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

// We will sent 1 report for HID keyboard: every 10ms (and we check if board button has been pressed then), OR when we receive a drum event
// tud_hid_report_complete_cb() is used to send the next report after previous one is complete
void hid_task(drum_hit_kind_t kind)
{
	static drum_hit_kind_t previous_kind = DRUM_HIT_NONE;		// previous hit
	const uint32_t interval_ms = 10;							// Poll every 10ms
	static uint32_t start_ms = 0;

	if (kind == previous_kind) {								// no drum hit, check whether 10ms have elapsed
		if ((board_millis() - start_ms) < interval_ms) return;	// not enough time and no drum hit: return
	}

	start_ms = board_millis();									// here: either 10ms have elapsed, or a drum event occured; in any case, we send a HID report
	uint32_t const btn = board_button_read();					// read button

	// Remote wakeup
	if ( tud_suspended() && (btn || (kind != DRUM_HIT_NONE)) ) {
		// Wake up host if we are in suspend mode
		// and REMOTE_WAKEUP feature is enabled by host
		tud_remote_wakeup();
	}

	// Send the 1st of report chain, the rest will be sent by tud_hid_report_complete_cb()
	send_hid_report(REPORT_ID_KEYBOARD, btn, kind);
	
	previous_kind = kind;
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
