/*
 * Copyright (c) 2023 Bjarki Arge Andreasen
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/modem/chat.h>
#include <zephyr/modem/cmux.h>
#include <zephyr/modem/pipe.h>
#include <zephyr/modem/ppp.h>
#include <zephyr/modem/backend/uart.h>
#include <zephyr/net/ppp.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/atomic.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(modem_cellular, CONFIG_MODEM_LOG_LEVEL);

#include <string.h>
#include <stdlib.h>

#define MODEM_CELLULAR_POWER_GPIO_PULSE K_MSEC(1500)
#define MODEM_CELLULAR_RESET_GPIO_PULSE K_MSEC(100)
#define MODEM_CELLULAR_STARTUP_TIME     K_MSEC(10000)
#define MODEM_CELLULAR_SHUTDOWN_TIME    K_MSEC(10000)

enum modem_cellular_state {
	MODEM_CELLULAR_STATE_IDLE = 0,
	MODEM_CELLULAR_STATE_POWER_ON,
	MODEM_CELLULAR_STATE_RUN_INIT_SCRIPT,
	MODEM_CELLULAR_STATE_CONNECT_CMUX,
	MODEM_CELLULAR_STATE_OPEN_DLCI1,
	MODEM_CELLULAR_STATE_OPEN_DLCI2,
	MODEM_CELLULAR_STATE_RUN_DIAL_SCRIPT,
	MODEM_CELLULAR_STATE_REGISTER,
	MODEM_CELLULAR_STATE_CARRIER_ON,
	MODEM_CELLULAR_STATE_CARRIER_OFF,
	MODEM_CELLULAR_STATE_POWER_OFF,
};

enum modem_cellular_event {
	MODEM_CELLULAR_EVENT_RESUME = 0,
	MODEM_CELLULAR_EVENT_SUSPEND,
	MODEM_CELLULAR_EVENT_SCRIPT_SUCCESS,
	MODEM_CELLULAR_EVENT_SCRIPT_FAILED,
	MODEM_CELLULAR_EVENT_CMUX_CONNECTED,
	MODEM_CELLULAR_EVENT_DLCI1_OPENED,
	MODEM_CELLULAR_EVENT_DLCI2_OPENED,
	MODEM_CELLULAR_EVENT_TIMEOUT,
};

struct modem_cellular_data {
	/* UART backend */
	struct modem_pipe *uart_pipe;
	struct modem_backend_uart uart_backend;
	uint8_t uart_backend_receive_buf[512];
	uint8_t uart_backend_transmit_buf[512];

	/* CMUX */
	struct modem_cmux cmux;
	uint8_t cmux_receive_buf[128];
	uint8_t cmux_transmit_buf[256];
	struct modem_cmux_dlci dlci1;
	struct modem_cmux_dlci dlci2;
	struct modem_pipe *dlci1_pipe;
	struct modem_pipe *dlci2_pipe;
	uint8_t dlci1_receive_buf[128];
	uint8_t dlci2_receive_buf[256];

	/* Modem chat */
	struct modem_chat chat;
	uint8_t chat_receive_buf[128];
	uint8_t chat_delimiter[1];
	uint8_t chat_filter[1];
	uint8_t *chat_argv[32];

	/* Status */
	uint8_t imei[15];
	uint8_t hwinfo[64];
	uint8_t access_tech;
	uint8_t registration_status;
	uint8_t packet_service_attached;

	/* PPP */
	struct modem_ppp *ppp;

	enum modem_cellular_state state;
	const struct device *dev;
	struct k_work_delayable timeout_work;

	/* Power management */
	atomic_t suspend_atomic;
	struct k_sem suspended_sem;

	/* Event dispatcher */
	struct k_work event_dispatch_work;
	uint8_t event_buf[8];
	struct ring_buf event_rb;
	struct k_mutex event_rb_lock;
};

struct modem_cellular_config {
	const struct device *uart;
	const struct gpio_dt_spec power_gpio;
	const struct gpio_dt_spec reset_gpio;
};

static const char *modem_cellular_state_str(enum modem_cellular_state state)
{
	switch (state) {
	case MODEM_CELLULAR_STATE_IDLE:
		return "idle";
	case MODEM_CELLULAR_STATE_POWER_ON:
		return "power on";
	case MODEM_CELLULAR_STATE_RUN_INIT_SCRIPT:
		return "run init script";
	case MODEM_CELLULAR_STATE_CONNECT_CMUX:
		return "connect cmux";
	case MODEM_CELLULAR_STATE_OPEN_DLCI1:
		return "open dlci1";
	case MODEM_CELLULAR_STATE_OPEN_DLCI2:
		return "open dlci2";
	case MODEM_CELLULAR_STATE_RUN_DIAL_SCRIPT:
		return "run dial script";
	case MODEM_CELLULAR_STATE_REGISTER:
		return "register";
	case MODEM_CELLULAR_STATE_CARRIER_ON:
		return "carrier on";
	case MODEM_CELLULAR_STATE_CARRIER_OFF:
		return "carrier off";
	case MODEM_CELLULAR_STATE_POWER_OFF:
		return "power off";
	}

	return "";
}

static const char *modem_cellular_event_str(enum modem_cellular_event event)
{
	switch (event) {
	case MODEM_CELLULAR_EVENT_RESUME:
		return "resume";
	case MODEM_CELLULAR_EVENT_SUSPEND:
		return "suspend";
	case MODEM_CELLULAR_EVENT_SCRIPT_SUCCESS:
		return "script success";
	case MODEM_CELLULAR_EVENT_SCRIPT_FAILED:
		return "script failed";
	case MODEM_CELLULAR_EVENT_CMUX_CONNECTED:
		return "cmux connected";
	case MODEM_CELLULAR_EVENT_DLCI1_OPENED:
		return "dlci1 opened";
	case MODEM_CELLULAR_EVENT_DLCI2_OPENED:
		return "dlci2 opened";
	case MODEM_CELLULAR_EVENT_TIMEOUT:
		return "timeout";
	}

	return "";
}

static bool modem_cellular_gpio_is_enabled(const struct gpio_dt_spec *gpio)
{
	return gpio->port != NULL;
}

static void modem_cellular_enter_state(struct modem_cellular_data *data,
				       enum modem_cellular_state state);

static void modem_cellular_delegate_event(struct modem_cellular_data *data,
					  enum modem_cellular_event evt);

static void modem_cellular_event_handler(struct modem_cellular_data *data,
					 enum modem_cellular_event evt);

static void modem_cellular_dlci1_pipe_handler(struct modem_pipe *pipe,
					      enum modem_pipe_event event,
					      void *user_data)
{
	struct modem_cellular_data *data = (struct modem_cellular_data *)user_data;

	switch (event) {
	case MODEM_PIPE_EVENT_OPENED:
		modem_cellular_delegate_event(data, MODEM_CELLULAR_EVENT_DLCI1_OPENED);
		break;

	default:
		break;
	}
}

static void modem_cellular_dlci2_pipe_handler(struct modem_pipe *pipe,
					      enum modem_pipe_event event,
					      void *user_data)
{
	struct modem_cellular_data *data = (struct modem_cellular_data *)user_data;

	switch (event) {
	case MODEM_PIPE_EVENT_OPENED:
		modem_cellular_delegate_event(data, MODEM_CELLULAR_EVENT_DLCI2_OPENED);
		break;

	default:
		break;
	}
}

static void modem_cellular_chat_callback_handler(struct modem_chat *chat,
						 enum modem_chat_script_result result,
						 void *user_data)
{
	struct modem_cellular_data *data = (struct modem_cellular_data *)user_data;

	if (result == MODEM_CHAT_SCRIPT_RESULT_SUCCESS) {
		modem_cellular_delegate_event(data, MODEM_CELLULAR_EVENT_SCRIPT_SUCCESS);
	} else {
		modem_cellular_delegate_event(data, MODEM_CELLULAR_EVENT_SCRIPT_FAILED);
	}
}

static void modem_cellular_chat_on_imei(struct modem_chat *chat, char **argv, uint16_t argc,
					void *user_data)
{
	struct modem_cellular_data *data = (struct modem_cellular_data *)user_data;

	if (argc != 2) {
		return;
	}

	if (strlen(argv[1]) != 15) {
		return;
	}

	for (uint8_t i = 0; i < 15; i++) {
		data->imei[i] = argv[1][i] - '0';
	}
}

static void modem_cellular_chat_on_cgmm(struct modem_chat *chat, char **argv, uint16_t argc,
					void *user_data)
{
	struct modem_cellular_data *data = (struct modem_cellular_data *)user_data;

	if (argc != 2) {
		return;
	}

	strncpy(data->hwinfo, argv[1], sizeof(data->hwinfo) - 1);
}

static void modem_cellular_chat_on_creg(struct modem_chat *chat, char **argv, uint16_t argc,
					void *user_data)
{
	struct modem_cellular_data *data = (struct modem_cellular_data *)user_data;

	if (argc != 3) {
		return;
	}

	data->access_tech = atoi(argv[1]);
	data->registration_status = atoi(argv[2]);
}

static void modem_cellular_chat_on_cgatt(struct modem_chat *chat, char **argv, uint16_t argc,
					 void *user_data)
{
	struct modem_cellular_data *data = (struct modem_cellular_data *)user_data;

	if (argc != 2) {
		return;
	}

	data->packet_service_attached = atoi(argv[1]);
}

MODEM_CHAT_MATCH_DEFINE(ok_match, "OK", "", NULL);
MODEM_CHAT_MATCH_DEFINE(imei_match, "", "", modem_cellular_chat_on_imei);
MODEM_CHAT_MATCH_DEFINE(cgmm_match, "", "", modem_cellular_chat_on_cgmm);
MODEM_CHAT_MATCH_DEFINE(creg_match, "+CREG: ", ",", modem_cellular_chat_on_creg);
MODEM_CHAT_MATCH_DEFINE(cgatt_match, "+CGATT: ", ",", modem_cellular_chat_on_cgatt);

MODEM_CHAT_MATCHES_DEFINE(abort_matches, MODEM_CHAT_MATCH("ERROR", "", NULL));

MODEM_CHAT_MATCHES_DEFINE(connect_abort_matches,
			  MODEM_CHAT_MATCH("ERROR", "", NULL),
			  MODEM_CHAT_MATCH("BUSY", "", NULL),
			  MODEM_CHAT_MATCH("NO ANSWER", "", NULL),
			  MODEM_CHAT_MATCH("NO CARRIER", "", NULL),
			  MODEM_CHAT_MATCH("NO DIALTONE", "", NULL));

MODEM_CHAT_SCRIPT_CMDS_DEFINE(init_chat_script_cmds,
			      MODEM_CHAT_SCRIPT_CMD_RESP_NONE("AT", 100),
			      MODEM_CHAT_SCRIPT_CMD_RESP_NONE("AT", 100),
			      MODEM_CHAT_SCRIPT_CMD_RESP_NONE("AT", 100),
			      MODEM_CHAT_SCRIPT_CMD_RESP_NONE("AT", 100),
			      MODEM_CHAT_SCRIPT_CMD_RESP("ATE0", ok_match),
			      MODEM_CHAT_SCRIPT_CMD_RESP("AT+CMEE=1", ok_match),
			      MODEM_CHAT_SCRIPT_CMD_RESP("AT+CREG=0", ok_match),
			      MODEM_CHAT_SCRIPT_CMD_RESP("AT+CGSN", imei_match),
			      MODEM_CHAT_SCRIPT_CMD_RESP("", ok_match),
			      MODEM_CHAT_SCRIPT_CMD_RESP("AT+CGMM", cgmm_match),
			      MODEM_CHAT_SCRIPT_CMD_RESP("", ok_match),
			      MODEM_CHAT_SCRIPT_CMD_RESP_NONE("AT+CMUX=0,0,5,127,10,3,30,10,2",
							      100));

MODEM_CHAT_SCRIPT_DEFINE(init_chat_script, init_chat_script_cmds, abort_matches,
			 modem_cellular_chat_callback_handler, 10);

MODEM_CHAT_SCRIPT_CMDS_DEFINE(net_stat_chat_script_cmds,
			      MODEM_CHAT_SCRIPT_CMD_RESP("AT+CREG?", creg_match),
			      MODEM_CHAT_SCRIPT_CMD_RESP("", ok_match),
			      MODEM_CHAT_SCRIPT_CMD_RESP("AT+CGATT?", cgatt_match),
			      MODEM_CHAT_SCRIPT_CMD_RESP("", ok_match));

MODEM_CHAT_SCRIPT_DEFINE(net_stat_chat_script, net_stat_chat_script_cmds, abort_matches,
			 modem_cellular_chat_callback_handler, 10);

MODEM_CHAT_SCRIPT_CMDS_DEFINE(connect_chat_script_cmds,
			      MODEM_CHAT_SCRIPT_CMD_RESP("AT+CGDCONT=1,\"IP\","
							 "\""CONFIG_MODEM_CELLULAR_APN"\","
							 "\""CONFIG_MODEM_CELLULAR_USERNAME"\","
							 "\""CONFIG_MODEM_CELLULAR_PASSWORD"\"",
							 ok_match),
			      MODEM_CHAT_SCRIPT_CMD_RESP_NONE("ATD*99#", 0));

MODEM_CHAT_SCRIPT_DEFINE(connect_chat_script, connect_chat_script_cmds, connect_abort_matches,
			 modem_cellular_chat_callback_handler, 10);

static void modem_cellular_log_state_changed(enum modem_cellular_state last_state,
					     enum modem_cellular_state new_state)
{
	LOG_INF("switch from %s to %s", modem_cellular_state_str(last_state),
		modem_cellular_state_str(new_state));
}

static void modem_cellular_log_event(enum modem_cellular_event evt)
{
	LOG_INF("event %s", modem_cellular_event_str(evt));
}

static void modem_cellular_start_timer(struct modem_cellular_data *data, k_timeout_t timeout)
{
	k_work_schedule(&data->timeout_work, timeout);
}

static void modem_cellular_stop_timer(struct modem_cellular_data *data)
{
	k_work_cancel_delayable(&data->timeout_work);
}

static void modem_cellular_timeout_handler(struct k_work *item)
{
	struct modem_cellular_data *data =
		CONTAINER_OF(item, struct modem_cellular_data, timeout_work);

	modem_cellular_delegate_event(data, MODEM_CELLULAR_EVENT_TIMEOUT);
}

static void modem_cellular_event_dispatch_handler(struct k_work *item)
{
	struct modem_cellular_data *data =
		CONTAINER_OF(item, struct modem_cellular_data, event_dispatch_work);

	uint8_t events[sizeof(data->event_buf)];
	uint8_t events_cnt;

	k_mutex_lock(&data->event_rb_lock, K_FOREVER);

	events_cnt = (uint8_t)ring_buf_get(&data->event_rb, events, sizeof(data->event_buf));

	k_mutex_unlock(&data->event_rb_lock);

	for (uint8_t i = 0; i < events_cnt; i++) {
		modem_cellular_event_handler(data, (enum modem_cellular_event)events[i]);
	}

	if (atomic_get(&data->suspend_atomic)) {
		modem_cellular_event_handler(data, MODEM_CELLULAR_EVENT_SUSPEND);
	}
}

static void modem_cellular_delegate_event(struct modem_cellular_data *data,
					  enum modem_cellular_event evt)
{
	k_mutex_lock(&data->event_rb_lock, K_FOREVER);
	ring_buf_put(&data->event_rb, (uint8_t *)&evt, 1);
	k_mutex_unlock(&data->event_rb_lock);
	k_work_submit(&data->event_dispatch_work);
}

static void modem_cellular_idle_event_handler(struct modem_cellular_data *data,
					      enum modem_cellular_event evt)
{
	const struct modem_cellular_config *config =
		(const struct modem_cellular_config *)data->dev->config;

	switch (evt) {
	case MODEM_CELLULAR_EVENT_RESUME:
		if (modem_cellular_gpio_is_enabled(&config->power_gpio) ||
		    modem_cellular_gpio_is_enabled(&config->reset_gpio)) {
			modem_cellular_enter_state(data, MODEM_CELLULAR_STATE_POWER_ON);
			break;
		}

		modem_cellular_enter_state(data, MODEM_CELLULAR_STATE_RUN_INIT_SCRIPT);
		break;

	default:
		break;
	}
}

static int modem_cellular_on_idle_state_leave(struct modem_cellular_data *data)
{
	return modem_pipe_open(data->uart_pipe);
}

static int modem_cellular_on_power_on_state_enter(struct modem_cellular_data *data)
{
	const struct modem_cellular_config *config =
		(const struct modem_cellular_config *)data->dev->config;

	if (modem_cellular_gpio_is_enabled(&config->power_gpio)) {
		gpio_pin_set_dt(&config->power_gpio, 1);
		modem_cellular_start_timer(data, MODEM_CELLULAR_POWER_GPIO_PULSE);
	} else {
		gpio_pin_set_dt(&config->reset_gpio, 1);
		modem_cellular_start_timer(data, MODEM_CELLULAR_RESET_GPIO_PULSE);
	}

	return 0;
}

static void modem_cellular_power_on_event_handler(struct modem_cellular_data *data,
						  enum modem_cellular_event evt)
{
	const struct modem_cellular_config *config =
		(const struct modem_cellular_config *)data->dev->config;

	switch (evt) {
	case MODEM_CELLULAR_EVENT_TIMEOUT:
		if ((modem_cellular_gpio_is_enabled(&config->power_gpio) &&
		    gpio_pin_get_dt(&config->power_gpio))) {
			gpio_pin_set_dt(&config->power_gpio, 0);
			modem_cellular_start_timer(data, MODEM_CELLULAR_STARTUP_TIME);
			break;
		}

		if ((modem_cellular_gpio_is_enabled(&config->reset_gpio) &&
		    gpio_pin_get_dt(&config->reset_gpio))) {
			gpio_pin_set_dt(&config->reset_gpio, 0);
			modem_cellular_start_timer(data, MODEM_CELLULAR_STARTUP_TIME);
			break;
		}

		modem_cellular_enter_state(data, MODEM_CELLULAR_STATE_RUN_INIT_SCRIPT);
		break;

	default:
		break;
	}
}

static int modem_cellular_on_run_init_script_state_enter(struct modem_cellular_data *data)
{
	if (modem_chat_attach(&data->chat, data->uart_pipe) < 0) {
		return -EAGAIN;
	}

	return modem_chat_script_run(&data->chat, &init_chat_script);
}

static void modem_cellular_run_init_script_event_handler(struct modem_cellular_data *data,
							 enum modem_cellular_event evt)
{
	const struct modem_cellular_config *config =
		(const struct modem_cellular_config *)data->dev->config;

	switch (evt) {
	case MODEM_CELLULAR_EVENT_SCRIPT_SUCCESS:
		net_if_set_link_addr(modem_ppp_get_iface(data->ppp), data->imei,
				     ARRAY_SIZE(data->imei), NET_LINK_UNKNOWN);

		modem_cellular_enter_state(data, MODEM_CELLULAR_STATE_CONNECT_CMUX);

		break;

	case MODEM_CELLULAR_EVENT_SCRIPT_FAILED:
		if (modem_cellular_gpio_is_enabled(&config->power_gpio)) {
			modem_cellular_enter_state(data, MODEM_CELLULAR_STATE_POWER_ON);
			break;
		}

		modem_cellular_enter_state(data, MODEM_CELLULAR_STATE_RUN_INIT_SCRIPT);

		break;

	default:
		break;
	}
}

static int modem_cellular_on_run_init_script_state_leave(struct modem_cellular_data *data)
{
	modem_chat_release(&data->chat);

	return 0;
}

static int modem_cellular_on_connect_cmux_state_enter(struct modem_cellular_data *data)
{
	if (modem_cmux_attach(&data->cmux, data->uart_pipe) < 0) {
		return -EAGAIN;
	}

	modem_cellular_start_timer(data, K_MSEC(500));

	return 0;
}

static void modem_cellular_connect_cmux_event_handler(struct modem_cellular_data *data,
						      enum modem_cellular_event evt)
{
	switch (evt) {
	case MODEM_CELLULAR_EVENT_TIMEOUT:
		modem_cmux_connect_async(&data->cmux);
		break;

	case MODEM_CELLULAR_EVENT_CMUX_CONNECTED:
		modem_cellular_enter_state(data, MODEM_CELLULAR_STATE_OPEN_DLCI1);
		break;

	default:
		break;
	}
}

static int modem_cellular_on_open_dlci1_state_enter(struct modem_cellular_data *data)
{
	modem_pipe_attach(data->dlci1_pipe, modem_cellular_dlci1_pipe_handler, data);

	return modem_pipe_open_async(data->dlci1_pipe);
}

static void modem_cellular_open_dlci1_event_handler(struct modem_cellular_data *data,
						    enum modem_cellular_event evt)
{
	switch (evt) {
	case MODEM_CELLULAR_EVENT_DLCI1_OPENED:
		modem_cellular_enter_state(data, MODEM_CELLULAR_STATE_OPEN_DLCI2);
		break;

	default:
		break;
	}
}

static int modem_cellular_on_open_dlci1_state_leave(struct modem_cellular_data *data)
{
	modem_pipe_release(data->dlci1_pipe);

	return 0;
}

static int modem_cellular_on_open_dlci2_state_enter(struct modem_cellular_data *data)
{
	modem_pipe_attach(data->dlci2_pipe, modem_cellular_dlci2_pipe_handler, data);

	return modem_pipe_open_async(data->dlci2_pipe);
}

static void modem_cellular_open_dlci2_event_handler(struct modem_cellular_data *data,
						    enum modem_cellular_event evt)
{
	switch (evt) {
	case MODEM_CELLULAR_EVENT_DLCI2_OPENED:
		modem_cellular_enter_state(data, MODEM_CELLULAR_STATE_RUN_DIAL_SCRIPT);
		break;

	default:
		break;
	}
}

static int modem_cellular_on_open_dlci2_state_leave(struct modem_cellular_data *data)
{
	modem_pipe_release(data->dlci2_pipe);

	return 0;
}

static int modem_cellular_on_run_dial_script_state_enter(struct modem_cellular_data *data)
{
	if (modem_chat_attach(&data->chat, data->dlci2_pipe) < 0) {
		return -EAGAIN;
	}

	modem_cellular_start_timer(data, K_MSEC(500));

	return 0;
}

static void modem_cellular_run_dial_script_event_handler(struct modem_cellular_data *data,
							 enum modem_cellular_event evt)
{
	switch (evt) {
	case MODEM_CELLULAR_EVENT_TIMEOUT:
		modem_chat_script_run(&data->chat, &connect_chat_script);
		break;

	case MODEM_CELLULAR_EVENT_SCRIPT_FAILED:
		modem_cellular_start_timer(data, K_MSEC(500));
		break;

	case MODEM_CELLULAR_EVENT_SCRIPT_SUCCESS:
		modem_cellular_enter_state(data, MODEM_CELLULAR_STATE_REGISTER);
		break;

	default:
		break;
	}
}

static int modem_cellular_on_run_dial_script_state_leave(struct modem_cellular_data *data)
{
	modem_chat_release(&data->chat);

	return modem_ppp_attach(data->ppp, data->dlci2_pipe);
}

static int modem_cellular_on_register_state_enter(struct modem_cellular_data *data)
{
	if (modem_chat_attach(&data->chat, data->dlci1_pipe) < 0) {
		return -EAGAIN;
	}

	modem_cellular_start_timer(data, K_SECONDS(2));

	return modem_chat_script_run(&data->chat, &net_stat_chat_script);
}

static bool modem_cellular_is_registered(struct modem_cellular_data *data)
{
	return (data->registration_status == 5) && (data->packet_service_attached == 1);
}

static void modem_cellular_register_event_handler(struct modem_cellular_data *data,
						  enum modem_cellular_event evt)
{
	const struct modem_cellular_config *config =
		(const struct modem_cellular_config *)data->dev->config;

	switch (evt) {
	case MODEM_CELLULAR_EVENT_SUSPEND:
		if (modem_cellular_gpio_is_enabled(&config->power_gpio)) {
			modem_cellular_enter_state(data, MODEM_CELLULAR_STATE_POWER_OFF);
		} else {
			modem_cellular_enter_state(data, MODEM_CELLULAR_STATE_IDLE);
		}

		break;

	case MODEM_CELLULAR_EVENT_SCRIPT_SUCCESS:
		if (modem_cellular_is_registered(data)) {
			modem_cellular_enter_state(data, MODEM_CELLULAR_STATE_CARRIER_ON);
		}

		break;

	case MODEM_CELLULAR_EVENT_TIMEOUT:
		modem_cellular_start_timer(data, K_SECONDS(2));
		modem_chat_script_run(&data->chat, &net_stat_chat_script);
		break;

	default:
		break;
	}
}

static int modem_cellular_on_register_state_leave(struct modem_cellular_data *data)
{
	modem_cellular_stop_timer(data);
	modem_chat_release(&data->chat);
	return 0;
}

static int modem_cellular_on_carrier_on_state_enter(struct modem_cellular_data *data)
{
	net_if_carrier_on(modem_ppp_get_iface(data->ppp));
	modem_chat_attach(&data->chat, data->dlci1_pipe);
	modem_chat_script_run(&data->chat, &net_stat_chat_script);
	modem_cellular_start_timer(data, K_SECONDS(4));
	return 0;
}

static void modem_cellular_carrier_on_event_handler(struct modem_cellular_data *data,
						    enum modem_cellular_event evt)
{
	switch (evt) {
	case MODEM_CELLULAR_EVENT_SUSPEND:
		modem_cellular_enter_state(data, MODEM_CELLULAR_STATE_CARRIER_OFF);
		break;

	case MODEM_CELLULAR_EVENT_SCRIPT_SUCCESS:
		if (modem_cellular_is_registered(data) == false) {
			modem_cellular_enter_state(data, MODEM_CELLULAR_STATE_RUN_DIAL_SCRIPT);
		}

		break;

	case MODEM_CELLULAR_EVENT_TIMEOUT:
		modem_chat_script_run(&data->chat, &net_stat_chat_script);
		modem_cellular_start_timer(data, K_SECONDS(4));
		break;

	default:
		break;
	}
}

static int modem_cellular_on_carrier_on_state_leave(struct modem_cellular_data *data)
{
	modem_cellular_stop_timer(data);
	modem_chat_script_abort(&data->chat);
	modem_chat_release(&data->chat);
	modem_ppp_release(data->ppp);
	return 0;
}

static int modem_cellular_on_carrier_off_state_enter(struct modem_cellular_data *data)
{
	net_if_carrier_off(modem_ppp_get_iface(data->ppp));
	modem_cellular_start_timer(data, K_SECONDS(1));
	return 0;
}

static void modem_cellular_carrier_off_event_handler(struct modem_cellular_data *data,
						 enum modem_cellular_event evt)
{
	const struct modem_cellular_config *config =
		(const struct modem_cellular_config *)data->dev->config;

	switch (evt) {
	case MODEM_CELLULAR_EVENT_TIMEOUT:
		if (modem_cellular_gpio_is_enabled(&config->power_gpio)) {
			modem_cellular_enter_state(data, MODEM_CELLULAR_STATE_POWER_OFF);
			break;
		}

		if (modem_cellular_gpio_is_enabled(&config->reset_gpio)) {
			gpio_pin_set_dt(&config->reset_gpio, 1);
		}

		modem_cellular_enter_state(data, MODEM_CELLULAR_STATE_IDLE);
		break;

	default:
		break;
	}
}

static int modem_cellular_on_power_off_state_enter(struct modem_cellular_data *data)
{
	const struct modem_cellular_config *config =
		(const struct modem_cellular_config *)data->dev->config;

	modem_cmux_release(&data->cmux);
	modem_pipe_close(data->uart_pipe);
	gpio_pin_set_dt(&config->power_gpio, 1);
	modem_cellular_start_timer(data, MODEM_CELLULAR_POWER_GPIO_PULSE);
	return 0;
}

static void modem_cellular_power_off_event_handler(struct modem_cellular_data *data,
						   enum modem_cellular_event evt)
{
	const struct modem_cellular_config *config =
		(const struct modem_cellular_config *)data->dev->config;

	switch (evt) {
	case MODEM_CELLULAR_EVENT_TIMEOUT:
		if (gpio_pin_get_dt(&config->power_gpio)) {
			gpio_pin_set_dt(&config->power_gpio, 0);
			modem_cellular_start_timer(data, MODEM_CELLULAR_SHUTDOWN_TIME);
			break;
		}

		modem_cellular_enter_state(data, MODEM_CELLULAR_STATE_IDLE);
		break;

	default:
		break;
	}
}

static int modem_cellular_on_power_off_state_leave(struct modem_cellular_data *data)
{
	k_sem_give(&data->suspended_sem);
	return 0;
}

static int modem_cellular_on_state_enter(struct modem_cellular_data *data)
{
	int ret;

	switch (data->state) {
	case MODEM_CELLULAR_STATE_POWER_ON:
		ret = modem_cellular_on_power_on_state_enter(data);
		break;

	case MODEM_CELLULAR_STATE_RUN_INIT_SCRIPT:
		ret = modem_cellular_on_run_init_script_state_enter(data);
		break;

	case MODEM_CELLULAR_STATE_CONNECT_CMUX:
		ret = modem_cellular_on_connect_cmux_state_enter(data);
		break;

	case MODEM_CELLULAR_STATE_OPEN_DLCI1:
		ret = modem_cellular_on_open_dlci1_state_enter(data);
		break;

	case MODEM_CELLULAR_STATE_OPEN_DLCI2:
		ret = modem_cellular_on_open_dlci2_state_enter(data);
		break;

	case MODEM_CELLULAR_STATE_RUN_DIAL_SCRIPT:
		ret = modem_cellular_on_run_dial_script_state_enter(data);
		break;

	case MODEM_CELLULAR_STATE_REGISTER:
		ret = modem_cellular_on_register_state_enter(data);
		break;

	case MODEM_CELLULAR_STATE_CARRIER_ON:
		ret = modem_cellular_on_carrier_on_state_enter(data);
		break;

	case MODEM_CELLULAR_STATE_CARRIER_OFF:
		ret = modem_cellular_on_carrier_off_state_enter(data);
		break;

	case MODEM_CELLULAR_STATE_POWER_OFF:
		ret = modem_cellular_on_power_off_state_enter(data);
		break;

	default:
		ret = 0;
		break;
	}

	return ret;
}

static int modem_cellular_on_state_leave(struct modem_cellular_data *data)
{
	int ret;

	switch (data->state) {
	case MODEM_CELLULAR_STATE_IDLE:
		ret = modem_cellular_on_idle_state_leave(data);
		break;

	case MODEM_CELLULAR_STATE_RUN_INIT_SCRIPT:
		ret = modem_cellular_on_run_init_script_state_leave(data);
		break;

	case MODEM_CELLULAR_STATE_OPEN_DLCI1:
		ret = modem_cellular_on_open_dlci1_state_leave(data);
		break;

	case MODEM_CELLULAR_STATE_OPEN_DLCI2:
		ret = modem_cellular_on_open_dlci2_state_leave(data);
		break;

	case MODEM_CELLULAR_STATE_RUN_DIAL_SCRIPT:
		ret = modem_cellular_on_run_dial_script_state_leave(data);
		break;

	case MODEM_CELLULAR_STATE_REGISTER:
		ret = modem_cellular_on_register_state_leave(data);
		break;

	case MODEM_CELLULAR_STATE_CARRIER_ON:
		ret = modem_cellular_on_carrier_on_state_leave(data);
		break;

	case MODEM_CELLULAR_STATE_POWER_OFF:
		ret = modem_cellular_on_power_off_state_leave(data);
		break;

	default:
		ret = 0;
		break;
	}

	return ret;
}

static void modem_cellular_enter_state(struct modem_cellular_data *data,
				       enum modem_cellular_state state)
{
	int ret;

	ret = modem_cellular_on_state_leave(data);

	if (ret < 0) {
		LOG_WRN("failed to leave state, error: %i", ret);

		return;
	}

	data->state = state;
	ret = modem_cellular_on_state_enter(data);

	if (ret < 0) {
		LOG_WRN("failed to enter state error: %i", ret);
	}
}

static void modem_cellular_event_handler(struct modem_cellular_data *data,
					 enum modem_cellular_event evt)
{
	enum modem_cellular_state state;

	state = data->state;

	modem_cellular_log_event(evt);

	switch (data->state) {
	case MODEM_CELLULAR_STATE_IDLE:
		modem_cellular_idle_event_handler(data, evt);
		break;

	case MODEM_CELLULAR_STATE_POWER_ON:
		modem_cellular_power_on_event_handler(data, evt);
		break;

	case MODEM_CELLULAR_STATE_RUN_INIT_SCRIPT:
		modem_cellular_run_init_script_event_handler(data, evt);
		break;

	case MODEM_CELLULAR_STATE_CONNECT_CMUX:
		modem_cellular_connect_cmux_event_handler(data, evt);
		break;

	case MODEM_CELLULAR_STATE_OPEN_DLCI1:
		modem_cellular_open_dlci1_event_handler(data, evt);
		break;

	case MODEM_CELLULAR_STATE_OPEN_DLCI2:
		modem_cellular_open_dlci2_event_handler(data, evt);
		break;

	case MODEM_CELLULAR_STATE_RUN_DIAL_SCRIPT:
		modem_cellular_run_dial_script_event_handler(data, evt);
		break;

	case MODEM_CELLULAR_STATE_REGISTER:
		modem_cellular_register_event_handler(data, evt);
		break;

	case MODEM_CELLULAR_STATE_CARRIER_ON:
		modem_cellular_carrier_on_event_handler(data, evt);
		break;

	case MODEM_CELLULAR_STATE_CARRIER_OFF:
		modem_cellular_carrier_off_event_handler(data, evt);
		break;

	case MODEM_CELLULAR_STATE_POWER_OFF:
		modem_cellular_power_off_event_handler(data, evt);
		break;
	}

	if (state != data->state) {
		modem_cellular_log_state_changed(state, data->state);
	}
}

static void modem_cellular_cmux_handler(struct modem_cmux *cmux, enum modem_cmux_event event,
					void *user_data)
{
	struct modem_cellular_data *data = (struct modem_cellular_data *)user_data;

	switch (event) {
	case MODEM_CMUX_EVENT_CONNECTED:
		modem_cellular_delegate_event(data, MODEM_CELLULAR_EVENT_CMUX_CONNECTED);
		break;

	default:
		break;
	}
}

#ifdef CONFIG_PM_DEVICE
static int modem_cellular_pm_action(const struct device *dev, enum pm_device_action action)
{
	struct modem_cellular_data *data = (struct modem_cellular_data *)dev->data;
	int ret;

	switch (action) {
	case PM_DEVICE_ACTION_RESUME:
		atomic_set(&data->suspend_atomic, 0);
		modem_cellular_delegate_event(data, MODEM_CELLULAR_EVENT_RESUME);
		ret = 0;
		break;

	case PM_DEVICE_ACTION_SUSPEND:
		atomic_set(&data->suspend_atomic, 1);
		ret = k_sem_take(&data->suspended_sem, K_SECONDS(30));
		break;

	default:
		ret = -ENOTSUP;
		break;
	}

	return ret;
}
#endif /* CONFIG_PM_DEVICE */

static int modem_cellular_init(const struct device *dev)
{
	struct modem_cellular_data *data = (struct modem_cellular_data *)dev->data;
	struct modem_cellular_config *config = (struct modem_cellular_config *)dev->config;

	data->dev = dev;

	k_work_init_delayable(&data->timeout_work, modem_cellular_timeout_handler);

	k_work_init(&data->event_dispatch_work, modem_cellular_event_dispatch_handler);
	ring_buf_init(&data->event_rb, sizeof(data->event_buf), data->event_buf);

	atomic_set(&data->suspend_atomic, 0);
	k_sem_init(&data->suspended_sem, 0, 1);

	if (modem_cellular_gpio_is_enabled(&config->power_gpio)) {
		gpio_pin_configure_dt(&config->power_gpio, GPIO_OUTPUT_INACTIVE);
	}

	if (modem_cellular_gpio_is_enabled(&config->reset_gpio)) {
		gpio_pin_configure_dt(&config->reset_gpio, GPIO_OUTPUT_ACTIVE);
	}

	{
		const struct modem_backend_uart_config uart_backend_config = {
			.uart = config->uart,
			.receive_buf = data->uart_backend_receive_buf,
			.receive_buf_size = ARRAY_SIZE(data->uart_backend_receive_buf),
			.transmit_buf = data->uart_backend_transmit_buf,
			.transmit_buf_size = ARRAY_SIZE(data->uart_backend_transmit_buf),
		};

		data->uart_pipe = modem_backend_uart_init(&data->uart_backend,
							  &uart_backend_config);
	}

	{
		const struct modem_cmux_config cmux_config = {
			.callback = modem_cellular_cmux_handler,
			.user_data = data,
			.receive_buf = data->cmux_receive_buf,
			.receive_buf_size = ARRAY_SIZE(data->cmux_receive_buf),
			.transmit_buf = data->cmux_transmit_buf,
			.transmit_buf_size = ARRAY_SIZE(data->cmux_transmit_buf),
		};

		modem_cmux_init(&data->cmux, &cmux_config);
	}

	{
		const struct modem_cmux_dlci_config dlci1_config = {
			.dlci_address = 1,
			.receive_buf = data->dlci1_receive_buf,
			.receive_buf_size = ARRAY_SIZE(data->dlci1_receive_buf),
		};

		data->dlci1_pipe = modem_cmux_dlci_init(&data->cmux, &data->dlci1,
							&dlci1_config);
	}

	{
		const struct modem_cmux_dlci_config dlci2_config = {
			.dlci_address = 2,
			.receive_buf = data->dlci2_receive_buf,
			.receive_buf_size = ARRAY_SIZE(data->dlci2_receive_buf),
		};

		data->dlci2_pipe = modem_cmux_dlci_init(&data->cmux, &data->dlci2,
							&dlci2_config);
	}

	{
		const struct modem_chat_config chat_config = {
			.user_data = data,
			.receive_buf = data->chat_receive_buf,
			.receive_buf_size = ARRAY_SIZE(data->chat_receive_buf),
			.delimiter = data->chat_delimiter,
			.delimiter_size = ARRAY_SIZE(data->chat_delimiter),
			.filter = data->chat_filter,
			.filter_size = ARRAY_SIZE(data->chat_filter),
			.argv = data->chat_argv,
			.argv_size = ARRAY_SIZE(data->chat_argv),
			.unsol_matches = NULL,
			.unsol_matches_size = 0,
			.process_timeout = K_MSEC(2),
		};

		modem_chat_init(&data->chat, &chat_config);
	}

#ifndef CONFIG_PM_DEVICE
	modem_cellular_delegate_event(data, MODEM_CELLULAR_EVENT_RESUME);
#else
	pm_device_init_suspended(dev);
#endif /* CONFIG_PM_DEVICE */

	return 0;
}

#define MODEM_CELLULAR_INST_NAME(name, inst) \
	_CONCAT(_CONCAT(_CONCAT(name, _), DT_DRV_COMPAT), inst)

#define MODEM_CELLULAR_DEVICE(inst)								\
	MODEM_PPP_DEFINE(MODEM_CELLULAR_INST_NAME(ppp, inst), NULL, 98, 1500, 64);		\
												\
	static struct modem_cellular_data MODEM_CELLULAR_INST_NAME(data, inst) = {		\
		.chat_delimiter = {'\r'},							\
		.chat_filter = {'\n'},								\
		.ppp = &MODEM_CELLULAR_INST_NAME(ppp, inst),					\
	};											\
												\
	static struct modem_cellular_config MODEM_CELLULAR_INST_NAME(config, inst) = {		\
		.uart = DEVICE_DT_GET(DT_INST_BUS(inst)),					\
		.power_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, mdm_power_gpios, {}),		\
		.reset_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, mdm_reset_gpios, {}),		\
	};											\
												\
	PM_DEVICE_DT_INST_DEFINE(inst, modem_cellular_pm_action);				\
												\
	DEVICE_DT_INST_DEFINE(inst, modem_cellular_init, PM_DEVICE_DT_INST_GET(inst),		\
			      &MODEM_CELLULAR_INST_NAME(data, inst),				\
			      &MODEM_CELLULAR_INST_NAME(config, inst), POST_KERNEL, 99, NULL);

#define DT_DRV_COMPAT quectel_bg95
DT_INST_FOREACH_STATUS_OKAY(MODEM_CELLULAR_DEVICE)
#undef DT_DRV_COMPAT

#define DT_DRV_COMPAT zephyr_gsm_ppp
DT_INST_FOREACH_STATUS_OKAY(MODEM_CELLULAR_DEVICE)
#undef DT_DRV_COMPAT

#define DT_DRV_COMPAT simcom_sim7080
DT_INST_FOREACH_STATUS_OKAY(MODEM_CELLULAR_DEVICE)
#undef DT_DRV_COMPAT

#define DT_DRV_COMPAT u_blox_sara_r4
DT_INST_FOREACH_STATUS_OKAY(MODEM_CELLULAR_DEVICE)
#undef DT_DRV_COMPAT

#define DT_DRV_COMPAT swir_hl7800
DT_INST_FOREACH_STATUS_OKAY(MODEM_CELLULAR_DEVICE)
#undef DT_DRV_COMPAT
