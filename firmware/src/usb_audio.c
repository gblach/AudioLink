#include "usb_audio.h"

#include <zephyr/drivers/usb/udc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/usb/class/usbd_uac2.h>
#include <zephyr/usb/usbd.h>

#include "audio_out.h"

LOG_MODULE_REGISTER(usb_audio, LOG_LEVEL_INF);

/* Zephyr project VID with a free PID, fine for development but not for shipping */
#define USB_VID 0x2fe3
#define USB_PID 0xa0d1
/* 2 mA units */
#define USB_MAX_POWER 125

#define FRAME_BYTES     (2 * sizeof(int16_t))
#define FRAMES_PER_SOF  (AUDIO_OUT_RATE_HZ / 1000)
#define MAX_PACKET      ((FRAMES_PER_SOF + 1) * FRAME_BYTES)
#define RX_BUFFERS      4

/* Full-Speed feedback is Q10.14 samples per frame */
#define FB_NOMINAL      (FRAMES_PER_SOF << 14)
#define FB_MAX_DEV      (FB_NOMINAL / 200)
/*
 * Gain in Q14 per frame of fill error. 8 gives a time constant of about 2 s,
 * far slower than the fill average below.
 */
#define FB_GAIN         8
/* Fill average in Q8, filtered over about 64 SOFs */
#define FILL_AVG_SHIFT  6

USBD_DEVICE_DEFINE(usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)), USB_VID, USB_PID);
USBD_DESC_LANG_DEFINE(lang);
USBD_DESC_MANUFACTURER_DEFINE(mfr, "Magik");
USBD_DESC_PRODUCT_DEFINE(product, "AudioLink");
USBD_DESC_SERIAL_NUMBER_DEFINE(sn);
USBD_DESC_CONFIG_DEFINE(fs_cfg_desc, "FS Configuration");
USBD_CONFIGURATION_DEFINE(fs_config, 0, USB_MAX_POWER, &fs_cfg_desc);

K_MEM_SLAB_DEFINE_STATIC(rx_slab, ROUND_UP(MAX_PACKET, UDC_BUF_GRANULARITY), RX_BUFFERS,
			 UDC_BUF_ALIGN);

static int32_t fill_avg;

static void terminal_update_cb(const struct device *dev, uint8_t terminal, bool enabled,
			       bool microframes, void *user_data)
{
	LOG_INF("Host %s streaming", enabled ? "started" : "stopped");

	fill_avg = audio_out_start_level(AUDIO_SRC_USB) << 8;
	audio_out_request(AUDIO_SRC_USB, enabled);
}

static void *get_recv_buf(const struct device *dev, uint8_t terminal, uint16_t size,
			  void *user_data)
{
	void *buf;

	if (size > MAX_PACKET || k_mem_slab_alloc(&rx_slab, &buf, K_NO_WAIT) != 0) {
		return NULL;
	}

	return buf;
}

static void data_recv_cb(const struct device *dev, uint8_t terminal, void *buf, uint16_t size,
			 void *user_data)
{
	audio_out_write(AUDIO_SRC_USB, buf, size / FRAME_BYTES);
	k_mem_slab_free(&rx_slab, buf);
}

static void buf_release_cb(const struct device *dev, uint8_t terminal, void *buf,
			   void *user_data)
{
	/* Only called for data sent to the host, which this device never does */
}

static void sof_cb(const struct device *dev, void *user_data)
{
	int32_t fill = audio_out_fill() << 8;

	fill_avg += (fill - fill_avg) >> FILL_AVG_SHIFT;
}

/*
 * The audio clock free-runs while USB is the source, so the host is asked to
 * send more or fewer samples to hold the fill level near the start level.
 */
static uint32_t feedback_cb(const struct device *dev, uint8_t terminal, void *user_data)
{
	int32_t err = (fill_avg >> 8) - (int32_t)audio_out_start_level(AUDIO_SRC_USB);

	return FB_NOMINAL - CLAMP(err * FB_GAIN, -FB_MAX_DEV, FB_MAX_DEV);
}

static const struct uac2_ops ops = {
	.sof_cb = sof_cb,
	.terminal_update_cb = terminal_update_cb,
	.get_recv_buf = get_recv_buf,
	.data_recv_cb = data_recv_cb,
	.buf_release_cb = buf_release_cb,
	.feedback_cb = feedback_cb,
};

int usb_audio_init(void)
{
	struct usbd_desc_node *const descs[] = {&lang, &mfr, &product, &sn};
	int err;

	usbd_uac2_set_ops(DEVICE_DT_GET(DT_NODELABEL(uac2_out)), &ops, NULL);

	for (size_t i = 0; i < ARRAY_SIZE(descs); i++) {
		err = usbd_add_descriptor(&usbd, descs[i]);
		if (err) {
			return err;
		}
	}

	err = usbd_add_configuration(&usbd, USBD_SPEED_FS, &fs_config);
	if (err) {
		return err;
	}

	err = usbd_register_all_classes(&usbd, USBD_SPEED_FS, 1, NULL);
	if (err) {
		return err;
	}

	/* UAC2 uses an Interface Association Descriptor */
	usbd_device_set_code_triple(&usbd, USBD_SPEED_FS, USB_BCC_MISCELLANEOUS, 0x02, 0x01);

	err = usbd_init(&usbd);
	if (err) {
		return err;
	}

	return usbd_enable(&usbd);
}
