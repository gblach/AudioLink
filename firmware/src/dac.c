#include "dac.h"

#include <string.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(dac, LOG_LEVEL_INF);

/* PCM5242 page 0 registers */
#define REG_PAGE      0x00
#define REG_RESET     0x01
#define REG_POWER     0x02
#define REG_PLL_REF   0x0d
#define REG_ERR_DET   0x25
#define REG_I2S_FMT   0x28
#define REG_VOL_L     0x3d
#define REG_VOL_R     0x3e
#define REG_FS_DET    0x5b
#define REG_CLK_DET   0x5e
#define REG_CLK_ERR   0x5f
#define REG_PWR_STATE 0x76

#define RESET_MODULES BIT(4)
#define RESET_REGS    BIT(0)
#define POWER_STANDBY BIT(4)
#define PLL_REF_BCK   (1 << 4)
/* IDCH, what the Linux pcm512x driver sets when there is no SCK */
#define IGNORE_SCK    BIT(3)
#define I2S_32BIT     0x03

/* Digital volume: 0x30 is 0 dB, each step is -0.5 dB, 0xff mutes */
#define VOL_0DB      0x30
#define VOL_MIN_STEP 120
#define VOL_MUTE     0xff

static const struct i2c_dt_spec i2c = I2C_DT_SPEC_GET(DT_NODELABEL(pcm5242));
static const struct gpio_dt_spec power = GPIO_DT_SPEC_GET(DT_NODELABEL(analog), power_gpios);
static const struct gpio_dt_spec mute = GPIO_DT_SPEC_GET(DT_NODELABEL(analog), mute_gpios);

static int reg_write(uint8_t reg, uint8_t val)
{
	int err = i2c_reg_write_byte_dt(&i2c, reg, val);

	if (err) {
		LOG_ERR("Write 0x%02x failed (%d)", reg, err);
	}

	return err;
}

void dac_mute(bool on)
{
	gpio_pin_set_dt(&mute, on);
}

int dac_set_volume(uint8_t volume)
{
	uint8_t val = volume ? VOL_0DB + (255 - volume) * VOL_MIN_STEP / 255 : VOL_MUTE;
	int err;

	err = reg_write(REG_VOL_L, val);
	if (err) {
		return err;
	}

	return reg_write(REG_VOL_R, val);
}

void dac_poll_status(void)
{
	static const char *const states[] = {
		"powerdown", "wait CP", "calibration 1", "calibration 2", "ramp up",
		"run", "output short", "ramp down", "standby",
	};
	static uint8_t last[4] = {0xff, 0xff, 0xff, 0xff};
	const uint8_t regs[] = {REG_PWR_STATE, REG_FS_DET, REG_CLK_DET, REG_CLK_ERR};
	uint8_t now[ARRAY_SIZE(regs)];
	uint8_t state;

	for (size_t i = 0; i < ARRAY_SIZE(regs); i++) {
		if (i2c_reg_read_byte_dt(&i2c, regs[i], &now[i]) != 0) {
			return;
		}
	}

	if (memcmp(now, last, sizeof(now)) == 0) {
		return;
	}

	memcpy(last, now, sizeof(now));
	state = now[0] & 0x0f;
	LOG_INF("PCM5242 %s, fs detect 0x%02x, clock detect 0x%02x, clock error 0x%02x",
		state < ARRAY_SIZE(states) ? states[state] : "?", now[1], now[2], now[3]);
}

static int configure(void)
{
	static const uint8_t seq[][2] = {
		{REG_PAGE, 0},
		{REG_RESET, RESET_MODULES | RESET_REGS},
		{REG_RESET, 0},
		{REG_POWER, POWER_STANDBY},
		/* SCK is tied to ground, so the PLL runs from BCK */
		{REG_PLL_REF, PLL_REF_BCK},
		{REG_ERR_DET, IGNORE_SCK},
		{REG_I2S_FMT, I2S_32BIT},
		{REG_VOL_L, VOL_0DB},
		{REG_VOL_R, VOL_0DB},
		{REG_POWER, 0},
	};
	int err;

	for (size_t i = 0; i < ARRAY_SIZE(seq); i++) {
		err = reg_write(seq[i][0], seq[i][1]);
		if (err) {
			return err;
		}
	}

	return 0;
}

int dac_init(void)
{
	uint8_t clk_det;
	int err;

	if (!i2c_is_ready_dt(&i2c) || !gpio_is_ready_dt(&power) || !gpio_is_ready_dt(&mute)) {
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&mute, GPIO_OUTPUT_ACTIVE);
	if (err) {
		return err;
	}

	err = gpio_pin_configure_dt(&power, GPIO_OUTPUT_ACTIVE);
	if (err) {
		return err;
	}

	/* TPS65133 and TLV75533 soft start, plus PCM5242 power-on reset */
	k_sleep(K_MSEC(50));

	err = configure();
	if (err) {
		return err;
	}

	/* Let the PLL lock before reading the clock detection status */
	k_sleep(K_MSEC(10));

	err = i2c_reg_read_byte_dt(&i2c, REG_CLK_DET, &clk_det);
	if (err) {
		return err;
	}

	LOG_INF("PCM5242 ready, clock detection status 0x%02x", clk_det);

	return 0;
}
