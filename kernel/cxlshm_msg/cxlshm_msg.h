/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MSG_SENDER_INT_H
#define MSG_SENDER_INT_H

extern struct socket *client_socket;

extern int tcp_client_start(char *ip_4_addr, int port);
extern int send_message(char *message);
extern int tcp_client_stop(void);
extern int send_one_message(char *ip_4_addr, int port, char *message);

#endif