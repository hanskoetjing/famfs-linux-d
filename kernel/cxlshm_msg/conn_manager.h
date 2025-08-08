#ifndef CONN_MGR_K_H   /* Include guard */
#define CONN_MGR_K_H

#include "../../mm/cxlshm/cxlshm_handler_private.h" 

#define MAX_BUFFER_NET          128
#define DEFAULT_PORT            57580
#define DEFAULT_HOST            "127.0.0.1"

extern int open_port;
extern char message_received[MAX_BUFFER_NET];
extern char ownership_transfer_message[MAX_BUFFER_NET];
extern char page_ownership_message[MAX_BUFFER_NET];
extern char response_received[MAX_BUFFER_NET];
extern spinlock_t ctr_lock;
extern spinlock_t client_lock;
extern struct completion is_complete;

//server side
int _tcp_server_start(void);
int _tcp_server_stop(void);
void set_port(int port_param);
void set_ownership_completion(struct completion *param);
void set_page_ownership_completion(struct completion *param) ;
int _send_response(char *response_message);

//client side
int _tcp_client_start(char *ip_4_addr, int port);
int _send_message(char *message);
int _tcp_client_stop(void);

//messaging
int send_invalidation_message(struct ownership *owner, int type);
int get_message_type(char *message_str);
#endif