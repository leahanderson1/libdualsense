#include <hidapi/hidapi.h>
#include "dualsense.h"

int main() {
	struct dualsense ds;
	if (!dualsense_init(&ds, NULL))
			return 1;
	dualsense_set_microphone_led_status(&ds, DS_MIC_LED_PULSE);
	dualsense_set_lightbar_rgb(&ds, 245, 0, 0, 255);
	return 0;
}
