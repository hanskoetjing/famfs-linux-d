#ifndef CONN_MGR_K_H   /* Include guard */
#define CONN_MGR_K_H

#define MAX_BUFFER_NET          128
#define DEFAULT_PORT            57580

extern int open_port;
extern char message_received[MAX_BUFFER_NET];
extern char ownership_transfer_message[MAX_BUFFER_NET];
extern char response_received[MAX_BUFFER_NET];
extern spinlock_t ctr_lock;
extern spinlock_t client_lock;
extern struct completion is_complete;

//server side
int tcp_server_start(void);
void tcp_server_stop(void);
void set_port(int port_param);
void set_ownership_completion(struct completion *param);
int send_response(char *response_message);

//client side
int tcp_client_start(char *ip_4_addr, int port);
int send_message(char *message);
int tcp_client_stop(void);

#endif