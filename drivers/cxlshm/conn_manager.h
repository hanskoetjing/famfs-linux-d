#ifndef CONN_MGR_H   /* Include guard */
#define CONN_MGR_H

#define MAX_BUFFER_NET          128
#define DEFAULT_PORT            57581

int port = DEFAULT_PORT;
char message[MAX_BUFFER_NET] = {0};

int accept_connection(void *socket_in);
int tcp_server_start(void);
int tcp_server_stop(void);

void set_port(int port_param);

#ifndef