/*
 * Copyright (c) 2023, Bjarki Arge Andreasen
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/dns_resolve.h>
#include <zephyr/pm/device.h>
#include <string.h>

#define SAMPLE_TEST_ENDPOINT_HOSTNAME      "test-endpoint.com"
#define SAMPLE_TEST_ENDPOINT_UDP_ECHO_PORT 7780
#define SAMPLE_TEST_PACKET_SIZE            1024

const struct device *modem = DEVICE_DT_GET(DT_ALIAS(modem));

static uint8_t sample_test_packet[SAMPLE_TEST_PACKET_SIZE];
static uint8_t sample_recv_buffer[SAMPLE_TEST_PACKET_SIZE];
static bool sample_test_dns_in_progress;
static struct dns_addrinfo sample_test_dns_addrinfo;

K_SEM_DEFINE(dns_query_sem, 0, 1);

static uint8_t sample_prng_random(void)
{
	static uint32_t prng_state = 1234;

	prng_state = ((1103515245 * prng_state) + 12345) % (1U << 31);

	return (uint8_t)(prng_state & 0xFF);
}

static void init_sample_test_packet(void)
{
	for (size_t i = 0; i < sizeof(sample_test_packet); i++) {
		sample_test_packet[i] = sample_prng_random();
	}
}

static void sample_dns_request_result(enum dns_resolve_status status, struct dns_addrinfo *info,
				      void *user_data)
{
	if (sample_test_dns_in_progress == false) {
		return;
	}

	if (status != DNS_EAI_INPROGRESS) {
		return;
	}

	sample_test_dns_in_progress = false;

	sample_test_dns_addrinfo = *info;

	k_sem_give(&dns_query_sem);
}

static int sample_dns_request(void)
{
	static uint16_t dns_id;
	int ret;

	sample_test_dns_in_progress = true;

	ret = dns_get_addr_info(SAMPLE_TEST_ENDPOINT_HOSTNAME,
				DNS_QUERY_TYPE_A,
				&dns_id,
				sample_dns_request_result,
				NULL,
				19000);
	if (ret < 0) {
		return -EAGAIN;
	}

	if (k_sem_take(&dns_query_sem, K_SECONDS(20)) < 0) {
		return -EAGAIN;
	}

	return 0;
}

int sample_echo_packet(struct sockaddr *ai_addr, socklen_t ai_addrlen)
{
	int ret;
	int socket_fd;

	printk("Opening UDP socket\n");

	socket_fd = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

	if (socket_fd < 0) {
		printk("Failed to open socket\n");
		return -1;
	}

	printk("Socket opened\n");

	if (ai_addr->sa_family == AF_INET) {
		net_sin(ai_addr)->sin_port = htons(SAMPLE_TEST_ENDPOINT_UDP_ECHO_PORT);
	} else if (ai_addr->sa_family == AF_INET6) {
		net_sin6(ai_addr)->sin6_port = htons(SAMPLE_TEST_ENDPOINT_UDP_ECHO_PORT);
	} else {
		printk("Unsupported address family\n");
		return -1;
	}

	printk("Sending sample test packet\n");

	ret = zsock_sendto(socket_fd, sample_test_packet, sizeof(sample_test_packet), 0,
			   ai_addr, ai_addrlen);

	if (ret < sizeof(sample_test_packet)) {
		printk("Failed to send sample test packet\n");
		return -1;
	}

	printk("Await sample test packet echo\n");

	ret = zsock_recv(socket_fd, sample_recv_buffer, sizeof(sample_recv_buffer), 0);

	if (ret != sizeof(sample_test_packet)) {
		printk("Echoed sample test packet has incorrect size\n");
		return -1;
	}

	if (memcmp(sample_test_packet, sample_recv_buffer, sizeof(sample_recv_buffer)) != 0) {
		printk("Echoed sample test packet data mismatch\n");
		return -1;
	}

	printk("Echo recevied successfully!\n");

	printk("Close UDP socket\n");

	ret = zsock_close(socket_fd);

	if (ret < 0) {
		printk("Failed to close socket\n");
		return -1;
	}

	return 0;
}

int main(void)
{
	uint32_t raised_event;
	const void *info;
	size_t info_len;
	int ret;

	init_sample_test_packet();

	printk("Powering on modem\n");

	pm_device_action_run(modem, PM_DEVICE_ACTION_RESUME);

	printk("Bring up network interface\n");

	ret = net_if_up(net_if_get_default());

	if (ret < 0) {
		printk("Failed to bring up network interface\n");
		return -1;
	}

	printk("Waiting for L4 connected\n");

	ret = net_mgmt_event_wait_on_iface(net_if_get_default(),
					   NET_EVENT_L4_CONNECTED, &raised_event, &info,
					   &info_len, K_SECONDS(120));

	if (ret != 0) {
		printk("L4 was not connected in time\n");
		return -1;
	}

	printk("Performing DNS lookup of %s\n", SAMPLE_TEST_ENDPOINT_HOSTNAME);

	ret = sample_dns_request();

	if (ret < 0) {
		printk("DNS query failed\n");
		return -1;
	}

	ret = sample_echo_packet(&sample_test_dns_addrinfo.ai_addr,
				 sample_test_dns_addrinfo.ai_addrlen);

	if (ret < 0) {
		printk("Failed to send echo\n");
		return -1;
	}

	printk("Restart modem\n");

	ret = pm_device_action_run(modem, PM_DEVICE_ACTION_SUSPEND);

	if (ret != 0) {
		printk("Failed to power down modem\n");
		return -1;
	}

	pm_device_action_run(modem, PM_DEVICE_ACTION_RESUME);

	ret = net_mgmt_event_wait_on_iface(net_if_get_default(),
					   NET_EVENT_L4_CONNECTED, &raised_event, &info,
					   &info_len, K_SECONDS(60));

	if (ret != 0) {
		printk("L4 was not connected in time\n");
		return -1;
	}

	ret = sample_echo_packet(&sample_test_dns_addrinfo.ai_addr,
				 sample_test_dns_addrinfo.ai_addrlen);

	if (ret < 0) {
		printk("Failed to send echo after restart\n");
		return -1;
	}

	ret = net_if_down(net_if_get_default());

	if (ret < 0) {
		printk("Failed to bring down network interface\n");
		return -1;
	}

	printk("Powering down modem\n");

	ret = pm_device_action_run(modem, PM_DEVICE_ACTION_SUSPEND);

	if (ret != 0) {
		printk("Failed to power down modem\n");
		return -1;
	}

	printk("Sample complete\n");
	return 0;
}
