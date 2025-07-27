#ifndef CONN_MGR_H   /* Include guard */
#define CONN_MGR_H

#define MAX_BUFFER_NET          128
#define DEFAULT_PORT            57581

extern int open_port;
extern char message_received[MAX_BUFFER_NET];

int tcp_server_start(void);
void tcp_server_stop(void);

void set_port(int port_param);

#endif