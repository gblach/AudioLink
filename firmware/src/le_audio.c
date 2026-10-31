#include "le_audio.h"

#include <string.h>

#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/bluetooth/audio/bap.h>
#include <zephyr/bluetooth/audio/pacs.h>
#include <zephyr/bluetooth/audio/vcp.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>

#include "lc3_rx.h"

LOG_MODULE_REGISTER(le_audio, LOG_LEVEL_INF);

#define SUPPORTED_FREQ                                                                             \
	(BT_AUDIO_CODEC_CAP_FREQ_16KHZ | BT_AUDIO_CODEC_CAP_FREQ_24KHZ |                           \
	 BT_AUDIO_CODEC_CAP_FREQ_32KHZ | BT_AUDIO_CODEC_CAP_FREQ_48KHZ)
#define SUPPORTED_DURATION (BT_AUDIO_CODEC_CAP_DURATION_7_5 | BT_AUDIO_CODEC_CAP_DURATION_10)
#define MIN_SDU            30U
#define MAX_SDU            155U
#define FRAMES_PER_SDU     1
#define CONTEXTS           BT_AUDIO_CONTEXT_TYPE_ANY
#define LOCATIONS          (BT_AUDIO_LOCATION_FRONT_LEFT | BT_AUDIO_LOCATION_FRONT_RIGHT)
#define MAX_BIS            CONFIG_BT_BAP_BROADCAST_SNK_STREAM_COUNT

/* Roughly -12 dB until the phone sets its own volume */
#define INITIAL_VOLUME 200

#define PA_SYNC_SKIP           5
#define PA_SYNC_TIMEOUT_FACTOR 20

#define BIG_SYNC_TIMEOUT K_SECONDS(10)
#define SKIP_TIME_MS     (60 * MSEC_PER_SEC)
#define SKIP_COUNT       4

struct stream {
	struct bt_bap_stream bap;
	struct lc3_rx_stream rx;
};

enum bc_flag {
	BC_SCANNING,
	BC_PA_SYNCING,
	BC_PA_SYNCED,
	BC_BASE_RECEIVED,
	BC_SYNCABLE,
	BC_CODE_REQUIRED,
	BC_CODE_RECEIVED,
	BC_SYNC_REQUESTED,
	BC_BIG_SYNCING,
	BC_BIG_SYNCED,
	BC_FLAG_COUNT,
};

static struct stream unicast_streams[CONFIG_BT_ASCS_MAX_ASE_SNK_COUNT];
static struct stream broadcast_streams[MAX_BIS];

static struct bt_le_ext_adv *adv;
static struct bt_conn *peer;
static atomic_t volume = ATOMIC_INIT(INITIAL_VOLUME);

static ATOMIC_DEFINE(bc_flags, BC_FLAG_COUNT);
static struct {
	struct bt_bap_broadcast_sink *sink;
	struct bt_le_per_adv_sync *pa_sync;
	const struct bt_bap_scan_delegator_recv_state *recv_state;
	uint8_t code[BT_ISO_BROADCAST_CODE_SIZE];
	uint32_t broadcast_id;
	uint32_t requested_bis;
	/* BIS indexes of the first subgroup in the BASE */
	uint32_t base_bis;
} bc;

/* Broadcasts found by our own scan that could not be played, ignored for a while */
static struct skipped_broadcast {
	uint32_t broadcast_id;
	int64_t until;
} skipped[SKIP_COUNT];

static void pa_timeout_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(pa_timer, pa_timeout_handler);
static void big_timeout_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(big_timer, big_timeout_handler);

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
	BT_DATA_BYTES(BT_DATA_UUID16_SOME, BT_UUID_16_ENCODE(BT_UUID_ASCS_VAL),
		      BT_UUID_16_ENCODE(BT_UUID_CAS_VAL), BT_UUID_16_ENCODE(BT_UUID_BASS_VAL),
		      BT_UUID_16_ENCODE(BT_UUID_VCS_VAL)),
	BT_DATA_BYTES(BT_DATA_SVC_DATA16, BT_UUID_16_ENCODE(BT_UUID_CAS_VAL),
		      BT_AUDIO_UNICAST_ANNOUNCEMENT_GENERAL),
	BT_DATA_BYTES(BT_DATA_SVC_DATA16, BT_UUID_16_ENCODE(BT_UUID_ASCS_VAL),
		      BT_AUDIO_UNICAST_ANNOUNCEMENT_GENERAL, BT_BYTES_LIST_LE16(CONTEXTS),
		      BT_BYTES_LIST_LE16(0) /* No source contexts */,
		      0x00 /* Metadata length */),
	BT_DATA_BYTES(BT_DATA_SVC_DATA16, BT_UUID_16_ENCODE(BT_UUID_BASS_VAL)),
};

/*
 * Zephyr's CAP Acceptor rejects any stream whose metadata names a media control
 * CCID (cap_acceptor.c: "TODO: check mcs"), which is what Android sends for
 * music. Phones still look for the Common Audio Service, so it is registered
 * here without the CAP Acceptor role.
 */
BT_GATT_SERVICE_DEFINE(cas_svc, BT_GATT_PRIMARY_SERVICE(BT_UUID_CAS));

static struct stream *to_stream(struct bt_bap_stream *bap_stream)
{
	return CONTAINER_OF(bap_stream, struct stream, bap);
}

uint8_t le_audio_volume(void)
{
	return atomic_get(&volume);
}

static void stream_started(struct bt_bap_stream *bap_stream)
{
	lc3_rx_start(&to_stream(bap_stream)->rx, bap_stream->codec_cfg);
}

static void stream_stopped(struct bt_bap_stream *bap_stream, uint8_t reason)
{
	LOG_INF("Stream %p stopped, reason 0x%02x", (void *)bap_stream, reason);
	lc3_rx_stop(&to_stream(bap_stream)->rx);
}

static void stream_recv(struct bt_bap_stream *bap_stream, const struct bt_iso_recv_info *info,
			struct net_buf *buf)
{
	lc3_rx_recv(&to_stream(bap_stream)->rx, info, buf);
}

/* Unicast server */

static void unicast_enabled(struct bt_bap_stream *bap_stream)
{
	/* Sink ASEs need the receiver start ready operation from our side */
	int err = bt_bap_stream_start(bap_stream);

	if (err) {
		LOG_ERR("Stream start failed (%d)", err);
	}
}

static struct bt_bap_stream_ops unicast_ops = {
	.enabled = unicast_enabled,
	.started = stream_started,
	.stopped = stream_stopped,
	.recv = stream_recv,
};

static int unicast_config(struct bt_conn *conn, const struct bt_bap_ep *ep, enum bt_audio_dir dir,
			  const struct bt_audio_codec_cfg *codec_cfg,
			  struct bt_bap_stream **bap_stream, struct bt_bap_qos_cfg_pref *const pref,
			  struct bt_bap_ascs_rsp *rsp)
{
	static const struct bt_bap_qos_cfg_pref qos_pref =
		BT_BAP_QOS_CFG_PREF(true, BT_GAP_LE_PHY_2M, 2U, 20U, 20000U, 40000U, 20000U,
				    40000U);

	if (dir != BT_AUDIO_DIR_SINK) {
		*rsp = BT_BAP_ASCS_RSP(BT_BAP_ASCS_RSP_CODE_CONF_UNSUPPORTED,
				       BT_BAP_ASCS_REASON_NONE);
		return -ENOTSUP;
	}

	ARRAY_FOR_EACH_PTR(unicast_streams, s) {
		if (s->bap.ep == NULL) {
			*bap_stream = &s->bap;
			*pref = qos_pref;
			return 0;
		}
	}

	*rsp = BT_BAP_ASCS_RSP(BT_BAP_ASCS_RSP_CODE_NO_MEM, BT_BAP_ASCS_REASON_NONE);

	return -ENOMEM;
}

/* ASCS rejects an operation whose callback is missing, so these just accept */
static int unicast_accept(struct bt_bap_stream *bap_stream, struct bt_bap_ascs_rsp *rsp)
{
	return 0;
}

static int unicast_accept_meta(struct bt_bap_stream *bap_stream, const uint8_t meta[],
			       size_t meta_len, struct bt_bap_ascs_rsp *rsp)
{
	return 0;
}

static const struct bt_bap_unicast_server_cb unicast_server_cb = {
	.config = unicast_config,
	.enable = unicast_accept_meta,
	.start = unicast_accept,
	.metadata = unicast_accept_meta,
	.disable = unicast_accept,
	.stop = unicast_accept,
	.release = unicast_accept,
};

/* Broadcast sink */

static void scan_start(void)
{
	int err;

	if (peer != NULL || atomic_test_bit(bc_flags, BC_PA_SYNCED) ||
	    atomic_test_bit(bc_flags, BC_BIG_SYNCED) ||
	    atomic_test_and_set_bit(bc_flags, BC_SCANNING)) {
		return;
	}

	err = bt_le_scan_start(BT_LE_SCAN_PASSIVE, NULL);
	if (err) {
		LOG_ERR("Scan start failed (%d)", err);
		atomic_clear_bit(bc_flags, BC_SCANNING);
	}
}

static void scan_stop(void)
{
	if (atomic_test_and_clear_bit(bc_flags, BC_SCANNING)) {
		bt_le_scan_stop();
	}
}

static uint32_t bis_to_sync(void)
{
	uint32_t bis = bc.requested_bis == BT_BAP_BIS_SYNC_NO_PREF ? bc.base_bis
								   : bc.requested_bis;
	uint32_t picked = 0;
	int count = 0;

	/* Keep the lowest indexes, at most one per stream we have */
	for (int i = 0; i < 32 && count < MAX_BIS; i++) {
		if (bis & BIT(i)) {
			picked |= BIT(i);
			count++;
		}
	}

	return picked;
}

static void check_big_sync(void)
{
	struct bt_bap_stream *streams[MAX_BIS];
	uint32_t bis;
	int err;

	if (!atomic_test_bit(bc_flags, BC_BASE_RECEIVED) ||
	    !atomic_test_bit(bc_flags, BC_SYNCABLE) ||
	    !atomic_test_bit(bc_flags, BC_SYNC_REQUESTED) ||
	    !atomic_test_bit(bc_flags, BC_PA_SYNCED) ||
	    (atomic_test_bit(bc_flags, BC_CODE_REQUIRED) &&
	     !atomic_test_bit(bc_flags, BC_CODE_RECEIVED)) ||
	    atomic_test_bit(bc_flags, BC_BIG_SYNCING) ||
	    atomic_test_bit(bc_flags, BC_BIG_SYNCED)) {
		return;
	}

	bis = bis_to_sync();
	if (bis == 0) {
		return;
	}

	for (size_t i = 0; i < MAX_BIS; i++) {
		streams[i] = &broadcast_streams[i].bap;
	}

	LOG_INF("Syncing to BIS 0x%08x", bis);

	err = bt_bap_broadcast_sink_sync(bc.sink, bis, streams, bc.code);
	if (err) {
		LOG_ERR("BIG sync failed (%d)", err);
		return;
	}

	atomic_set_bit(bc_flags, BC_BIG_SYNCING);
}

static void broadcast_started(struct bt_bap_stream *bap_stream)
{
	atomic_clear_bit(bc_flags, BC_BIG_SYNCING);
	atomic_set_bit(bc_flags, BC_BIG_SYNCED);
	stream_started(bap_stream);
}

static void broadcast_stopped(struct bt_bap_stream *bap_stream, uint8_t reason)
{
	atomic_clear_bit(bc_flags, BC_BIG_SYNCING);
	atomic_clear_bit(bc_flags, BC_BIG_SYNCED);
	stream_stopped(bap_stream, reason);
	scan_start();
}

static struct bt_bap_stream_ops broadcast_ops = {
	.started = broadcast_started,
	.stopped = broadcast_stopped,
	.recv = stream_recv,
};

static bool first_subgroup_cb(const struct bt_bap_base_subgroup *subgroup, void *user_data)
{
	bt_bap_base_subgroup_get_bis_indexes(subgroup, user_data);

	return false;
}

static void base_recv_cb(struct bt_bap_broadcast_sink *sink, const struct bt_bap_base *base,
			 size_t base_size)
{
	if (atomic_test_and_set_bit(bc_flags, BC_BASE_RECEIVED)) {
		return;
	}

	bc.base_bis = 0;
	bt_bap_base_foreach_subgroup(base, first_subgroup_cb, &bc.base_bis);
	LOG_INF("BASE received, first subgroup BIS 0x%08x", bc.base_bis);

	check_big_sync();
}

static void syncable_cb(struct bt_bap_broadcast_sink *sink, const struct bt_iso_biginfo *biginfo)
{
	atomic_set_bit_to(bc_flags, BC_CODE_REQUIRED, biginfo->encryption);

	if (!atomic_test_and_set_bit(bc_flags, BC_SYNCABLE)) {
		check_big_sync();
	}
}

static void sink_started_cb(struct bt_bap_broadcast_sink *sink)
{
	LOG_INF("Broadcast sink started");
	atomic_clear_bit(bc_flags, BC_SYNC_REQUESTED);
}

/*
 * Without PA there is no BASE or BIGInfo, so everything learned from it goes.
 * The sink object is kept while its BIG is still synced and deleted once it stops.
 */
static void pa_reset(void)
{
	k_work_cancel_delayable(&pa_timer);
	k_work_cancel_delayable(&big_timer);
	bc.pa_sync = NULL;

	for (int f = BC_PA_SYNCING; f <= BC_SYNC_REQUESTED; f++) {
		atomic_clear_bit(bc_flags, f);
	}

	if (bc.sink != NULL && !atomic_test_bit(bc_flags, BC_BIG_SYNCED) &&
	    !atomic_test_bit(bc_flags, BC_BIG_SYNCING) &&
	    bt_bap_broadcast_sink_delete(bc.sink) == 0) {
		bc.sink = NULL;
	}
}

static void pa_drop(void)
{
	if (bc.pa_sync != NULL) {
		bt_le_per_adv_sync_delete(bc.pa_sync);
	}

	pa_reset();
}

static void sink_stopped_cb(struct bt_bap_broadcast_sink *sink, uint8_t reason)
{
	LOG_INF("Broadcast sink stopped, reason 0x%02x", reason);

	atomic_clear_bit(bc_flags, BC_BIG_SYNCING);
	atomic_clear_bit(bc_flags, BC_BIG_SYNCED);

	/*
	 * An assistant may re-sync the same sink to other BISes, so it is only
	 * dropped when we picked the broadcast ourselves or the PA is already gone.
	 */
	if (bc.recv_state == NULL || bc.pa_sync == NULL) {
		pa_drop();
		scan_start();
	}
}

static bool is_skipped(uint32_t broadcast_id)
{
	int64_t now = k_uptime_get();

	ARRAY_FOR_EACH_PTR(skipped, s) {
		if (s->broadcast_id == broadcast_id && s->until > now) {
			return true;
		}
	}

	return false;
}

static void skip_broadcast(uint32_t broadcast_id)
{
	struct skipped_broadcast *oldest = &skipped[0];

	ARRAY_FOR_EACH_PTR(skipped, s) {
		if (s->until < oldest->until) {
			oldest = s;
		}
	}

	oldest->broadcast_id = broadcast_id;
	oldest->until = k_uptime_get() + SKIP_TIME_MS;
}

/*
 * A broadcast we picked ourselves can be one we cannot play, for example an
 * encrypted one with nobody to send the code. Without this the board would
 * stay on it and never look for another one.
 */
static void big_timeout_handler(struct k_work *work)
{
	/* A BIG sync in progress ends in sink_stopped_cb if it fails */
	if (bc.recv_state != NULL || atomic_test_bit(bc_flags, BC_BIG_SYNCING) ||
	    atomic_test_bit(bc_flags, BC_BIG_SYNCED)) {
		return;
	}

	LOG_INF("Broadcast 0x%06x not playable, skipped for a while", bc.broadcast_id);

	skip_broadcast(bc.broadcast_id);
	pa_drop();
	scan_start();
}

static void pa_timeout_handler(struct k_work *work)
{
	LOG_INF("PA sync timeout");

	pa_drop();

	if (bc.recv_state != NULL) {
		bt_bap_scan_delegator_set_pa_state(bc.recv_state->src_id,
						   bc.recv_state->pa_sync_state ==
								   BT_BAP_PA_STATE_INFO_REQ
							   ? BT_BAP_PA_STATE_NO_PAST
							   : BT_BAP_PA_STATE_FAILED);
		bc.recv_state = NULL;
	}

	scan_start();
}

static uint16_t pa_sync_timeout(uint16_t pa_interval)
{
	uint32_t timeout;

	if (pa_interval == BT_BAP_PA_INTERVAL_UNKNOWN) {
		return BT_GAP_PER_ADV_MAX_TIMEOUT;
	}

	timeout = BT_GAP_US_TO_PER_ADV_SYNC_TIMEOUT(BT_GAP_PER_ADV_INTERVAL_TO_US(pa_interval)) *
		  PA_SYNC_TIMEOUT_FACTOR;

	return CLAMP(timeout, BT_GAP_PER_ADV_MIN_TIMEOUT, BT_GAP_PER_ADV_MAX_TIMEOUT);
}

static int pa_sync_create(const bt_addr_le_t *addr, uint8_t sid, uint16_t pa_interval)
{
	struct bt_le_per_adv_sync_param param = {
		.options = BT_LE_PER_ADV_SYNC_OPT_FILTER_DUPLICATE,
		.sid = sid,
		.skip = PA_SYNC_SKIP,
		.timeout = pa_sync_timeout(pa_interval),
	};
	int err;

	bt_addr_le_copy(&param.addr, addr);

	err = bt_le_per_adv_sync_create(&param, &bc.pa_sync);
	if (err) {
		LOG_ERR("PA sync create failed (%d)", err);
		return err;
	}

	atomic_set_bit(bc_flags, BC_PA_SYNCING);
	k_work_reschedule(&pa_timer, K_MSEC(param.timeout * 10));

	return 0;
}

static void pa_synced_cb(struct bt_le_per_adv_sync *sync,
			 struct bt_le_per_adv_sync_synced_info *info)
{
	int err;

	/* A sync handed over by PAST is not one we created ourselves */
	if (sync != bc.pa_sync &&
	    (bc.recv_state == NULL || !bt_addr_le_eq(info->addr, &bc.recv_state->addr) ||
	     info->sid != bc.recv_state->adv_sid)) {
		return;
	}

	LOG_INF("PA synced");

	bc.pa_sync = sync;
	k_work_cancel_delayable(&pa_timer);
	atomic_clear_bit(bc_flags, BC_PA_SYNCING);
	atomic_set_bit(bc_flags, BC_PA_SYNCED);
	scan_stop();

	if (bc.recv_state == NULL) {
		k_work_reschedule(&big_timer, BIG_SYNC_TIMEOUT);
	}

	if (bc.sink == NULL) {
		err = bt_bap_broadcast_sink_create(sync, bc.broadcast_id, &bc.sink);
		if (err) {
			LOG_ERR("Broadcast sink create failed (%d)", err);
			return;
		}
	}

	check_big_sync();
}

static void pa_term_cb(struct bt_le_per_adv_sync *sync,
		       const struct bt_le_per_adv_sync_term_info *info)
{
	if (sync != bc.pa_sync) {
		return;
	}

	LOG_INF("PA sync lost, reason 0x%02x", info->reason);

	pa_reset();

	if (bc.recv_state != NULL) {
		bt_bap_scan_delegator_set_pa_state(bc.recv_state->src_id,
						   BT_BAP_PA_STATE_NOT_SYNCED);
		bc.recv_state = NULL;
	}

	scan_start();
}

static bool find_broadcast_id(struct bt_data *data, void *user_data)
{
	uint32_t *broadcast_id = user_data;
	struct bt_uuid_16 uuid;

	if (data->type != BT_DATA_SVC_DATA16 ||
	    data->data_len < BT_UUID_SIZE_16 + BT_AUDIO_BROADCAST_ID_SIZE ||
	    !bt_uuid_create(&uuid.uuid, data->data, BT_UUID_SIZE_16) ||
	    bt_uuid_cmp(&uuid.uuid, BT_UUID_BROADCAST_AUDIO) != 0) {
		return true;
	}

	*broadcast_id = sys_get_le24(data->data + BT_UUID_SIZE_16);

	return false;
}

static void scan_recv_cb(const struct bt_le_scan_recv_info *info, struct net_buf_simple *ad)
{
	uint32_t broadcast_id = BT_BAP_INVALID_BROADCAST_ID;

	if (info->interval == 0 || !atomic_test_bit(bc_flags, BC_SCANNING) ||
	    atomic_test_bit(bc_flags, BC_PA_SYNCING)) {
		return;
	}

	bt_data_parse(ad, find_broadcast_id, &broadcast_id);
	if (broadcast_id == BT_BAP_INVALID_BROADCAST_ID || is_skipped(broadcast_id)) {
		return;
	}

	LOG_INF("Found broadcast 0x%06x", broadcast_id);

	if (pa_sync_create(info->addr, info->sid, info->interval) == 0) {
		bc.broadcast_id = broadcast_id;
		bc.requested_bis = BT_BAP_BIS_SYNC_NO_PREF;
		atomic_set_bit(bc_flags, BC_SYNC_REQUESTED);
	}
}

static int pa_sync_req_cb(struct bt_conn *conn,
			  const struct bt_bap_scan_delegator_recv_state *recv_state,
			  bool past_avail, uint16_t pa_interval)
{
	int err;

	LOG_INF("Assistant requests PA sync (PAST %savailable)", past_avail ? "" : "not ");

	if (bc.pa_sync != NULL || atomic_test_bit(bc_flags, BC_PA_SYNCING)) {
		return -EALREADY;
	}

	bc.recv_state = recv_state;
	bc.broadcast_id = recv_state->broadcast_id;
	scan_stop();

	if (past_avail) {
		struct bt_le_per_adv_sync_transfer_param param = {
			.skip = PA_SYNC_SKIP,
			.timeout = pa_sync_timeout(pa_interval),
		};

		err = bt_le_per_adv_sync_transfer_subscribe(conn, &param);
		if (err) {
			return err;
		}

		err = bt_bap_scan_delegator_set_pa_state(recv_state->src_id,
							 BT_BAP_PA_STATE_INFO_REQ);
		if (err) {
			return err;
		}

		atomic_set_bit(bc_flags, BC_PA_SYNCING);
		k_work_reschedule(&pa_timer, K_MSEC(param.timeout * 10));

		return 0;
	}

	return pa_sync_create(&recv_state->addr, recv_state->adv_sid, pa_interval);
}

static int pa_sync_term_req_cb(struct bt_conn *conn,
			       const struct bt_bap_scan_delegator_recv_state *recv_state)
{
	pa_drop();
	bc.recv_state = NULL;

	return 0;
}

static void broadcast_code_cb(struct bt_conn *conn,
			      const struct bt_bap_scan_delegator_recv_state *recv_state,
			      const uint8_t broadcast_code[BT_ISO_BROADCAST_CODE_SIZE])
{
	memcpy(bc.code, broadcast_code, BT_ISO_BROADCAST_CODE_SIZE);
	atomic_set_bit(bc_flags, BC_CODE_RECEIVED);
	check_big_sync();
}

static int bis_sync_req_cb(struct bt_conn *conn,
			   const struct bt_bap_scan_delegator_recv_state *recv_state,
			   const uint32_t bis_sync_req[BT_BAP_BASS_MAX_SUBGROUPS])
{
	uint32_t req = 0;

	for (int i = 0; i < BT_BAP_BASS_MAX_SUBGROUPS; i++) {
		req |= bis_sync_req[i];
	}

	LOG_INF("Assistant requests BIS 0x%08x", req);

	if (atomic_test_bit(bc_flags, BC_BIG_SYNCED) && req != bc.requested_bis) {
		int err = bt_bap_broadcast_sink_stop(bc.sink);

		if (err) {
			return err;
		}
	}

	bc.requested_bis = req;
	atomic_set_bit_to(bc_flags, BC_SYNC_REQUESTED, req != 0);
	check_big_sync();

	return 0;
}

/* Connection and advertising */

static void advertise(void)
{
	int err;

	if (adv == NULL) {
		err = bt_le_ext_adv_create(BT_BAP_ADV_PARAM_CONN_QUICK, NULL, &adv);
		if (err) {
			LOG_ERR("Advertising set create failed (%d)", err);
			return;
		}

		err = bt_le_ext_adv_set_data(adv, ad, ARRAY_SIZE(ad), NULL, 0);
		if (err) {
			LOG_ERR("Advertising data failed (%d)", err);
			return;
		}
	}

	err = bt_le_ext_adv_start(adv, BT_LE_EXT_ADV_START_DEFAULT);
	if (err && err != -EALREADY) {
		LOG_ERR("Advertising start failed (%d)", err);
	}
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		return;
	}

	LOG_INF("Connected");

	peer = bt_conn_ref(conn);
	scan_stop();
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	if (conn != peer) {
		return;
	}

	LOG_INF("Disconnected, reason 0x%02x %s", reason, bt_hci_err_to_str(reason));

	bt_conn_unref(peer);
	peer = NULL;
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
	if (err) {
		LOG_WRN("Security failed: level %u, %s", level, bt_security_err_to_str(err));
	} else {
		LOG_INF("Security level %u", level);
	}
}

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	LOG_INF("Pairing complete, %sbonded", bonded ? "" : "not ");
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	LOG_WRN("Pairing failed: %s", bt_security_err_to_str(reason));
}

static struct bt_conn_auth_info_cb auth_info_cb = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed,
};

/* The connection object is free again, so the advertising set can be reused */
static void recycled(void)
{
	advertise();
	scan_start();
}

BT_CONN_CB_DEFINE(conn_cb) = {
	.connected = connected,
	.disconnected = disconnected,
	.security_changed = security_changed,
	.recycled = recycled,
};

static void volume_state_cb(struct bt_conn *conn, int err, uint8_t vol, uint8_t mute)
{
	if (err) {
		return;
	}

	atomic_set(&volume, mute == BT_VCP_STATE_MUTED ? 0 : vol);
}

static int register_services(void)
{
	static const struct bt_audio_codec_cap codec_cap = BT_AUDIO_CODEC_CAP_LC3(
		SUPPORTED_FREQ, SUPPORTED_DURATION, BT_AUDIO_CODEC_CAP_CHAN_COUNT_SUPPORT(1, 2),
		MIN_SDU, MAX_SDU, FRAMES_PER_SDU, CONTEXTS);
	static struct bt_pacs_cap sink_cap = {.codec_cap = &codec_cap};
	static struct bt_vcp_vol_rend_cb vcs_cb = {.state = volume_state_cb};
	const struct bt_pacs_register_param pacs_param = {.snk_pac = true, .snk_loc = true};
	struct bt_vcp_vol_rend_register_param vcs_param = {
		.step = 1,
		.mute = BT_VCP_STATE_UNMUTED,
		.volume = INITIAL_VOLUME,
		.cb = &vcs_cb,
	};
	const struct bt_bap_unicast_server_register_param ascs_param = {
		.snk_cnt = CONFIG_BT_ASCS_MAX_ASE_SNK_COUNT,
		.src_cnt = 0,
	};
	int err;

	err = bt_pacs_register(&pacs_param);
	if (err) {
		return err;
	}

	err = bt_pacs_cap_register(BT_AUDIO_DIR_SINK, &sink_cap);
	if (err) {
		return err;
	}

	err = bt_pacs_set_location(BT_AUDIO_DIR_SINK, LOCATIONS);
	if (err) {
		return err;
	}

	err = bt_pacs_set_supported_contexts(BT_AUDIO_DIR_SINK, CONTEXTS);
	if (err && err != -EALREADY) {
		return err;
	}

	err = bt_pacs_set_available_contexts(BT_AUDIO_DIR_SINK, CONTEXTS);
	if (err && err != -EALREADY) {
		return err;
	}

	err = bt_bap_unicast_server_register(&ascs_param);
	if (err) {
		return err;
	}

	err = bt_bap_unicast_server_register_cb(&unicast_server_cb);
	if (err) {
		return err;
	}

	return bt_vcp_vol_rend_register(&vcs_param);
}

static int register_broadcast_sink(void)
{
	static struct bt_bap_scan_delegator_cb delegator_cb = {
		.pa_sync_req = pa_sync_req_cb,
		.pa_sync_term_req = pa_sync_term_req_cb,
		.broadcast_code = broadcast_code_cb,
		.bis_sync_req = bis_sync_req_cb,
	};
	static struct bt_bap_broadcast_sink_cb sink_cb = {
		.base_recv = base_recv_cb,
		.syncable = syncable_cb,
		.started = sink_started_cb,
		.stopped = sink_stopped_cb,
	};
	static struct bt_le_per_adv_sync_cb pa_cb = {
		.synced = pa_synced_cb,
		.term = pa_term_cb,
	};
	static struct bt_le_scan_cb scan_cb = {
		.recv = scan_recv_cb,
	};
	int err;

	err = bt_bap_scan_delegator_register(&delegator_cb);
	if (err) {
		return err;
	}

	err = bt_bap_broadcast_sink_register_cb(&sink_cb);
	if (err) {
		return err;
	}

	bt_le_per_adv_sync_cb_register(&pa_cb);
	bt_le_scan_cb_register(&scan_cb);

	return 0;
}

int le_audio_init(void)
{
	int err;

	err = lc3_rx_init();
	if (err) {
		return err;
	}

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("Bluetooth enable failed (%d)", err);
		return err;
	}

	/* Bonds have to be loaded before anyone can reconnect */
	settings_load();

	err = bt_conn_auth_info_cb_register(&auth_info_cb);
	if (err) {
		return err;
	}

	err = register_services();
	if (err) {
		LOG_ERR("Service registration failed (%d)", err);
		return err;
	}

	err = register_broadcast_sink();
	if (err) {
		LOG_ERR("Broadcast sink registration failed (%d)", err);
		return err;
	}

	ARRAY_FOR_EACH_PTR(unicast_streams, s) {
		bt_bap_stream_cb_register(&s->bap, &unicast_ops);
	}

	ARRAY_FOR_EACH_PTR(broadcast_streams, s) {
		s->rx.broadcast = true;
		bt_bap_stream_cb_register(&s->bap, &broadcast_ops);
	}

	advertise();
	scan_start();

	return 0;
}
