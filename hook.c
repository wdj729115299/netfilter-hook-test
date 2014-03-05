#include <linux/etherdevice.h>
#include <linux/fs.h>
#include <linux/ip.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/tcp.h>
#include <linux/version.h>
#include <linux/wait.h>
#include <asm/uaccess.h>

#include "hook.h"

#define NH_MINOR 214

static LIST_HEAD(current_skbs);
static DEFINE_SPINLOCK(skb_list_lock);
struct skb_entry {
	struct list_head list;
	struct sk_buff *skb;
};

#if defined(__DEBUG__)
static unsigned int list_size(struct list_head *head)
{
	unsigned int count = 0;
	struct list_head *pos;
	list_for_each(pos, head)
		count++;
	return count;
}
#endif


/*
 * Must hold skb_list_lock
 */
static struct skb_entry *is_hooked(struct sk_buff *skb)
{
	struct skb_entry *e;
	int found = 0;

	list_for_each_entry(e, &current_skbs, list) {
		if(e->skb == skb) {
			found = 1;
			break;
		}
	}

	return found ? e : NULL;
}

static LIST_HEAD(nh_privs);
static DEFINE_SPINLOCK(nh_privs_lock);

struct nh_private {
	struct list_head list;
	struct nh_filter *filter;
	struct nh_writer *writer;
	struct sk_buff_head skb_queue ;
	wait_queue_head_t wq;
};

static struct nf_hook_ops *cb_in_use[NF_IP_NUMHOOKS + 1];

enum {
	CHECK_PROTO 	= (1 << 0),
	CHECK_OUT 	= (1 << 1),
	CHECK_IN 	= (1 << 2),
	CHECK_SADDR 	= (1 << 3),
	CHECK_DADDR 	= (1 << 4),
	CHECK_SPORT 	= (1 << 5),
	CHECK_DPORT 	= (1 << 6),
};

/*
 * Must hold the nh_privs_lock
 */
static struct nh_private *pass(struct sk_buff *skb,
				 const struct net_device *in,
				 const struct net_device *out,
				 int hooknum)
{
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,22)
	struct iphdr *iph = (struct iphdr *)skb_network_header(skb);
	struct tcphdr *tph = (struct tcphdr *)skb_transport_header(skb);
#else
	struct iphdr *iph = skb->nh.iph;
	struct tcphdr *tph = skb->h.th;
#endif
	struct nh_private *e;

	list_for_each_entry(e, &nh_privs, list) {
		if (e->filter->hooknum != hooknum) {
			continue;
		}
		if ((e->filter->flags & CHECK_OUT) && e->filter->out != out) {
			continue;
		}
		if ((e->filter->flags & CHECK_IN) && e->filter->in != in) {
			continue;
		}

		if (iph) {
			if ((e->filter->flags & CHECK_PROTO) && e->filter->proto != iph->protocol) {
				continue;
			}
			if ((e->filter->flags & CHECK_SADDR) && e->filter->saddr != iph->saddr) {
				continue;
			}
			if ((e->filter->flags & CHECK_DADDR) && e->filter->daddr != iph->daddr) {
				continue;
			}
		}

		if (tph) {
			if ((e->filter->flags & CHECK_SPORT) && e->filter->sport != tph->source) {
				continue;
			}
			if ((e->filter->flags & CHECK_DPORT) && e->filter->dport != tph->dest) {
				continue;
			}
		}

		return e;
	}

	return NULL;
}

static unsigned int nf_cb(
		unsigned int hooknum,
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,23)
		struct sk_buff *skb,
#else
		struct sk_buff **pskb,
#endif
		const struct net_device *in, const struct net_device *out,
		int (*okfn)(struct sk_buff *))
{
	struct nh_private *p;
	struct skb_entry *e;
	unsigned long flags;
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,23)
	struct sk_buff **pskb = &skb;
#endif

	/* if the packet was hooked once, don't send it back to user space */
	spin_lock_irqsave(&skb_list_lock, flags);
	e = is_hooked(*pskb);
	if (e) {
		list_del(&e->list);
		spin_unlock_irqrestore(&skb_list_lock, flags);
		kfree(e);
		return NF_ACCEPT;
	}
	spin_unlock_irqrestore(&skb_list_lock, flags);

	spin_lock_irqsave(&nh_privs_lock, flags);
	p = pass(*pskb, in, out, hooknum);
	if (p) {
		skb_queue_tail(&p->skb_queue, *pskb);
		spin_unlock_irqrestore(&nh_privs_lock, flags);

		wake_up_interruptible(&p->wq);
		return NF_STOLEN;
	}
	spin_unlock_irqrestore(&nh_privs_lock, flags);
	return NF_ACCEPT;
}


#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,24)
#define NET_NAMESPACE
#else
#define NET_NAMESPACE &init_net,
#endif

static int setup_filter(struct nh_private *p)
{
	struct nh_filter *f = p->filter;
	struct nf_hook_ops *nf_hook;
	int ret = 0;

	if (f->hooknum < 0 || f->hooknum > (NF_IP_NUMHOOKS - 1))
		return -EINVAL;

	nf_hook = kzalloc(sizeof(*nf_hook), GFP_KERNEL);
	if (!nf_hook)
		return -ENOMEM;

	f->in = dev_get_by_name(NET_NAMESPACE f->in_dev);
	if (f->in)
		f->flags |= CHECK_IN;
	f->out = dev_get_by_name(NET_NAMESPACE f->out_dev);
	if (f->out)
	       f->flags |= CHECK_OUT;
	if (f->saddr)
	       f->flags |= CHECK_SADDR;
	if (f->daddr)
	       f->flags |= CHECK_DADDR;
	if (f->dport)
	       f->flags |= CHECK_DPORT;
	if (f->sport)
	       f->flags |= CHECK_SPORT;
	if (f->proto)
		f->flags |= CHECK_PROTO;

	if (!cb_in_use[f->hooknum]) {
		nf_hook->hook = nf_cb;
		nf_hook->owner = THIS_MODULE;
		nf_hook->pf = PF_INET;
		nf_hook->hooknum = f->hooknum;
		nf_hook->priority = f->priority;
		ret = nf_register_hook(nf_hook);
		if (ret < 0) {
			printk("nf_hook: can't register netfilter hook.\n");
			kfree(nf_hook);
			goto err;
		}
		cb_in_use[f->hooknum] = nf_hook;
	}

	return 0;
err:
	kfree(nf_hook);
	return ret;
}

static int nh_open(struct inode *inode, struct file *file)
{
	struct nh_private *p;

	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	init_waitqueue_head(&p->wq);
	skb_queue_head_init(&p->skb_queue);

	file->private_data = p;

	return 0;
}
static int nh_release(struct inode *inode, struct file *file)
{
	struct nh_private *p = file->private_data;
	unsigned long flags;

	spin_lock_irqsave(&nh_privs_lock, flags);

	if (p->filter) {
		list_del(&p->list);
		if (p->filter->in)
			dev_put(p->filter->in);
		if (p->filter->out)
			dev_put(p->filter->out);
		kfree(p->filter);
	}
	while (!skb_queue_empty(&p->skb_queue)) {
		struct sk_buff *skb;
		skb = skb_dequeue(&p->skb_queue);
		kfree_skb(skb);
	}

	if (p->writer) {
		if (p->writer->dest_dev)
			dev_put(p->writer->dest_dev);
		kfree(p->writer);
	}

	spin_unlock_irqrestore(&nh_privs_lock, flags);

	kfree(p);
	return 0;
}

static ssize_t nh_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
	struct nh_private *p;
	struct sk_buff *skb;
	int ret;
	unsigned long flags;

	p = file->private_data;

	if (!p->writer) {
		return -EBADF;
	}

	skb = dev_alloc_skb(count + 2);
	if (!skb)
		return -ENOMEM;

	skb_reserve(skb, 2);

	if (copy_from_user(skb_put(skb, count), buf, count)) {
		kfree_skb(skb);
		printk("nh_write: failed copy_from_user %d\n", count);
		return -EFAULT;
	}

	if (p->writer->mode == TO_ROUTING_STACK) {
		struct skb_entry *e;
	        e = kmalloc(sizeof(*e), GFP_ATOMIC);
		if (!e) {
			kfree_skb(skb);
			return -ENOMEM;
		}
		e->skb = skb;

		/* mark this skb as 'ours' */
		spin_lock_irqsave(&skb_list_lock, flags);
		list_add(&e->list, &current_skbs);
		spin_unlock_irqrestore(&skb_list_lock, flags);

		skb->dev = p->writer->dest_dev; /* needed for <= 2.6.18 */
		skb->protocol = eth_type_trans(skb, p->writer->dest_dev);

		ret = netif_rx_ni(skb);
	} else {
		struct net_device *dev;

		skb->dev = p->writer->dest_dev;
		skb->protocol = be16_to_cpu(0x0800);
		skb_pull(skb, sizeof(struct ethhdr));

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,22)
		skb_reset_network_header(skb);
#endif
		skb->protocol = __constant_htons(ETH_P_IP);

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,24)
		if (skb->dev->hard_header)
			skb->dev->hard_header(skb, skb->dev, be16_to_cpu(skb->protocol), NULL, skb->dev->dev_addr, skb->len);
#else
		dev_hard_header(skb, skb->dev, be16_to_cpu(skb->protocol), NULL, skb->dev->dev_addr, skb->len);
#endif

		switch (p->writer->mode) {
		case TO_INTERFACE:
			dev = skb->dev;

			spin_lock_irqsave(&dev->_tx->_xmit_lock, flags);
			dev->_tx->xmit_lock_owner = smp_processor_id();

			if (!netif_queue_stopped(skb->dev)
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,18)
			    && !netif_subqueue_stopped(skb->dev, skb)
#endif
			   ) {
				ret = skb->dev->netdev_ops->ndo_start_xmit(skb, skb->dev);
			}

			dev->_tx->xmit_lock_owner = -1;
			spin_unlock_irqrestore(&dev->_tx->_xmit_lock, flags);

			break;
		case TO_INTERFACE_QUEUE:
		        ret = dev_queue_xmit(skb);
			break;
		default:
			WARN_ON_ONCE(1);
		}
	}


	return count;
}

static ssize_t nh_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	struct nh_private *p;
	struct sk_buff *skb;
	int ret;
	unsigned long flags;

	if (!count)
		return count;

	p = file->private_data;

	if (!p->filter) {
		return -EBADF;
	}

wait_skb:
	wait_event_interruptible(p->wq, !(skb_queue_empty(&p->skb_queue)));
	if (signal_pending(current))
		return -ERESTARTSYS;

	spin_lock_irqsave(&nh_privs_lock, flags);
	skb = skb_dequeue(&p->skb_queue);
	spin_unlock_irqrestore(&nh_privs_lock, flags);

	if (!skb)
		goto wait_skb;

	/* Save the dest mac now, it will be lost otherwise */
	if (skb_dst(skb) && dst_get_neighbour(skb_dst(skb))) {
		struct ethhdr *eth;

		skb_push(skb, sizeof(struct ethhdr));
		eth = (struct ethhdr *)skb->data;
		skb_pull(skb, sizeof(struct ethhdr));
		memcpy(eth->h_dest, dst_get_neighbour(skb_dst(skb))->ha, ETH_ALEN);
	}

	skb_push(skb, ETH_HLEN);

	if (skb->len > count) {
		kfree_skb(skb);
		return -EINVAL;
	}

	ret = skb->len;
	if (copy_to_user(buf, skb->data, skb->len)) {
		printk("failed copy_to_user %d\n", skb->len);
		kfree_skb(skb);
		return -EFAULT;
	}

	kfree_skb(skb);

	return ret;
}

static long nh_ioctl(struct file *file,
		    unsigned int req, unsigned long pointer)
{
	struct nh_private *p;
	struct nh_filter *filter;
	struct nh_writer *writer;
	int ret;
	unsigned long flags;

	p = file->private_data;

	switch (req) {
	case NH_SET_FILTER:
		filter = kzalloc(sizeof(*filter), GFP_KERNEL);
		if (!filter)
			return -ENOMEM;

		if (copy_from_user(filter, (void __user*)pointer, sizeof(*filter))) {
			return -EFAULT;
		}


		p->filter = filter;
		ret = setup_filter(p);

#if 0
		printk("Got filter:\n\
				u8 proto = %d\n\
				u32 saddr = %ld\n\
				u32 daddr = %ld\n\
				u16 dport = %d\n\
				u16 sport = %d\n\
				char in_dev[255] = %s\n\
				char out_dev[255] = %s\n\
				int priority = %d\n\
				int hooknum = %d\n\
				int flags = %d\n", filter->proto, filter->saddr, filter->daddr, filter->dport, filter->sport,
						   filter->in_dev, filter->out_dev, filter->priority, filter->hooknum, filter->flags);
#endif

		if (!ret) {
			spin_lock_irqsave(&nh_privs_lock, flags);
			list_add(&p->list, &nh_privs);
			spin_unlock_irqrestore(&nh_privs_lock, flags);
		} else {
			kfree(p->filter);
		}


		return ret;
	case NH_RM_FILTER:
		if (p->filter) {
			spin_lock_irqsave(&nh_privs_lock, flags);
			list_del(&p->list);
			spin_unlock_irqrestore(&nh_privs_lock, flags);
			kfree(p->filter);
		}
		return 0;
	case NH_SET_WRITE_MODE:
		writer = kzalloc(sizeof(*writer), GFP_KERNEL);
		if (!writer)
			return -ENOMEM;

		if (copy_from_user(writer, (void __user *)pointer, sizeof(*writer)))
			return -EFAULT;

		p->writer = writer;
		p->writer->dest_dev = dev_get_by_name(NET_NAMESPACE writer->dest_dev_str);
		return 0;
	}

	return -EINVAL;
}

static const struct file_operations net_hook_fops = {
	.owner		= THIS_MODULE,
	.open		= nh_open,
	.release	= nh_release,
	.unlocked_ioctl		= nh_ioctl,
	.read		= nh_read,
	.write		= nh_write,
};


static struct miscdevice net_hook_dev = {
	NH_MINOR,
	"net_hook",
	&net_hook_fops
};

static int __init nh_init(void)
{
	int ret;

	ret = misc_register(&net_hook_dev);
	if (ret)
		printk(KERN_ERR "net_hook: can't misc_register on minor %d\n", NH_MINOR);

	printk("hk: module loaded\n");
	return ret;
}
module_init(nh_init);

static void __exit nh_exit(void)
{
	int i;
	for (i = 0; i < ARRAY_SIZE(cb_in_use); i++) {
		if (cb_in_use[i]) {
			nf_unregister_hook(cb_in_use[i]);
			kfree(cb_in_use[i]);
		}
	}
	misc_deregister(&net_hook_dev);
	printk("hk: module unloaded\n");
}
module_exit(nh_exit);

