#include <linux/socket.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/in.h>
#include <net/sock.h>
#include <linux/inet.h>
#include <linux/types.h>
#include <linux/completion.h> 
#include <linux/syscalls.h>
#include <linux/wait.h>
#include <asm-generic/int-ll64.h>
#include "conn_manager.h"

#define THIS_MOD "cxlshm_tcpconnmgr_in_kernel: "

DEFINE_SPINLOCK(ctr_lock);
EXPORT_SYMBOL(ctr_lock);
DEFINE_SPINLOCK(client_lock);
EXPORT_SYMBOL(client_lock);
DECLARE_COMPLETION(is_complete);
EXPORT_SYMBOL(is_complete);
struct completion *ownership_transfer_arrived = NULL;
struct completion *page_ownership_transfer = NULL;
static struct socket *server_socket, *connected_client_socket, *client_socket;
static struct sockaddr_in sin, client_sockaddr;
static struct task_struct *acceptor_thread, *response_acceptor_thread;
int open_port = DEFAULT_PORT;
char message_received[MAX_BUFFER_NET] = {0};
EXPORT_SYMBOL(message_received);
char response_received[MAX_BUFFER_NET] = {0};
EXPORT_SYMBOL(response_received);
char ownership_transfer_message[MAX_BUFFER_NET] = {0};
EXPORT_SYMBOL(ownership_transfer_message);
char page_ownership_message[MAX_BUFFER_NET] = {0};
EXPORT_SYMBOL(page_ownership_message);
char client_ip_4_addr[16] = {0};
int client_port = 0;

static int accept_connection(void *socket_in);
int _tcp_server_start(void);
int _tcp_server_stop(void);
void set_port(int port_param);
void set_ownership_completion(struct completion *param);
void set_page_ownership_completion(struct completion *param);
int _send_response(char *response_message);

int _tcp_client_start(char *ip_4_addr, int port);
int _send_message(char *message);
int _tcp_client_stop(void);

void set_port(int port_param) {
    open_port = port_param;
	pr_info(THIS_MOD "restart tcp server  using new config");
	_tcp_server_stop();
	_tcp_server_start();
}
EXPORT_SYMBOL(set_port);

void set_ownership_completion(struct completion *param) {
	spin_lock(&ctr_lock);
	ownership_transfer_arrived = param;
	spin_unlock(&ctr_lock);
}
EXPORT_SYMBOL(set_ownership_completion);

void set_page_ownership_completion(struct completion *param) {
	spin_lock(&ctr_lock);
	page_ownership_transfer = param;
	spin_unlock(&ctr_lock);
}
EXPORT_SYMBOL(set_page_ownership_completion);

int _tcp_server_start(void) 
{
	int ret = 0;
	if (server_socket)
		server_socket = NULL;
	if (!server_socket) 
	{
		pr_info(THIS_MOD "start TCP server on port %d\n", open_port);
		
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
EXPORT_SYMBOL(_tcp_server_start);

static int accept_connection(void *socket_in) 
{
	int ret_val = 0;
	struct socket *srv_socket = (struct socket *)socket_in;
	struct socket *new_socket;
	char buf[MAX_BUFFER_NET] = {0};
	pr_info(THIS_MOD "waiting for connection\n");
	while(!kthread_should_stop()) 
	{
		int ret = kernel_accept(srv_socket, &new_socket, SOCK_NONBLOCK);
		if (ret == 0) 
		{
			struct sockaddr_in connected_server_addr;
			struct msghdr hdr;
			memset(&hdr, 0, sizeof(hdr));
			struct kvec iov = 
			{
				.iov_base = buf,
				.iov_len = sizeof(buf) - 1
			};
			kernel_getpeername(new_socket, (struct sockaddr *)&connected_server_addr);
			pr_info(THIS_MOD "connected! client: %pI4\n", &connected_server_addr.sin_addr);
			int len = -1;
			connected_client_socket = new_socket;
			while(!kthread_should_stop()) 
			{
				len = kernel_recvmsg(new_socket, &hdr, &iov, 1, sizeof(buf) - 1, 0);
				if (len > 0)
				{
					int ready = 0;
					spin_lock(&ctr_lock);
					memset(message_received, 0, sizeof(message_received));
					if (strncmp(buf, "PFN:", 4) == 0) 
					{
						strscpy(page_ownership_message, buf, sizeof(buf));
						ready = 1;
					} 
					else if (strncmp(buf, enum_message_value[WHOLE_VMA], strlen(enum_message_value[WHOLE_VMA])) == 0) 
					{
						strscpy(ownership_transfer_message, buf, sizeof(buf));
						ready = 2;
					}
					else
					{
						pr_info(THIS_MOD "unknown message received. Discarded\n");
					}
					spin_unlock(&ctr_lock);
					if (ready == 1) 
					{
						pr_info(THIS_MOD "page invalidation request %s\n", page_ownership_message);
						if (page_ownership_transfer != NULL)
							complete(page_ownership_transfer);
						else
							pr_info(THIS_MOD "page ownership completion is not set\n");
					}
					else if (ready == 2) 
					{
						pr_info(THIS_MOD "Invalidation request %s\n", ownership_transfer_message);
						if (ownership_transfer_arrived != NULL)
							complete(ownership_transfer_arrived);
					}
				} 
				else if (len == 0) 
				{
					pr_info(THIS_MOD "client closed connection.\n");
					break;
				}
				else if (len == -EAGAIN) 
				{
					pr_info(THIS_MOD "socket not available\n");
					msleep(10);
				}
				else
				{
					pr_info(THIS_MOD "kernel_recvmsg returned %d\n", len);
					ret_val = len;
					break;
				}
				//overwrite buf data with NULL char
				memset(buf, 0, sizeof(buf));
			}
			sock_release(new_socket);
			new_socket = NULL;
			connected_client_socket = NULL;
			pr_info(THIS_MOD "done receiving data\n");
		}
		else if (ret == -EAGAIN)
		{
			continue;
		}
	}
	sock_release(srv_socket);
	pr_info(THIS_MOD "acceptor thread exit. Bye\n");
	return ret_val;
}

int _send_response(char *response_message) {
	char msg[MAX_BUFFER_NET] = {0};
	int len = strscpy(msg, response_message, sizeof(msg));
	pr_info(THIS_MOD "sending response %s length %d\n", msg, len);
	int ret = 0;
	if (connected_client_socket) {
		struct msghdr hdr;
		memset(&hdr, 0, sizeof(hdr));
		struct kvec iov = {
			.iov_base = response_message,
			.iov_len = sizeof(msg)
		};
		ret = kernel_sendmsg(connected_client_socket, &hdr, &iov, 1, strlen(msg));
		pr_info(THIS_MOD "sent response %d bytes\n", ret);
	} else {
		pr_info(THIS_MOD "connected client socket is not available\n");
		ret = -EAGAIN;
	}
	return ret;
}
EXPORT_SYMBOL(_send_response);

int _tcp_server_stop(void) {
	int thread_ret = 0;
    if (task_is_running(acceptor_thread)) {
        thread_ret = kthread_stop(acceptor_thread);
    }
	pr_info(THIS_MOD "stop acceptor thread\n");

	return 0;
}
EXPORT_SYMBOL(_tcp_server_stop);

//client. move to here for easy recompiling
int _tcp_client_start(char *ip_4_addr, int port) {
	int ret = 0;
	if (!client_socket) {
		strscpy(client_ip_4_addr, ip_4_addr, strlen(ip_4_addr) + 1);
		client_port = port;
		pr_info(THIS_MOD "connecting to: %s port %d\n", client_ip_4_addr, client_port);
		ret = sock_create_kern(&init_net, AF_INET, SOCK_STREAM, IPPROTO_TCP, &client_socket);
		if (ret < 0) return ret;
		memset(&client_sockaddr, 0, sizeof(client_sockaddr));
		//initialise client socket address
		client_sockaddr.sin_family = AF_INET;
		client_sockaddr.sin_port = htons(port);
		int ret_ip = in4_pton(ip_4_addr, INET_ADDRSTRLEN, (u8 *)&client_sockaddr.sin_addr.s_addr, -1, NULL);
		if (ret_ip == 0) return -EINVAL;
		ret = client_socket->ops->connect(client_socket, (struct sockaddr *)&client_sockaddr, sizeof(client_sockaddr), 0);
		if (ret < 0) return ret;
	} else {
		pr_info(THIS_MOD "there is a client socket\n");
		ret = -1;
	}
	return ret;
}
EXPORT_SYMBOL(_tcp_client_start);

static int wait_for_response(void *socket_in) 
{
	int ret_val = 0;
	struct socket *clnt_socket = (struct socket *)socket_in;
	char buf[MAX_BUFFER_NET] = {0};
	pr_info(THIS_MOD "waiting for response\n");
	while(!kthread_should_stop()) 
	{
		if (clnt_socket) {
			struct sockaddr_in connected_client_addr;
			struct msghdr hdr;
			memset(&hdr, 0, sizeof(hdr));
			struct kvec iov = 
			{
				.iov_base = buf,
				.iov_len = sizeof(buf) - 1
			};
			kernel_getpeername(clnt_socket, (struct sockaddr *)&connected_client_addr);
			pr_info(THIS_MOD "connected! server: %pI4\n", &connected_client_addr.sin_addr);
			int len = -1;
			while(!kthread_should_stop()) 
			{
				len = kernel_recvmsg(clnt_socket, &hdr, &iov, 1, sizeof(buf) - 1, MSG_DONTWAIT);
				if (len > 0) 
				{
					int ready = 0;
					spin_lock(&client_lock);
					memset(response_received, 0, sizeof(response_received));
					if (strncmp(buf, "DONE", 4) == 0) {
						strscpy(response_received, buf, sizeof(buf));
						ready = 1;
					} else {
						pr_info(THIS_MOD "server responds with unknown message. Discarded\n");
					}
					spin_unlock(&client_lock);
					if (ready == 1) {
						pr_info(THIS_MOD "response message: invalidation completion\n");
						complete(&is_complete);
					}
				}
				else if (len == 0)
				{
					pr_info(THIS_MOD "no response received\n");
					break;
				}
				else if (len == -EAGAIN) 
				{
					continue;
				}
				else
				{
					ret_val = len;
					break;
				}
				//overwrite buf data with NULL char
				memset(buf, 0, sizeof(buf));
				
			}
			pr_info(THIS_MOD "disconnect from server %s port %d\n", client_ip_4_addr, client_port);
			sock_release(clnt_socket);
			clnt_socket = NULL;
			pr_info(THIS_MOD "done receiving response\n");
			break;
		}
		else 
		{
			pr_info(THIS_MOD "no client socket\n");
			break;
		}
	}
	
	pr_info(THIS_MOD "response waiter thread exit. Bye\n");
	return ret_val;
}

int _send_message(char *message) {
	char msg[MAX_BUFFER_NET] = {0};
	int len = strscpy(msg, message, sizeof(msg));
	pr_info(THIS_MOD "sending message %s length %d\n", msg, len);
	int ret = 0;
	if (client_socket) {
		struct msghdr hdr;
		memset(&hdr, 0, sizeof(hdr));
		struct kvec iov = {
			.iov_base = message,
			.iov_len = sizeof(msg)
		};
		ret = kernel_sendmsg(client_socket, &hdr, &iov, 1, strlen(msg));
		if (ret >= 0) {
			pr_info(THIS_MOD "sent %d bytes\n", ret);
			//accept connection inkernel_sendmsg separate thread
			response_acceptor_thread = kthread_run(wait_for_response, (void *)client_socket, "wait_for_response");
		} else {
			pr_info(THIS_MOD "error happened when sending message: %d\n", ret);
		}
	} else {
		pr_info(THIS_MOD "client socket is not available\n");
	}
	return ret;
}
EXPORT_SYMBOL(_send_message);

int _tcp_client_stop(void) {
	int ret = 0;
	if (client_socket) {
		if (task_is_running(response_acceptor_thread)) {
			kthread_stop(response_acceptor_thread);
		}
	}
	client_socket = NULL;
	return ret;
}
EXPORT_SYMBOL(_tcp_client_stop);

SYSCALL_DEFINE2(tcp_client_start, char __user *, ip_v4_addr, int, server_port)
{
	char *ip_4_addr = kzalloc(sizeof(char) * 17, GFP_KERNEL);
	memset(ip_4_addr, 0, 17);
	int ret = strncpy_from_user(ip_4_addr, ip_v4_addr, sizeof(ip_4_addr));
	if (ret < 0) return -EFAULT;
	if (ret >= sizeof(ip_4_addr) || ret == 0) return -EINVAL;
	return _tcp_client_start(ip_4_addr, server_port);
}

SYSCALL_DEFINE1(send_message, char __user *, message)
{
	char message_buf[128] = {0};
	int ret = strncpy_from_user(message_buf, message, sizeof(message_buf));
	if (ret < 0) return -EFAULT;
	if (ret >= sizeof(message_buf) || ret == 0) return -EINVAL;
	return _send_message(message_buf);
}

SYSCALL_DEFINE0(tcp_client_stop)
{
	return _tcp_client_stop();
}


//tcp server syscall
SYSCALL_DEFINE1(tcp_server_start, int, server_port)
{
	if (server_port > 0)
		open_port = server_port;
	return _tcp_server_start();
}

SYSCALL_DEFINE1(send_response, char __user *, message)
{
	char message_buf[128] = {0};
	int ret = strncpy_from_user(message_buf, message, sizeof(message_buf));
	if (ret < 0) return -EFAULT;
	if (ret >= sizeof(message_buf) || ret == 0) return -EINVAL;
	return _send_response(message_buf);
}

SYSCALL_DEFINE0(tcp_server_stop)
{
	return _tcp_server_stop();
}


/*
static int __init cxlshm_tcpconnmgr_init(void) {	
    tcp_server_start();

	//init done
	pr_info(THIS_MOD "loaded\n");
	return 0;
}

static void __exit cxlshm_tcpconnmgr_exit(void) {
    //stopping tcp server
	tcp_server_stop();
	//exit done
	pr_info(THIS_MOD ": unloaded\n"); 
}

module_init(cxlshm_tcpconnmgr_init);
module_exit(cxlshm_tcpconnmgr_exit);
*/
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TCP connection manager for CXLSHM");