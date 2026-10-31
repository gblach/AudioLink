/* Custom Bluetooth service for user settings, only paired phones may read or write them */

#include <string.h>

#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>

#include "eq.h"
#include "leds.h"

LOG_MODULE_REGISTER(control, LOG_LEVEL_INF);

#define UUID(n) BT_UUID_DECLARE_128(BT_UUID_128_ENCODE(n, 0x7a3c, 0x4f5e, 0x9d2a, 0x3c8f1b6e4a20))

#define SVC_UUID        UUID(0x6b1e0000)
#define GAINS_UUID      UUID(0x6b1e0001)
#define LED_MAX_UUID    UUID(0x6b1e0002)
#define LED_THEME_UUID  UUID(0x6b1e0003)

#define PERM (BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT)

/* A flash write stalls the CPU long enough for a short audio gap */
#define SAVE_DELAY K_SECONDS(2)

enum unsaved {
	UNSAVED_GAINS,
	UNSAVED_LED_MAX,
	UNSAVED_LED_THEME,
	UNSAVED_COUNT,
};

static int8_t gains[EQ_BANDS];
static uint8_t led_max = LEDS_MAX_PERCENT_DEFAULT;
static ATOMIC_DEFINE(unsaved, UNSAVED_COUNT);

static bool gains_valid(const int8_t *val)
{
	for (int i = 0; i < EQ_BANDS; i++) {
		if (val[i] < -EQ_GAIN_MAX_DB || val[i] > EQ_GAIN_MAX_DB) {
			return false;
		}
	}

	return true;
}

static void save(const char *key, const void *val, size_t len)
{
	int err = settings_save_one(key, val, len);

	if (err) {
		LOG_WRN("Saving %s failed (%d)", key, err);
	}
}

static void save_work_handler(struct k_work *work)
{
	uint8_t theme = leds_theme();

	if (atomic_test_and_clear_bit(unsaved, UNSAVED_GAINS)) {
		save("eq/gains", gains, sizeof(gains));
	}

	if (atomic_test_and_clear_bit(unsaved, UNSAVED_LED_MAX)) {
		save("leds/max", &led_max, sizeof(led_max));
	}

	if (atomic_test_and_clear_bit(unsaved, UNSAVED_LED_THEME)) {
		save("leds/theme", &theme, sizeof(theme));
	}
}

static K_WORK_DELAYABLE_DEFINE(save_work, save_work_handler);

/* A slider sends many values, flash is only written once the last one is SAVE_DELAY old */
static void save_later(enum unsaved setting)
{
	atomic_set_bit(unsaved, setting);
	k_work_reschedule(&save_work, SAVE_DELAY);
}

static int eq_settings_set(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	int8_t val[EQ_BANDS];

	if (!settings_name_steq(key, "gains", NULL) || len != sizeof(val)) {
		return -ENOENT;
	}

	if (read_cb(cb_arg, val, sizeof(val)) != sizeof(val) || !gains_valid(val)) {
		return -EINVAL;
	}

	memcpy(gains, val, sizeof(gains));
	eq_set_gains(gains);

	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(eq, "eq", NULL, eq_settings_set, NULL, NULL);

static int leds_settings_set(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	uint8_t val;

	if (len != sizeof(val)) {
		return -ENOENT;
	}

	if (settings_name_steq(key, "theme", NULL)) {
		if (read_cb(cb_arg, &val, sizeof(val)) != sizeof(val) || val >= LEDS_THEME_COUNT) {
			return -EINVAL;
		}

		leds_set_theme(val);

		return 0;
	}

	if (!settings_name_steq(key, "max", NULL)) {
		return -ENOENT;
	}

	if (read_cb(cb_arg, &val, sizeof(val)) != sizeof(val) || val > 100) {
		return -EINVAL;
	}

	led_max = val;
	leds_set_max_percent(led_max);

	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(leds, "leds", NULL, leds_settings_set, NULL, NULL);

static ssize_t read_gains(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			  uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, gains, sizeof(gains));
}

static ssize_t write_gains(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			   const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	if (len != sizeof(gains)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	if (!gains_valid(buf)) {
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}

	memcpy(gains, buf, sizeof(gains));
	eq_set_gains(gains);
	save_later(UNSAVED_GAINS);

	return len;
}

static ssize_t read_led_max(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			    uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, &led_max, sizeof(led_max));
}

static ssize_t write_led_max(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			     const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	uint8_t val;

	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	if (len != sizeof(val)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	val = *(const uint8_t *)buf;
	if (val > 100) {
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}

	led_max = val;
	leds_set_max_percent(led_max);
	save_later(UNSAVED_LED_MAX);
	LOG_INF("LED max brightness %u%%", led_max);

	return len;
}

static ssize_t read_led_theme(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			      uint16_t len, uint16_t offset)
{
	uint8_t theme = leds_theme();

	return bt_gatt_attr_read(conn, attr, buf, len, offset, &theme, sizeof(theme));
}

static ssize_t write_led_theme(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			       const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	uint8_t theme;

	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	if (len != sizeof(theme)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	theme = *(const uint8_t *)buf;
	if (theme >= LEDS_THEME_COUNT) {
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}

	leds_set_theme(theme);
	save_later(UNSAVED_LED_THEME);
	LOG_INF("LED theme %u", theme);

	return len;
}

BT_GATT_SERVICE_DEFINE(control_svc, BT_GATT_PRIMARY_SERVICE(SVC_UUID),
		       BT_GATT_CHARACTERISTIC(GAINS_UUID, BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
					      PERM, read_gains, write_gains, NULL),
		       BT_GATT_CUD("EQ dB: 60 Hz, 250 Hz, 1 kHz, 4 kHz, 12 kHz",
				   BT_GATT_PERM_READ),
		       BT_GATT_CHARACTERISTIC(LED_MAX_UUID, BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
					      PERM, read_led_max, write_led_max, NULL),
		       BT_GATT_CUD("LED max brightness, % of full drive", BT_GATT_PERM_READ),
		       BT_GATT_CHARACTERISTIC(LED_THEME_UUID, BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
					      PERM, read_led_theme, write_led_theme, NULL),
		       BT_GATT_CUD("LED theme: 0 amber/magenta, 1 blue/cyan, 2 green/lime, "
				   "3 amber/yellow",
				   BT_GATT_PERM_READ));
