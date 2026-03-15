#include <stdio.h>
#include <hidapi/hidapi.h>
#include "dualsense.h"

int main() {
	struct dualsense ds;
	if (!dualsense_init(&ds, NULL))
			return 1;
	dualsense_set_microphone_led_status(&ds, DS_MIC_LED_PULSE);
	dualsense_set_lightbar_rgb(&ds, 245, 50, 200, 255);
	dualsense_set_player_leds(&ds, 3, true);
	dualsense_command_trigger_weapon(&ds, DS_TRIGGER_RIGHT, 4, 7, 6);	
	printf("battery: %i percent\n", dualsense_battery(&ds));
	return 0;
}
