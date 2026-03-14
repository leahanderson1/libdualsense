// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  HID driver for Sony DualSense(TM) controller.
 *
 *  Copyright (c) 2020 Sony Interactive Entertainment
 */

#define _XOPEN_SOURCE 700

#include <unistd.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include <hidapi/hidapi.h>

#include "crc32.h"
#include "dualsense.h"

void dualsense_init_output_report(struct dualsense *ds, struct dualsense_output_report *rp, void *buf)
{
    if (ds->bt) {
        struct dualsense_output_report_bt *bt = buf;

        memset(bt, 0, sizeof(*bt));
        bt->report_id = DS_OUTPUT_REPORT_BT;
        bt->tag = DS_OUTPUT_TAG; /* Tag must be set. Exact meaning is unclear. */

        /*
         * Highest 4-bit is a sequence number, which needs to be increased
         * every report. Lowest 4-bit is tag and can be zero for now.
         */
        bt->seq_tag = ds->output_seq;
        if (++ds->output_seq == 16)
            ds->output_seq = 0;

        rp->data = buf;
        rp->len = sizeof(*bt);
        rp->bt = bt;
        rp->usb = NULL;
        rp->common = &bt->common;
    } else { /* USB */
        struct dualsense_output_report_usb *usb = buf;

        memset(usb, 0, sizeof(*usb));
        usb->report_id = DS_OUTPUT_REPORT_USB;

        rp->data = buf;
        rp->len = sizeof(*usb);
        rp->bt = NULL;
        rp->usb = usb;
        rp->common = &usb->common;
    }
}

void dualsense_send_output_report(struct dualsense *ds, struct dualsense_output_report *report)
{
    /* Bluetooth packets need to be signed with a CRC in the last 4 bytes. */
    if (report->bt) {
        uint32_t crc;
        uint8_t seed = PS_OUTPUT_CRC32_SEED;

        crc = crc32_le(0xFFFFFFFF, &seed, 1);
        crc = ~crc32_le(crc, report->data, report->len - 4);

        report->bt->crc32 = crc;
    }

    int res = hid_write(ds->dev, report->data, report->len);
    if (res < 0) {
        fprintf(stderr, "err:ds: %ls\n", hid_error(ds->dev));
    }
}

static bool compare_serial(const char *s, const wchar_t *dev)
{
    if (!s) {
        return true;
    }
    const size_t len = wcslen(dev);
    if (strlen(s) != len) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        if (toupper(s[i]) != toupper(dev[i])) {
            return false;
        }
    }
    return true;
}

struct hid_device_info *dualsense_hid_enumerate(void)
{
    struct hid_device_info *devs;
    struct hid_device_info **end = &devs;
    *end = hid_enumerate(DS_VENDOR_ID, DS_PRODUCT_ID);
    while (*end) {
        end = &(*end)->next;
    }
    *end = hid_enumerate(DS_VENDOR_ID, DS_EDGE_PRODUCT_ID);
    return devs;
}

bool dualsense_init(struct dualsense *ds, const char *serial)
{
    bool ret = false;

    memset(ds, 0, sizeof(*ds));

    bool found = false;
    struct hid_device_info *devs = dualsense_hid_enumerate();
    struct hid_device_info *dev = devs;
    while (dev) {
        if (compare_serial(serial, dev->serial_number)) {
            found = true;
            break;
        }
        dev = dev->next;
    }

    if (!found) {
        if (serial) {
            fprintf(stderr, "err:ds: device '%s' not found\n", serial);
        } else {
            fprintf(stderr, "err:ds: no device found\n");
        }
        ret = false;
        goto out;
    }

    ds->dev = hid_open(DS_VENDOR_ID, dev->product_id, dev->serial_number);
    if (!ds->dev) {
        fprintf(stderr, "err:ds: failed to open device: %ls\n", hid_error(NULL));
        ret = false;
        goto out;
    }

    wchar_t *serial_number = dev->serial_number;

    if (wcslen(serial_number) != 17) {
        fprintf(stderr, "err:ds: invalid device serial number: %ls\n", serial_number);
        // Let's just fake serial number as everything will still work
        serial_number = L"00:00:00:00:00:00";
    }

    for (int i = 0; i < 18; ++i) {
        char c = serial_number[i];
        if (c && (i + 1) % 3) {
            c = toupper(c);
        }
        ds->mac_address[i] = c;
    }

    ds->bt = dev->interface_number == -1;
    ds->product_id = dev->product_id;

    ret = true;

out:
    if (devs) {
        hid_free_enumeration(devs);
    }
    return ret;
}

void dualsense_close(struct dualsense *ds)
{
    hid_close(ds->dev);
}

int dualsense_power_off(struct dualsense *ds)
{
    uint8_t buf[DS_FEATURE_REPORT_BLUETOOTH_CONTROL_SIZE];
    memset(buf, 0, sizeof(buf));
    buf[0] = DS_FEATURE_REPORT_BLUETOOTH_CONTROL;
    buf[1] = DS_BLUETOOTH_CONTROL_OFF;

    if (ds->bt) {
        uint32_t crc;
        uint8_t seed = PS_FEATURE_CRC32_SEED;

        crc = crc32_le(0xFFFFFFFF, &seed, 1);
        crc = ~crc32_le(crc, buf, sizeof(buf) - 4);

        *(uint32_t*)(buf + sizeof(buf) - sizeof(crc)) = crc;
    }

    int res = hid_send_feature_report(ds->dev, buf, sizeof(buf));
    if (res != sizeof(buf)) {
        fprintf(stderr, "err:ds: invalid feature report\n");
        return DS_INVALID;
    }

    return 0;
}

int dualsense_battery(struct dualsense *ds)
{
    uint8_t data[DS_INPUT_REPORT_BT_SIZE];
    int res = hid_read_timeout(ds->dev, data, sizeof(data), 1000);
    if (res <= 0) {
        if (res == 0) {
            fprintf(stderr, "err:ds: timeout waiting for report\n");
        } else {
		    fprintf(stderr, "err:ds: failed to read report %ls\n", hid_error(ds->dev));
		}
		return DS_ERROR;
	    }

	    struct dualsense_input_report *ds_report;

	    if (!ds->bt && data[0] == DS_INPUT_REPORT_USB && res == DS_INPUT_REPORT_USB_SIZE) {
		ds_report = (struct dualsense_input_report *)&data[1];
	    } else if (ds->bt && data[0] == DS_INPUT_REPORT_BT && res == DS_INPUT_REPORT_BT_SIZE) {
		/* Last 4 bytes of input report contain crc32 */
		/* uint32_t report_crc = *(uint32_t*)&data[res - 4]; */
		ds_report = (struct dualsense_input_report *)&data[2];
	    } else {
		fprintf(stderr, "err:ds: unhandled report ID %d\n", (int)data[0]);
		return DS_ERROR;
	    }

	    const char *battery_status;
	    uint8_t battery_capacity;
	    uint8_t battery_data = ds_report->status & DS_STATUS_BATTERY_CAPACITY;
	    uint8_t charging_status = (ds_report->status & DS_STATUS_CHARGING) >> DS_STATUS_CHARGING_SHIFT;

#define min(a, b) ((a) < (b) ? (a) : (b))
	    switch (charging_status) {
	    case 0x0:
		/*
		 * Each unit of battery data corresponds to 10%
		 * 0 = 0-9%, 1 = 10-19%, .. and 10 = 100%
		 */
		battery_capacity = min(battery_data * 10 + 5, 100);
		battery_status = "discharging";
		break;
	    case 0x1:
		battery_capacity = min(battery_data * 10 + 5, 100);
		battery_status = "charging";
		break;
	    case 0x2:
		battery_capacity = 100;
		battery_status = "full";
		break;
	    case 0xa: /* voltage or temperature out of range */
	    case 0xb: /* temperature error */
		// since the error flags are negative, it is extremely easy for a developer to tell if something got fucked up, and get the battery percentage in the same return value. useful, huh?
		battery_capacity = DS_ERROR;
		battery_status = "not-charging";
		fprintf(stderr, "ds:warn: temperature error has been detected. retuning DS_ERROR\n");
		break;
	    case 0xf: /* charging error */
	    default:
		battery_capacity = DS_ERROR;
		battery_status = "unknown";
		fprintf(stderr, "ds:warn: charging error has been detected. returning DS_ERROR\n");
	    }
#undef min
	    return (int)battery_capacity;
	}
/* can't think of a good way to implement this
	int dualsense_info(struct dualsense *ds)
	{
	    uint8_t buf[DS_FEATURE_REPORT_FIRMWARE_INFO_SIZE];
	    memset(buf, 0, sizeof(buf));
	    buf[0] = DS_FEATURE_REPORT_FIRMWARE_INFO;
	    int res = hid_get_feature_report(ds->dev, buf, sizeof(buf));
	    if (res != sizeof(buf)) {
		fprintf(stderr, "Invalid feature report\n");
		return false;
	    }

	    struct dualsense_feature_report_firmware *ds_report;
	    ds_report = (struct dualsense_feature_report_firmware *)&buf;

	    printf("Hardware: %x\n", ds_report->hardware_info);
	    printf("Build date: %.11s %.8s\n", ds_report->build_date, ds_report->build_time);
	    printf("Firmware: %x (type %i)\n", ds_report->firmware_version, ds_report->fw_type);
	    printf("Fw version: %i %i %i\n", ds_report->fw_version_1, ds_report->fw_version_2, ds_report->fw_version_3);
	    printf("Sw series: %i\n", ds_report->sw_series);
	    printf("Update version: %04x\n", ds_report->update_version);
	    \/\*printf("Device info: %.12s\n", ds_report->device_info); \*\/
	    \/\*printf("Update image info: %c\n", ds_report->update_image_info); \*\/

	    return 0;
	} */
	int dualsense_set_lightbar_state(struct dualsense *ds, bool state)
	{
	    struct dualsense_output_report rp;
	    uint8_t rbuf[DS_OUTPUT_REPORT_BT_SIZE];
	    dualsense_init_output_report(ds, &rp, rbuf);

	    rp.common->valid_flag2 = DS_OUTPUT_VALID_FLAG2_LIGHTBAR_SETUP_CONTROL_ENABLE;
	    if (state)
		rp.common->lightbar_setup = DS_OUTPUT_LIGHTBAR_SETUP_LIGHT_ON;
	    else 
		rp.common->lightbar_setup = DS_OUTPUT_LIGHTBAR_SETUP_LIGHT_OUT;

	    dualsense_send_output_report(ds, &rp);

	    return 0;
	}

	int dualsense_set_lightbar_rgb(struct dualsense *ds, uint8_t red, uint8_t green, uint8_t blue, uint8_t brightness)
	{
	    struct dualsense_output_report rp;
	    uint8_t rbuf[DS_OUTPUT_REPORT_BT_SIZE];
	    dualsense_init_output_report(ds, &rp, rbuf);

	    uint8_t max_brightness = 255;

	    rp.common->valid_flag1 = DS_OUTPUT_VALID_FLAG1_LIGHTBAR_CONTROL_ENABLE;
	    rp.common->lightbar_red = brightness * red / max_brightness;
	    rp.common->lightbar_green = brightness * green / max_brightness;
	    rp.common->lightbar_blue = brightness * blue / max_brightness;

	    dualsense_send_output_report(ds, &rp);

	    return 0;
	}

	int dualsense_set_led_brightness(struct dualsense *ds, bool number)
	{
	    struct dualsense_output_report rp;
	    uint8_t rbuf[DS_OUTPUT_REPORT_BT_SIZE];
	    dualsense_init_output_report(ds, &rp, rbuf);

	    rp.common->valid_flag2 = DS_OUTPUT_VALID_FLAG2_LED_BRIGHTNESS_CONTROL_ENABLE;
	    rp.common->led_brightness = (uint8_t)number;

	    dualsense_send_output_report(ds, &rp);

	    return 0;
	}

	int dualsense_set_player_leds(struct dualsense *ds, uint8_t number, bool instant)
	{
	    const int player_ids[] = {
		0,
		BIT(2),
		BIT(3) | BIT(1),
		BIT(4) | BIT(2) | BIT(0),
		BIT(4) | BIT(3) | BIT(1) | BIT(0),
		BIT(4) | BIT(3) | BIT(2) | BIT(1) | BIT(0),
		BIT(4) | BIT(0),
		BIT(3) | BIT(2) | BIT(1),
	    };

	    if (number >= sizeof(player_ids)/sizeof(*player_ids)) {
		fprintf(stderr, "Invalid player number\n");
		return 1;
	    }

	    struct dualsense_output_report rp;
	    uint8_t rbuf[DS_OUTPUT_REPORT_BT_SIZE];
	    dualsense_init_output_report(ds, &rp, rbuf);

	    rp.common->valid_flag1 = DS_OUTPUT_VALID_FLAG1_PLAYER_INDICATOR_CONTROL_ENABLE;
	    rp.common->player_leds = player_ids[number] | (instant << 5);

	    dualsense_send_output_report(ds, &rp);

	    return 0;
	}

	int dualsense_set_microphone_state(struct dualsense *ds, bool state)
	{
	    struct dualsense_output_report rp;
	    uint8_t rbuf[DS_OUTPUT_REPORT_BT_SIZE];
	    dualsense_init_output_report(ds, &rp, rbuf);

	    rp.common->valid_flag1 = DS_OUTPUT_VALID_FLAG1_POWER_SAVE_CONTROL_ENABLE;
	    /* i see once again we are using a STRING FOR A VALUE THAT SHOULD BE A BOOLEAN
	    if (!strcmp(state, "on")) {
		rp.common->power_save_control &= ~DS_OUTPUT_POWER_SAVE_CONTROL_MIC_MUTE & ~DS_OUTPUT_POWER_SAVE_CONTROL_AUDIO;
	    } else if (!strcmp(state, "off")) {
		rp.common->power_save_control |= DS_OUTPUT_POWER_SAVE_CONTROL_MIC_MUTE;
	    } else {
		fprintf(stderr, "Invalid state\n");
		return 1;
	    }
	    */
	    if (state)
		rp.common->power_save_control &= ~DS_OUTPUT_POWER_SAVE_CONTROL_MIC_MUTE & ~DS_OUTPUT_POWER_SAVE_CONTROL_AUDIO;
	    else
		rp.common->power_save_control |= DS_OUTPUT_POWER_SAVE_CONTROL_MIC_MUTE;
	    dualsense_send_output_report(ds, &rp);
	    return 0;
	}

	int dualsense_set_microphone_led_status(struct dualsense *ds, uint8_t state)
	{
	    struct dualsense_output_report rp;
	    uint8_t rbuf[DS_OUTPUT_REPORT_BT_SIZE];
	    dualsense_init_output_report(ds, &rp, rbuf);

	    rp.common->valid_flag1 = DS_OUTPUT_VALID_FLAG1_MIC_MUTE_LED_CONTROL_ENABLE;
	    if (state > 2)
		    // there are only 3 states the led can be in
		    return DS_INVALID;
	    rp.common->mute_button_led = state;

	    dualsense_send_output_report(ds, &rp);

	    return 0;
	}
	// not implementing allat
	/*
	int command_microphone_mode(struct dualsense *ds, char *state)
	{
	    struct dualsense_output_report rp;
	    uint8_t rbuf[DS_OUTPUT_REPORT_BT_SIZE];
	    dualsense_init_output_report(ds, &rp, rbuf);

	    rp.common->valid_flag0 = DS_OUTPUT_VALID_FLAG0_AUDIO_CONTROL_ENABLE;
	    if (!strcmp(state, "chat")) {
		rp.common->audio_flags = 1 << DS_OUTPUT_AUDIO_INPUT_PATH_SHIFT;
	    } else if (!strcmp(state, "asr")) {
		rp.common->audio_flags = 2 << DS_OUTPUT_AUDIO_INPUT_PATH_SHIFT;
	    } else if (!strcmp(state, "both")) {
		rp.common->audio_flags = 0;
	    } else {
		fprintf(stderr, "Invalid state\n");
		return 1;
	    }

	    dualsense_send_output_report(ds, &rp);

	    return 0;
	}

	int command_microphone_volume(struct dualsense *ds, uint8_t volume)
	{
	    struct dualsense_output_report rp;
	    uint8_t rbuf[DS_OUTPUT_REPORT_BT_SIZE];
	    dualsense_init_output_report(ds, &rp, rbuf);

	    rp.common->valid_flag0 = DS_OUTPUT_VALID_FLAG0_MICROPHONE_VOLUME_ENABLE;
	    rp.common->internal_microphone_volume = volume;

	    dualsense_send_output_report(ds, &rp);

	    return 0;
	}
	*/
	/*int command_speaker(struct dualsense *ds, char *state)
	{
	    struct dualsense_output_report rp;
	    uint8_t rbuf[DS_OUTPUT_REPORT_BT_SIZE];
	    dualsense_init_output_report(ds, &rp, rbuf);

	    rp.common->valid_flag0 = DS_OUTPUT_VALID_FLAG0_AUDIO_CONTROL_ENABLE;*/
	    /* value
	     * | /left headphone
	     * | | / right headphone
	     * | | | / internal speaker
	     * 0 L_R_X
	     * 1 L_L_X
	     * 2 L_L_R
	     * 3 X_X_R
	     *//*
	    if (!strcmp(state, "internal")) { // right channel to speaker
		rp.common->audio_flags = 3 << DS_OUTPUT_AUDIO_OUTPUT_PATH_SHIFT;
	    } else if (!strcmp(state, "headphone")) { // stereo channel to headphone 
		rp.common->audio_flags = 0;
	    } else if (!strcmp(state, "monoheadphone")) { // left channel to headphone
		rp.common->audio_flags = 1 << DS_OUTPUT_AUDIO_OUTPUT_PATH_SHIFT;
	    } else if (!strcmp(state, "both")) { // left channel to headphone, right channel to speaker
		rp.common->audio_flags = 2 << DS_OUTPUT_AUDIO_OUTPUT_PATH_SHIFT;
	    } else {
		fprintf(stderr, "Invalid state\n");
		return 1;
	    }

	    dualsense_send_output_report(ds, &rp);

	    return 0;
	}

	int command_volume(struct dualsense *ds, uint8_t volume)
	{
	    struct dualsense_output_report rp;
	    uint8_t rbuf[DS_OUTPUT_REPORT_BT_SIZE];
	    dualsense_init_output_report(ds, &rp, rbuf);

	    uint8_t max_volume = 255;

	    // TODO see if we can get old values of volumes to be able to set values independently
	    rp.common->valid_flag0 = DS_OUTPUT_VALID_FLAG0_HEADPHONE_VOLUME_ENABLE;
	    rp.common->headphone_audio_volume = volume * 0x7f / max_volume;

	    rp.common->valid_flag0 |= DS_OUTPUT_VALID_FLAG0_SPEAKER_VOLUME_ENABLE;
	    // the PS5 use 0x3d-0x64. trying over 0x64 doesnt change anything, but below 0x3d can still lower the volume
	    rp.common->speaker_audio_volume = volume * 0x64 / max_volume;

	    // if we want to set speaker pre gain
	    //rp.common->valid_flag1 = DS_OUTPUT_VALID_FLAG1_AUDIO_CONTROL2_ENABLE;
	    //rp.common->audio_flags2 = (3 << DS_OUTPUT_AUDIO2_SPEAKER_PREGAIN_SHIFT);

	    dualsense_send_output_report(ds, &rp);

	    return 0;
	}
	*/
	int dualsense_set_vibration_attenuation(struct dualsense *ds, uint8_t rumble_attenuation, uint8_t trigger_attenuation)
	{
	    struct dualsense_output_report rp;
	    uint8_t rbuf[DS_OUTPUT_REPORT_BT_SIZE];
	    dualsense_init_output_report(ds, &rp, rbuf);

	    /* need to store or get current values if we want to change motor/haptic and trigger separately */
	    rp.common->valid_flag1 = DS_OUTPUT_VALID_FLAG1_VIBRATION_ATTENUATION_ENABLE;
	    rp.common->reduce_motor_power = (uint8_t)((rumble_attenuation & 0x07) | ((trigger_attenuation & 0x07) << 4 ));

	    dualsense_send_output_report(ds, &rp);

	    return 0;
	}
	// TODO:  label the parameters
	int dualsense_command_trigger(struct dualsense *ds, uint8_t trigger, uint8_t mode, uint8_t param1, uint8_t param2, uint8_t param3, uint8_t param4, uint8_t param5, uint8_t param6, uint8_t param7, uint8_t param8, uint8_t param9 )
	{
	    struct dualsense_output_report rp;
	    uint8_t rbuf[DS_OUTPUT_REPORT_BT_SIZE];
	    dualsense_init_output_report(ds, &rp, rbuf);
	    bool is_both = (trigger == DS_TRIGGER_BOTH);
	    if (trigger != DS_TRIGGER_RIGHT || !is_both) {
		rp.common->valid_flag0 = DS_OUTPUT_VALID_FLAG0_RIGHT_TRIGGER_MOTOR_ENABLE;
	    }
	    if (trigger != DS_TRIGGER_LEFT || !is_both) {
		rp.common->valid_flag0 |= DS_OUTPUT_VALID_FLAG0_LEFT_TRIGGER_MOTOR_ENABLE;
	    }
	    // huh. so that flag appears to just make the controller FW ignore the output report for that trigger. cool.
	    rp.common->right_trigger_motor_mode = mode;
	    rp.common->right_trigger_param[0] = param1;
	    rp.common->right_trigger_param[1] = param2;
	    rp.common->right_trigger_param[2] = param3;
	    rp.common->right_trigger_param[3] = param4;
	    rp.common->right_trigger_param[4] = param5;
	    rp.common->right_trigger_param[5] = param6;
	    rp.common->right_trigger_param[6] = param7;
	    rp.common->right_trigger_param[7] = param8;
	    rp.common->right_trigger_param[8] = param9;

	    rp.common->left_trigger_motor_mode = mode;
	    rp.common->left_trigger_param[0] = param1;
	    rp.common->left_trigger_param[1] = param2;
	    rp.common->left_trigger_param[2] = param3;
	    rp.common->left_trigger_param[3] = param4;
	    rp.common->left_trigger_param[4] = param5;
	    rp.common->left_trigger_param[5] = param6;
	    rp.common->left_trigger_param[6] = param7;
	    rp.common->left_trigger_param[7] = param8;
	    rp.common->left_trigger_param[8] = param9;

	    dualsense_send_output_report(ds, &rp);

	    return 0;
	}

	int dualsense_set_trigger_off(struct dualsense *ds, uint8_t trigger)
	{
	    return dualsense_command_trigger(ds, trigger, DS_TRIGGER_EFFECT_OFF, 0, 0, 0, 0, 0, 0, 0, 0, 0);
	}

	static int trigger_bitpacking_array(struct dualsense *ds, uint8_t trigger, uint8_t mode, uint8_t strength[10], uint8_t frequency)
	{
	    uint32_t strength_zones = 0;
	    uint16_t active_zones = 0;
	    for (int i = 0; i < 10; i++) {
		if (strength[i] > 8) {
		    fprintf(stderr, "strengths must be between 0 and 8\n");
		    return 1;
		}
		if (strength[i] > 0) {
		    uint8_t strength_value = (uint8_t)((strength[i] -1) & 0x07);
		    strength_zones |= (uint32_t)(strength_value << (3 * i));
		    active_zones |= (uint16_t)(1 << i);
		}
	    }

	    return dualsense_command_trigger(ds, trigger, mode,
				   (uint8_t)((active_zones >> 0) & 0xff),
				   (uint8_t)((active_zones >> 8) & 0xff),
				   (uint8_t)((strength_zones >> 0) & 0xff),
				   (uint8_t)((strength_zones >> 8) & 0xff),
				   (uint8_t)((strength_zones >> 16) & 0xff),
				   (uint8_t)((strength_zones >> 24) & 0xff),
				   0, 0,
				   frequency);
	}

	int dualsense_command_trigger_feedback(struct dualsense *ds, uint8_t trigger, uint8_t position, uint8_t strength)
	{
	    if (position > 9) {
		fprintf(stderr, "position must be between 0 and 9\n");
		return 1;
	    }
	    if (strength > 8 || !(strength > 0)) {
		fprintf(stderr, "strength must be between 1 and 8\n");
		return 1;
	    }
	    uint8_t strength_array[10] = {0};
	    for (int i = position; i < 10; i++) {
		strength_array[i] = strength;
	    }

	    return trigger_bitpacking_array(ds, trigger, DS_TRIGGER_EFFECT_FEEDBACK, strength_array, 0);
	}

	int dualsense_command_trigger_weapon(struct dualsense *ds, uint8_t trigger, uint8_t start_position, uint8_t end_position, uint8_t strength)
	{
	    if (start_position > 7 || start_position < 2) {
		fprintf(stderr, "start position must be between 2 and 7\n");
		return 1;
	    }
	    if (end_position > 8 || end_position < start_position+1) {
		fprintf(stderr, "end position must be between start position+1 and 8\n");
		return 1;
	    }
	    if (strength > 8 || !(strength > 0)) {
		fprintf(stderr, "strength must be between 1 and 8\n");
		return 1;
	    }

	    uint16_t start_stop_zones = (uint16_t)((1 << start_position) | (1 << end_position));
	    return dualsense_command_trigger(ds, trigger, DS_TRIGGER_EFFECT_WEAPON,
				   (uint8_t)((start_stop_zones >> 0) & 0xff),
				   (uint8_t)((start_stop_zones >> 8) & 0xff),
				   strength-1,
				   0, 0, 0, 0, 0, 0);
	}

	int dualsense_command_trigger_bow(struct dualsense *ds, uint8_t trigger, uint8_t start_position, uint8_t end_position, uint8_t strength, uint8_t snap_force)
	{
	    if (start_position > 8 || !(start_position > 0)) {
		fprintf(stderr, "start position must be between 0 and 8\n");
		return 1;
	    }
	    if (end_position > 8 || end_position < start_position+1) {
		fprintf(stderr, "end position must be between start position+1 and 8\n");
		return 1;
	    }
	    if (strength > 8 || !(strength > 0)) {
		fprintf(stderr, "strength must be between 1 and 8\n");
		return 1;
	    }
	    if (snap_force > 8 || !(snap_force > 0)) {
		fprintf(stderr, "snap_force must be between 1 and 8\n");
		return 1;
	    }

	    uint16_t start_stop_zones = (uint16_t)((1 << start_position) | (1 << end_position));
	    uint32_t force_pair =  (uint16_t)(((strength -1) & 0x07) | (((snap_force -1 ) & 0x07) << 3 ));
	    return dualsense_command_trigger(ds, trigger, DS_TRIGGER_EFFECT_BOW,
				   (uint8_t)((start_stop_zones >> 0) & 0xff),
				   (uint8_t)((start_stop_zones >> 8) & 0xff),
				   (uint8_t)((force_pair >> 0) & 0xff),
				   0, 0, 0, 0, 0, 0);
	}

	int dualsense_command_trigger_galloping(struct dualsense *ds, uint8_t trigger, uint8_t start_position, uint8_t end_position, uint8_t first_foot, uint8_t second_foot, uint8_t frequency)
	{
	    if (start_position > 8) {
		fprintf(stderr, "start position must be between 0 and 8\n");
		return 1;
	    }
	    if (end_position > 9 || end_position < start_position+1) {
		fprintf(stderr, "end position must be between start position+1 and 9\n");
		return 1;
	    }
	    if (first_foot > 6) {
		fprintf(stderr, "first_foot must be between 0 and 8\n");
		return 1;
	    }
	    if (second_foot > 7 || second_foot < first_foot+1) {
		fprintf(stderr, "second_foot must be between first_foot+1 and 8\n");
		return 1;
	    }

	    if (!(frequency > 0)) {
		fprintf(stderr, "frequency must be greater than 0\n");
		return 1;
	    }
	    if (frequency > 8) {
		fprintf(stdout, "frequency has a better effect when lower than 8\n");
	    }
	    uint16_t start_stop_zones = (uint16_t)((1 << start_position) | (1 << end_position));
	    uint32_t ratio =  (uint16_t)((second_foot & 0x07) | ((first_foot & 0x07) << 3 ));
	    return dualsense_command_trigger(ds, trigger, DS_TRIGGER_EFFECT_GALLOPING,
				   (uint8_t)((start_stop_zones >> 0) & 0xff),
				   (uint8_t)((start_stop_zones >> 8) & 0xff),
				   (uint8_t)((ratio >> 0) & 0xff),
				   frequency,
				   0, 0, 0, 0, 0);
	}

	int dualsense_command_trigger_machine(struct dualsense *ds, uint8_t trigger, uint8_t start_position, uint8_t end_position, uint8_t strength_a, uint8_t strength_b, uint8_t frequency, uint8_t period)
	{
	    // if start_position == 0 nothing happen
	    if (start_position > 8 || !(start_position > 0)) {
		fprintf(stderr, "start position must be between 1 and 8\n");
		return 1;
	    }
	    if (end_position > 9 || end_position < start_position+1) {
		fprintf(stderr, "end position must be between start position+1 and 9\n");
		return 1;
	    }
	    if (strength_a > 7) {
		fprintf(stderr, "strength_a position must be between 0 and 7\n");
		return 1;
	    }
	    if (strength_b > 7) {
		fprintf(stderr, "strength_b position must be between 0 and 7\n");
		return 1;
	    }
	    if (!(frequency > 0)) {
		fprintf(stderr, "frequency must be greater than 0\n");
		return 1;
	    }
	    uint16_t start_stop_zones = (uint16_t)((1 << start_position) | (1 << end_position));
	    uint32_t force_pair =  (uint16_t)((strength_a & 0x07) | ((strength_b & 0x07) << 3 ));
	    return dualsense_command_trigger(ds, trigger, DS_TRIGGER_EFFECT_MACHINE,
				   (uint8_t)((start_stop_zones >> 0) & 0xff),
				   (uint8_t)((start_stop_zones >> 8) & 0xff),
				   (uint8_t)((force_pair >> 0) & 0xff),
				   frequency,
				   period,
				   0, 0, 0, 0);
	}

	int dualsense_command_trigger_vibration(struct dualsense *ds, uint8_t trigger, uint8_t position, uint8_t amplitude, uint8_t frequency)
	{
	    if (position > 9) {
		fprintf(stderr, "position must be between 0 and 9\n");
		return 1;
	    }
	    if (amplitude > 8 || !(amplitude > 0)) {
		fprintf(stderr, "amplitude must be between 1 and 8\n");
		return 1;
	    }
	    if (!(frequency > 0)) {
		fprintf(stderr, "frequency must be greater than 0\n");
		return 1;
	    }

	    uint8_t strength_array[10] = {0};
	    for (int i = position; i < 10; i++) {
		strength_array[i] = amplitude;
	    }
	    return trigger_bitpacking_array(ds, trigger, DS_TRIGGER_EFFECT_VIBRATION, strength_array, frequency);

	}

	int dualsense_command_trigger_feedback_raw(struct dualsense *ds, uint8_t trigger, uint8_t strength[10] )
	{
	    return trigger_bitpacking_array(ds, trigger, DS_TRIGGER_EFFECT_FEEDBACK, strength, 0);
	}

	int dualsense_command_trigger_vibration_raw(struct dualsense *ds, uint8_t trigger, uint8_t strength[10], uint8_t frequency)
	{
	    return trigger_bitpacking_array(ds, trigger, DS_TRIGGER_EFFECT_VIBRATION, strength, frequency);
	}
/* debug function
int dualsense_list_devices(void)
{
    struct hid_device_info *devs = dualsense_hid_enumerate();
    if (!devs) {
        fprintf(stderr, "No devices found\n");
        return 1;
    }
    printf("Devices:\n");
    struct hid_device_info *dev = devs;
    while (dev) {
        printf(" %ls (%s)\n", dev->serial_number ? dev->serial_number : L"???", dev->interface_number == -1 ? "Bluetooth" : "USB");
        dev = dev->next;
    }
    return 0;
} */
