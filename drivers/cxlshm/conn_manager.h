#ifndef CONN_MGR_H   /* Include guard */
#define CONN_MGR_H

#define MAX_BUFFER_NET          128
#define DEFAULT_PORT            57581

int open_port = DEFAULT_PORT;
char message_received[MAX_BUFFER_NET] = {0};

int tcp_server_start(void);
void tcp_server_stop(void);

void set_port(int port_param);

#endif