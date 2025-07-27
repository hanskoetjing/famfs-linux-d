#include <linux/socket.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/in.h>
#include <net/sock.h>
#include <linux/inet.h>
#include <linux/types.h>
#include "conn_manager.h"

static DEFINE_SPINLOCK(ctr_lock);
static struct socket *server_socket;
static struct sockaddr_in sin;
static struct task_struct *acceptor_thread;

static int accept_connection(void *socket_in);
int tcp_server_start(void);
void tcp_server_stop(void);
void set_port(int port_param);

void set_port(int port_param) {
    open_port = port_param;
}

int tcp_server_start(void) {
	int ret = 0;
	if (!server_socket) {
		pr_info("Start TCP server on port %d\n", open_port);
		
		//initialise socket address
		memset(&sin, 0, sizeof(sin));
		sin.sin_addr.s_addr = INADDR_ANY;
		sin.sin_family = AF_INET;
		sin.sin_port = htons(open_port);

		//create socket, bind, and listen
		ret = sock_create_kern(&init_net, AF_INET, SOCK_STREAM, IPPROTO_TCP, &server_socket);
		if (ret < 0) return ret;
		ret = server_socket->ops->bind(server_socket, (struct sockaddr *)&sin, sizeof(sin));
		if (ret < 0) return ret;
		ret = server_socket->ops->listen(server_socket, 1);
		if (ret < 0) return ret;

		//accept connection inkernel_sendmsg separate thread
		acceptor_thread = kthread_run(accept_connection, (void *)server_socket, "accept_connection");
	}
	return ret;
}
EXPORT_SYMBOL(tcp_server_start);

static int accept_connection(void *socket_in) {
	int ret_val = 0;
	struct socket *srv_socket = (struct socket *)socket_in;
	struct socket *new_socket;
	char buf[MAX_BUFFER_NET] = {0};
	pr_info("Waiting for connection\n");
	while(!kthread_should_stop()) {
		kernel_accept(srv_socket, &new_socket, 0);
		if (new_socket) {
			struct sockaddr_in connected_client_addr;
			struct msghdr hdr;
			memset(&hdr, 0, sizeof(hdr));
			struct kvec iov = {
				.iov_base = buf,
				.iov_len = sizeof(buf) - 1
			};
			kernel_getpeername(new_socket, (struct sockaddr *)&connected_client_addr);
			pr_info("Connected! client: %pI4\n", &connected_client_addr.sin_addr);
			int len = -1;
			for(;;) {
				len = kernel_recvmsg(new_socket, &hdr, &iov, 1, sizeof(buf) - 1, 0);
				if (len > 0) {
					spin_lock(&ctr_lock);
					memset(message_received, 0, sizeof(message_received));
					strscpy(message_received, buf, sizeof(buf));
					spin_unlock(&ctr_lock);
					pr_info("Data: %s\n", message_received);
				} else if (len == 0) {
					pr_info("Client closed connection.\n");
					break;
				} else if (len == -EAGAIN) {
					msleep(10);
					int accept_connection(void *socket_in);
				} else {
					ret_val = len;
					break;
				}
				//overwrite buf data with NULL char
				memset(buf, 0, sizeof(buf));			
			}
			sock_release(new_socket);
			new_socket = NULL;
			pr_info("Done receiving data\n");
		}
	}
	pr_info("Acceptor thread exit. Bye\n");
	return ret_val;
}

void tcp_server_stop(void) {
    if (task_is_running(acceptor_thread) || acceptor_thread->__state == TASK_NORMAL) {
        kthread_stop(acceptor_thread);
    }
	if (server_socket) {
		pr_info("Release server socket on port %d\n", open_port);
		sock_release(server_socket);
		server_socket = NULL;
	}
}
EXPORT_SYMBOL(tcp_server_stop);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("conn mgr");