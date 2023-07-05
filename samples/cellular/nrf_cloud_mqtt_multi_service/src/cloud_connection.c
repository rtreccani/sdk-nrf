/* Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <stdio.h>
#include <zephyr/net/socket.h>
#include <net/nrf_cloud.h>
#include <date_time.h>
#include <zephyr/logging/log.h>
#include <modem/nrf_modem_lib.h>

#include "cloud_connection.h"

#include "fota_support.h"
#include "location_tracking.h"
#include "led_control.h"

LOG_MODULE_REGISTER(cloud_connection, CONFIG_MQTT_MULTI_SERVICE_LOG_LEVEL);

/* Flow control event identifiers */

/* The NETWORK_CONNECTED event is raised when network connection is established, and cleared
 * when network connection is lost.
 */
#define NETWORK_CONNECTED		(1 << 1)

/* CLOUD_CONNECTED is fired when we first connect to the nRF Cloud.
 * CLOUD_READY is fired when the connection is fully associated and ready to send device messages.
 * CLOUD_ASSOCIATION_REQUEST is a special state only used when first associating a device with
 *				an nRF Cloud user account.
 * CLOUD_DISCONNECTED is fired when disconnection is detected or requested, and will trigger
 *				a total reset of the nRF cloud connection, and the event flag state.
 */
#define CLOUD_CONNECTED			(1 << 1)
#define CLOUD_READY			(1 << 2)
#define CLOUD_ASSOCIATION_REQUEST	(1 << 3)
#define CLOUD_DISCONNECTED		(1 << 4)

/* Time either is or is not known. This is only fired once, and is never cleared. */
#define DATE_TIME_KNOWN			(1 << 1)

/* Flow control event objects for waiting for key events. */
static K_EVENT_DEFINE(network_connection_events);
static K_EVENT_DEFINE(cloud_connection_events);
static K_EVENT_DEFINE(datetime_connection_events);

static dev_msg_handler_cb_t general_dev_msg_handler;
/**
 * @brief Notify that network connection has been established.
 */
static void notify_network_connected(void)
{
	k_event_post(&network_connection_events, NETWORK_CONNECTED);
}

/**
 * @brief Reset the network connection event flag.
 */
static void clear_network_connected(void)
{
	k_event_set(&network_connection_events, 0);
}

bool await_network_connection(k_timeout_t timeout)
{
	LOG_DBG("Awaiting network connection");
	return k_event_wait_all(&network_connection_events, NETWORK_CONNECTED, false, timeout) != 0;
}

/**
 * @brief Notify that the current date and time have become known.
 */
static void notify_date_time_known(void)
{
	k_event_post(&cloud_connection_events, DATE_TIME_KNOWN);
}

bool await_date_time_known(k_timeout_t timeout)
{
	return k_event_wait(&cloud_connection_events, DATE_TIME_KNOWN, false, timeout) != 0;
}

/**
 * @brief Notify that connection to nRF Cloud has been established.
 */
static void notify_cloud_connected(void)
{
	k_event_post(&cloud_connection_events, CLOUD_CONNECTED);
}

/**
 * @brief Notify that cloud connection is ready.
 */
static void notify_cloud_ready(void)
{
	k_event_post(&cloud_connection_events, CLOUD_READY);
}

/**
 * @brief Clear nRF Cloud connection events.
 */
static void clear_cloud_connection_events(void)
{
	k_event_set(&cloud_connection_events, 0);
}

/**
 * @brief Await a connection to nRF Cloud (ignoring network connection state and cloud readiness).
 *
 * @param timeout - The time to wait before timing out.
 * @return true if occurred.
 * @return false if timed out.
 */
static bool await_cloud_connected(k_timeout_t timeout)
{
	LOG_DBG("Awaiting Cloud Connection");
	return k_event_wait(&cloud_connection_events, CLOUD_CONNECTED, false, timeout) != 0;
}

/**
 * @brief Notify that a cloud association request has been made.
 */
static void notify_cloud_requested_association(void)
{
	k_event_post(&cloud_connection_events, CLOUD_ASSOCIATION_REQUEST);
}

/**
 * @brief Check whether a user association request has been received from nRF Cloud.
 *
 * If true, we have received an association request, and we must restart the nRF Cloud connection
 * after association succeeds.
 *
 * This flag is reset by the reconnection attempt.
 *
 * @return bool - Whether we have been requested to associate with an nRF Cloud user.
 */
static bool cloud_has_requested_association(void)
{
	return k_event_wait(&cloud_connection_events, CLOUD_ASSOCIATION_REQUEST,
			    false, K_NO_WAIT) != 0;
}

/**
 * @brief Wait for nRF Cloud readiness.
 *
 * @param timeout - The time to wait before timing out.
 * @param timeout_on_disconnection - Should cloud disconnection events count as a timeout?
 * @return true if occurred.
 * @return false if timed out.
 */
static bool await_cloud_ready(k_timeout_t timeout, bool timeout_on_disconnection)
{
	LOG_DBG("Awaiting Cloud Ready");
	int await_condition = CLOUD_READY;

	if (timeout_on_disconnection) {
		await_condition |= CLOUD_DISCONNECTED;
	}

	return k_event_wait(&cloud_connection_events, await_condition,
			    false, timeout) == CLOUD_READY;
}

/*
 * This is really a convenience callback to help keep this sample clean and modular. You could
 * implement device message handling directly in the cloud_event_handler if desired.
 */
void register_general_dev_msg_handler(dev_msg_handler_cb_t handler_cb)
{
	general_dev_msg_handler = handler_cb;
}

bool await_connection(k_timeout_t timeout)
{
	return await_network_connection(timeout) && await_cloud_ready(timeout, false);
}

bool cloud_is_connected(void)
{
	return k_event_wait(&cloud_connection_events, CLOUD_CONNECTED, false, K_NO_WAIT) != 0;
}

void disconnect_cloud(void)
{
	k_event_post(&cloud_connection_events, CLOUD_DISCONNECTED);
}

bool await_cloud_disconnection(k_timeout_t timeout)
{
	return k_event_wait(&cloud_connection_events, CLOUD_DISCONNECTED, false, timeout) != 0;
}

bool cloud_is_disconnecting(void)
{
	return k_event_wait(&cloud_connection_events, CLOUD_DISCONNECTED, false, K_NO_WAIT) != 0;
}

/**
 * @brief Handler for date_time events. Used exclusively to detect when we have obtained
 * a valid modem time.
 *
 * @param date_time_evt - The date_time event. Ignored.
 */
static void date_time_evt_handler(const struct date_time_evt *date_time_evt)
{
	if (date_time_is_valid()) {
		notify_date_time_known();
	}
}

/**
 * @brief Handler for events from nRF Cloud Lib.
 *
 * @param nrf_cloud_evt Passed in event.
 */
static void cloud_event_handler(const struct nrf_cloud_evt *nrf_cloud_evt)
{
	switch (nrf_cloud_evt->type) {
	case NRF_CLOUD_EVT_TRANSPORT_CONNECTED:
		LOG_DBG("NRF_CLOUD_EVT_TRANSPORT_CONNECTED");
		/* Notify that we have connected to the nRF Cloud. */
		notify_cloud_connected();
		break;
	case NRF_CLOUD_EVT_TRANSPORT_CONNECTING:
		LOG_DBG("NRF_CLOUD_EVT_TRANSPORT_CONNECTING");
		break;
	case NRF_CLOUD_EVT_TRANSPORT_CONNECT_ERROR:
		LOG_DBG("NRF_CLOUD_EVT_TRANSPORT_CONNECT_ERROR: %d", nrf_cloud_evt->status);
		break;
	case NRF_CLOUD_EVT_USER_ASSOCIATION_REQUEST:
		LOG_DBG("NRF_CLOUD_EVT_USER_ASSOCIATION_REQUEST");
		/* This event indicates that the user must associate the device with their
		 * nRF Cloud account in the nRF Cloud portal.
		 */
		LOG_INF("Please add this device to your cloud account in the nRF Cloud portal.");

		/* Notify that we have been requested to associate with a user account.
		 * This will cause the next NRF_CLOUD_EVT_USER_ASSOCIATED event to
		 * disconnect and reconnect the device to nRF Cloud, which is required
		 * when devices are first associated with an nRF Cloud account.
		 */
		notify_cloud_requested_association();
		break;
	case NRF_CLOUD_EVT_USER_ASSOCIATED:
		LOG_DBG("NRF_CLOUD_EVT_USER_ASSOCIATED");
		/* Indicates successful association with an nRF Cloud account.
		 * Will be fired every time the device connects.
		 * If an association request has been previously received from nRF Cloud,
		 * this means this is the first association of the device, and we must disconnect
		 * and reconnect to ensure proper function of the nRF Cloud connection.
		 */

		if (cloud_has_requested_association()) {
			/* We rely on the connection loop to reconnect automatically afterwards. */
			LOG_INF("Device successfully associated with cloud!");
			disconnect_cloud();
		}
		break;
	case NRF_CLOUD_EVT_READY:
		LOG_DBG("NRF_CLOUD_EVT_READY");
		/* Notify that nRF Cloud is ready for communications from us. */
		notify_cloud_ready();
		break;
	case NRF_CLOUD_EVT_SENSOR_DATA_ACK:
		LOG_DBG("NRF_CLOUD_EVT_SENSOR_DATA_ACK");
		break;
	case NRF_CLOUD_EVT_TRANSPORT_DISCONNECTED:
		LOG_DBG("NRF_CLOUD_EVT_TRANSPORT_DISCONNECTED");
		/* Notify that we have lost contact with nRF Cloud. */
		disconnect_cloud();
		break;
	case NRF_CLOUD_EVT_ERROR:
		LOG_DBG("NRF_CLOUD_EVT_ERROR: %d", nrf_cloud_evt->status);
		break;
	case NRF_CLOUD_EVT_RX_DATA_GENERAL:
		LOG_DBG("NRF_CLOUD_EVT_RX_DATA_GENERAL");
		LOG_DBG("%d bytes received from cloud", nrf_cloud_evt->data.len);

		/* Pass the device message along to the application, if it is listening */
		if (general_dev_msg_handler) {
			/* To keep the sample simple, we invoke the callback directly.
			 * If you want to do complex operations in this callback without blocking
			 * receipt of data from nRF Cloud, you should set up a work queue and pass
			 * messages to it either here, or from inside the callback.
			 */
			general_dev_msg_handler(&nrf_cloud_evt->data);
		}

		break;
	case NRF_CLOUD_EVT_RX_DATA_SHADOW:
		LOG_DBG("NRF_CLOUD_EVT_RX_DATA_SHADOW");
		break;
	case NRF_CLOUD_EVT_FOTA_START:
		LOG_DBG("NRF_CLOUD_EVT_FOTA_START");
		break;
	case NRF_CLOUD_EVT_FOTA_DONE: {
		enum nrf_cloud_fota_type fota_type = NRF_CLOUD_FOTA_TYPE__INVALID;

		if (nrf_cloud_evt->data.ptr) {
			fota_type = *((enum nrf_cloud_fota_type *) nrf_cloud_evt->data.ptr);
		}

		LOG_DBG("NRF_CLOUD_EVT_FOTA_DONE, FOTA type: %s",
			fota_type == NRF_CLOUD_FOTA_APPLICATION	  ?		"Application"	:
			fota_type == NRF_CLOUD_FOTA_MODEM_DELTA	  ?		"Modem (delta)"	:
			fota_type == NRF_CLOUD_FOTA_MODEM_FULL	  ?		"Modem (full)"	:
			fota_type == NRF_CLOUD_FOTA_BOOTLOADER	  ?		"Bootloader"	:
										"Invalid");

		/* Notify fota_support of the completed download. */
		on_fota_downloaded();
		break;
	}
	case NRF_CLOUD_EVT_FOTA_ERROR:
		LOG_DBG("NRF_CLOUD_EVT_FOTA_ERROR");
		break;
	default:
		LOG_DBG("Unknown event type: %d", nrf_cloud_evt->type);
		break;
	}
}

/**
 * @brief Handler for LTE events coming from modem.
 *
 * @param evt Events from modem.
 */
static void lte_event_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		LOG_DBG("LTE_EVENT: Network registration status %d, %s", evt->nw_reg_status,
			evt->nw_reg_status == LTE_LC_NW_REG_NOT_REGISTERED ?	  "Not Registered" :
			evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ?	 "Registered Home" :
			evt->nw_reg_status == LTE_LC_NW_REG_SEARCHING ?		       "Searching" :
			evt->nw_reg_status == LTE_LC_NW_REG_REGISTRATION_DENIED ?
									     "Registration Denied" :
			evt->nw_reg_status == LTE_LC_NW_REG_UNKNOWN ?			 "Unknown" :
			evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING ?
									      "Registered Roaming" :
			evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_EMERGENCY ?
									    "Registered Emergency" :
			evt->nw_reg_status == LTE_LC_NW_REG_UICC_FAIL ?		       "UICC Fail" :
											 "Invalid");

		if ((evt->nw_reg_status != LTE_LC_NW_REG_REGISTERED_HOME) &&
		     (evt->nw_reg_status != LTE_LC_NW_REG_REGISTERED_ROAMING)) {
			/* Clear connected status. */
			clear_network_connected();

			/* Also reset the nRF Cloud connection if we were currently connected
			 * Failing to do this will result in nrf_cloud_send stalling upon connection
			 * re-establishment.
			 *
			 * We check cloud_is_disconnecting solely to avoid double-printing the
			 * LTE connection lost message. This check has no other effect.
			 */
			if (cloud_is_connected() && !cloud_is_disconnecting()) {
				LOG_INF("LTE connection lost. Disconnecting from nRF Cloud too...");
				disconnect_cloud();
			}
		} else {
			/* Notify we are connected to LTE. */
			notify_network_connected();
		}

		break;
	case LTE_LC_EVT_PSM_UPDATE:
		LOG_DBG("LTE_EVENT: PSM parameter update: TAU: %d, Active time: %d",
			evt->psm_cfg.tau, evt->psm_cfg.active_time);
		break;
	case LTE_LC_EVT_EDRX_UPDATE: {
		/* This check is necessary to silence compiler warnings by
		 * sprintf when debug logs are not enabled.
		 */
		if (IS_ENABLED(CONFIG_MQTT_MULTI_SERVICE_LOG_LEVEL_DBG)) {
			char log_buf[60];
			ssize_t len;

			len = snprintf(log_buf, sizeof(log_buf),
				"LTE_EVENT: eDRX parameter update: eDRX: %f, PTW: %f",
				evt->edrx_cfg.edrx, evt->edrx_cfg.ptw);
			if (len > 0) {
				LOG_DBG("%s", log_buf);
			}
		}
		break;
	}
	case LTE_LC_EVT_RRC_UPDATE:
		LOG_DBG("LTE_EVENT: RRC mode: %s",
			evt->rrc_mode == LTE_LC_RRC_MODE_CONNECTED ?
			"Connected" : "Idle");
		break;
	case LTE_LC_EVT_CELL_UPDATE:
		LOG_DBG("LTE_EVENT: LTE cell changed: Cell ID: %d, Tracking area: %d",
			evt->cell.id, evt->cell.tac);
		break;
	case LTE_LC_EVT_LTE_MODE_UPDATE:
		LOG_DBG("LTE_EVENT: Active LTE mode changed: %s",
			evt->lte_mode == LTE_LC_LTE_MODE_NONE ? "None" :
			evt->lte_mode == LTE_LC_LTE_MODE_LTEM ? "LTE-M" :
			evt->lte_mode == LTE_LC_LTE_MODE_NBIOT ? "NB-IoT" :
			"Unknown");
		break;
	case LTE_LC_EVT_MODEM_EVENT:
		LOG_DBG("LTE_EVENT: Modem domain event, type: %s",
			evt->modem_evt == LTE_LC_MODEM_EVT_LIGHT_SEARCH_DONE ?
				"Light search done" :
			evt->modem_evt == LTE_LC_MODEM_EVT_SEARCH_DONE ?
				"Search done" :
			evt->modem_evt == LTE_LC_MODEM_EVT_RESET_LOOP ?
				"Reset loop detected" :
			evt->modem_evt == LTE_LC_MODEM_EVT_BATTERY_LOW ?
				"Low battery" :
			evt->modem_evt == LTE_LC_MODEM_EVT_OVERHEATED ?
				"Modem is overheated" :
				"Unknown");
		break;
	default:
		break;
	}
}

/**
 * @brief Updates the nRF Cloud shadow with information about supported capabilities, current
 *  firmware running, FOTA support, and so on.
 */
static void update_shadow(void)
{
	int err;
	struct nrf_cloud_svc_info_fota fota_info = {
		.application = nrf_cloud_fota_is_type_enabled(NRF_CLOUD_FOTA_APPLICATION),
		.bootloader = nrf_cloud_fota_is_type_enabled(NRF_CLOUD_FOTA_BOOTLOADER),
		.modem = nrf_cloud_fota_is_type_enabled(NRF_CLOUD_FOTA_MODEM_DELTA),
		.modem_full = nrf_cloud_fota_is_type_enabled(NRF_CLOUD_FOTA_MODEM_FULL)
	};

	struct nrf_cloud_svc_info_ui ui_info = {
		.gnss = location_tracking_enabled(),
		.temperature = IS_ENABLED(CONFIG_TEMP_TRACKING),
		.log = IS_ENABLED(CONFIG_NRF_CLOUD_LOG_BACKEND) &&
		       IS_ENABLED(CONFIG_LOG_BACKEND_NRF_CLOUD_OUTPUT_TEXT),
		.dictionary_log = IS_ENABLED(CONFIG_NRF_CLOUD_LOG_BACKEND) &&
				  IS_ENABLED(CONFIG_LOG_BACKEND_NRF_CLOUD_OUTPUT_DICTIONARY)
	};
	struct nrf_cloud_svc_info service_info = {
		.fota = &fota_info,
		.ui = &ui_info
	};
	struct nrf_cloud_device_status device_status = {
		.modem = NULL,
		.svc = &service_info
	};

	err = nrf_cloud_shadow_device_status_update(&device_status);
	if (err) {
		LOG_ERR("Failed to update device shadow, error: %d", err);
	}
}
static struct nrf_cloud_obj *allocate_dev_msg_for_queue(struct nrf_cloud_obj *msg_to_copy)
{
	struct nrf_cloud_obj *new_msg = k_malloc(sizeof(struct nrf_cloud_obj));

	if (new_msg && msg_to_copy) {
		*new_msg = *msg_to_copy;
	}

	return new_msg;
}

static int enqueue_device_message(struct nrf_cloud_obj *const msg_obj, const bool create_copy)
{
	if (!msg_obj) {
		return -EINVAL;
	}

	struct nrf_cloud_obj *q_msg = msg_obj;

	if (create_copy) {
		/* Allocate a new nrf_cloud_obj structure for the message queue.
		 * Copy the contents of msg_obj, which contains a pointer to the
		 * original message data, into the new structure.
		 */
		q_msg = allocate_dev_msg_for_queue(msg_obj);
		if (!q_msg) {
			return -ENOMEM;
		}
	}

	/* Attempt to append data onto message queue. */
	LOG_DBG("Adding device message to queue");
	if (k_msgq_put(&device_message_queue, &q_msg, K_NO_WAIT)) {
		LOG_ERR("Device message rejected, outgoing message queue is full");
		if (create_copy) {
			k_free(q_msg);
		}
		return -ENOMEM;
	}

	return 0;
}

static void free_queued_dev_msg_message(struct nrf_cloud_obj *msg_obj)
{
	/* Free the memory pointed to by the msg_obj struct */
	nrf_cloud_obj_free(msg_obj);
	/* Free the msg_obj struct itself */
	k_free(msg_obj);
}

/**
 * @brief Close any connection to nRF Cloud, and reset connection status event state.
 * For internal use only. Externally, disconnect_cloud() may be used to trigger a disconnect.
 */
static void reset_cloud(void)
{
	int err;

	/* Wait for a few seconds to help residual events settle. */
	LOG_INF("Disconnecting from nRF Cloud");
	k_sleep(K_SECONDS(20));

	/* Disconnect from nRF Cloud */
	err = nrf_cloud_disconnect();

	/* nrf_cloud_uninit returns -EACCES if we are not currently in a connected state. */
	if (err == -EACCES) {
		LOG_INF("Cannot disconnect from nRF Cloud because we are not currently connected");
	} else if (err) {
		LOG_ERR("Cannot disconnect from nRF Cloud, error: %d. Continuing anyways", err);
	} else {
		LOG_INF("Successfully disconnected from nRF Cloud");
	}

	/* Clear cloud connection event state (reset to initial state). */
	clear_cloud_connection_events();
}

/**
 * @brief Establish a connection to nRF Cloud (presuming we are connected to LTE).
 *
 * @return int - 0 on success, otherwise negative error code.
 */
static int connect_cloud(void)
{
	int err;

	LOG_INF("Connecting to nRF Cloud...");

	/* Begin attempting to connect persistently. */
	while (true) {
		LOG_INF("Next connection retry in %d seconds",
			CONFIG_CLOUD_CONNECTION_RETRY_TIMEOUT_SECONDS);

		err = nrf_cloud_connect();
		if (err) {
			LOG_ERR("cloud_connect, error: %d", err);
		}

		/* Wait for cloud connection success. If succeessful, break out of the loop. */
		if (await_cloud_connected(
			K_SECONDS(CONFIG_CLOUD_CONNECTION_RETRY_TIMEOUT_SECONDS))) {
			break;
		}
	}

	/* Wait for cloud to become ready, resetting if we time out or are disconnected. */
	if (!await_cloud_ready(K_SECONDS(CONFIG_CLOUD_READY_TIMEOUT_SECONDS), true)) {
		LOG_INF("nRF Cloud failed to become ready. Resetting connection.");
		reset_cloud();
		return -ETIMEDOUT;
	}

	LOG_INF("Connected to nRF Cloud");

	return err;
}

/**
 * @brief Set up the modem library.
 *
 * @return int - 0 on success, otherwise a negative error code.
 */
static int setup_modem(void)
{
	int ret;

	/*
	 * If there is a pending modem delta firmware update stored, nrf_modem_lib_init will
	 * attempt to install it before initializing the modem library, and return a
	 * positive value to indicate that this occurred. This code can be used to
	 * determine whether the update was successful.
	 */
	ret = nrf_modem_lib_init();

	if (ret < 0) {
		LOG_ERR("Modem library initialization failed, error: %d", ret);
		return ret;
	} else if (ret == NRF_MODEM_DFU_RESULT_OK) {
		LOG_DBG("Modem library initialized after "
			"successful modem firmware update.");
	} else if (ret > 0) {
		LOG_ERR("Modem library initialized after "
			"failed modem firmware update, error: %d", ret);
	} else {
		LOG_DBG("Modem library initialized.");
	}

	/* Register to be notified when the modem has figured out the current time. */
	date_time_register_handler(date_time_evt_handler);

	return 0;
}


/**
 * @brief Set up the nRF Cloud library
 *
 * Call this before setup_network so that any pending FOTA job is handled first.
 * This avoids calling setup_network pointlessly right before a FOTA-initiated reboot.
 *
 * @return int - 0 on success, otherwise negative error code.
 */
static int setup_cloud(void)
{
	/* Initialize nrf_cloud library. */
	struct nrf_cloud_init_param params = {
		.event_handler = cloud_event_handler,
		.fmfu_dev_inf = get_full_modem_fota_fdev(),
		.application_version = CONFIG_APP_VERSION
	};

	int err = nrf_cloud_init(&params);

	if (err) {
		LOG_ERR("nRF Cloud library could not be initialized, error: %d", err);
		return err;
	}

	return 0;
}

/**
 * @brief Set up network and start trying to connect.
 *
 * @return int - 0 on success, otherwise a negative error code.
 */
static int setup_network(void)
{
	int err;

	/* Perform Configuration */
	if (IS_ENABLED(CONFIG_POWER_SAVING_MODE_ENABLE)) {
		/* Requesting PSM before connecting allows the modem to inform
		 * the network about our wish for certain PSM configuration
		 * already in the connection procedure instead of in a separate
		 * request after the connection is in place, which may be
		 * rejected in some networks.
		 */
		LOG_INF("Requesting PSM mode");

		err = lte_lc_psm_req(true);
		if (err) {
			LOG_ERR("Failed to set PSM parameters, error: %d", err);
			return err;
		} else {
			LOG_INF("PSM mode requested");
		}
	}

	/* Modem events must be enabled before we can receive them. */
	err = lte_lc_modem_events_enable();
	if (err) {
		LOG_ERR("lte_lc_modem_events_enable failed, error: %d", err);
		return err;
	}

	/* Init the modem, and start keeping an active connection.
	 * Note that if connection is lost, the modem will automatically attempt to
	 * re-establish it after this call.
	 */
	LOG_INF("Starting connection to LTE network...");
	err = lte_lc_init_and_connect_async(lte_event_handler);
	if (err) {
		LOG_ERR("Modem could not be configured, error: %d", err);
		return err;
	}

	return 0;
}

void connection_management_thread_fn(void)
{
	long_led_pattern(LED_WAITING);

	/* Enable the modem */
	LOG_INF("Setting up modem...");
	if (setup_modem()) {
		LOG_ERR("Fatal: Modem setup failed");
		long_led_pattern(LED_FAILURE);
		return;
	}

	/* The nRF Cloud library need only be initialized once, and does not need to be reset
	 * under any circumstances, even error conditions.
	 */
	LOG_INF("Setting up nRF Cloud library...");
	if (setup_cloud()) {
		LOG_ERR("Fatal: nRF Cloud library setup failed");
		long_led_pattern(LED_FAILURE);
		return;
	}

	/* Set up network and start trying to connect.
	 * This is done once only, since the network implementation should handle network
	 * persistence then after. (Once we request connection, it will automatically try to
	 * reconnect whenever connection is lost).
	 */
	LOG_INF("Setting up network...");
	if (setup_network()) {
		LOG_ERR("Fatal: Network setup failed");
		long_led_pattern(LED_FAILURE);
		return;
	}

	LOG_INF("Connecting to network. This may take several minutes...");
	while (true) {
		/* Wait for network to become connected (or re-connected if connection was lost). */
		LOG_INF("Waiting for connection to network...");

		if (IS_ENABLED(CONFIG_LED_VERBOSE_INDICATION)) {
			long_led_pattern(LED_WAITING);
		}

		(void)await_network_connection(K_FOREVER);
		LOG_INF("Connected to network");

		/* Attempt to connect to nRF Cloud. */
		if (!connect_cloud()) {
			/* If successful, update the device shadow. */
			update_shadow();

			/* and then wait patiently for a connection problem. */
			(void)await_cloud_disconnection(K_FOREVER);

			LOG_INF("Disconnected from nRF Cloud");
		} else {
			LOG_INF("Failed to connect to nRF Cloud");
		}

		/* Reset cloud connection state before trying again. */
		reset_cloud();

		/* Wait a bit before trying again. */
		k_sleep(K_SECONDS(CONFIG_CLOUD_CONNECTION_REESTABLISH_DELAY_SECONDS));
	}
}