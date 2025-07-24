/* SPDX-License-Identifier: GPL-2.0-only */
int tcp_client_start(char *ip_4_addr, int port);
int send_message(char *message);
int tcp_client_stop(void);
int send_one_message(char *ip_4_addr, int port, char *message);