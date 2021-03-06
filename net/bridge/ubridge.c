#define DRV_NAME		"ubridge"
#define DRV_VERSION		"0.4"
#define DRV_DESCRIPTION	"Tiny bridge driver"
#define DRV_COPYRIGHT	"(C) 2012 NDM Systems Inc. <ap@ndmsystems.com>"

#define UBRIDGE_MINOR	201


#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/ctype.h>
#include <linux/if.h>
#include <linux/if_ether.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/rtnetlink.h>
#include <linux/if_bridge.h>
#include <linux/netfilter_bridge.h>
#include <../net/8021q/vlan.h>
#include "br_private.h"

#define BR_PORT_BITS	10
#define BR_MAX_PORTS	(1<<BR_PORT_BITS)

static LIST_HEAD(ubr_list);

struct ubr_private {
	struct net_device		*slave_dev;
	struct net_device_stats	stats;
	struct list_head		list;
	struct net_device		*dev;
};

static int ubr_dev_ioctl(struct net_device *, struct ifreq *, int);



static struct sk_buff *ubr_handle_frame(struct net_bridge_port *p, struct sk_buff *skb)
{
	struct ubr_private *ubr, *tmp;

//	printk(KERN_ERR"handler(proto=0x%x): %d bytes\n", ntohs(skb->protocol) ,skb->len);

	list_for_each_entry_safe(ubr, tmp, &ubr_list, list) {
		if (skb->dev == ubr->slave_dev) {
			skb->dev = ubr->dev;
			skb->pkt_type = PACKET_HOST;
			ubr->dev->last_rx = jiffies;

			ubr->stats.rx_packets++;
			ubr->stats.rx_bytes += skb->len;

			netif_receive_skb(skb);
			return NULL;
		}
	}
	return skb;

}


static int ubr_open(struct net_device *master_dev)
{
	netif_start_queue(master_dev);
	return 0;
}

static int ubr_stop(struct net_device *master_dev)
{
	netif_stop_queue(master_dev);
	return 0;
}

static int ubr_xmit(struct sk_buff *skb, struct net_device *master_dev)
{
	struct ubr_private *master_info = netdev_priv(master_dev);
	struct net_device *slave_dev = master_info->slave_dev;

	if (!slave_dev) {
		dev_kfree_skb(skb);
		return NETDEV_TX_OK;
	}

	master_info->stats.tx_packets++;
	master_info->stats.tx_bytes += skb->len;

	skb->dev = slave_dev;
	dev_queue_xmit(skb);

	return NETDEV_TX_OK;
}

static struct net_device_stats *ubr_get_stats(struct net_device *dev)
{
	struct ubr_private *ubr = netdev_priv(dev);

	return &ubr->stats;
}

#ifdef HAVE_CHANGE_RX_FLAGS
void ubr_change_rx_flags(struct net_device *dev,
						int flags)
{
	if (flags & IFF_PROMISC) {
		struct ubr_private *master_info = netdev_priv(dev);
		struct net_device *slave_dev = master_info->slave_dev;

		printk(KERN_INFO "%s promiscuous mode for ubridge\n",
				dev->flags & IFF_PROMISC? "Set": "Clear");

		if (slave_dev)
			dev_set_promiscuity(slave_dev, dev->flags & IFF_PROMISC? 1: -1);
	}
}
#endif

/* RTNL locked */
static int ubr_deregister(struct net_device *dev)
{
	struct ubr_private *ubr = netdev_priv(dev);

	dev_close(dev);

	if (!list_empty(&ubr->list))
		list_del_init(&ubr->list);

	if (ubr->slave_dev) {
		ubr_handle_frame_hook = NULL;
		//kobject_del(&p->kobj);	// no need
	}
	unregister_netdevice(dev);

	return 0;
}

static int ubr_free_master(const char *name)
{
	struct net_device *dev;
	int ret = 0;

	rtnl_lock();
	dev = __dev_get_by_name(name);
	if (dev == NULL)
		ret =  -ENXIO; 	/* Could not find device */
	else if (dev->flags & IFF_UP)
		/* Not shutdown yet. */
		ret = -EBUSY;
	else
		ret = ubr_deregister(dev);

	rtnl_unlock();
	return ret;
}

static int ubr_alloc_master(const char *name)
{
	struct net_device *dev;
	struct ubr_private *ubr;
	int err = 0;

	dev = alloc_netdev(sizeof(struct ubr_private), name, ether_setup);
	if (!dev)
		return -ENOMEM;

	ubr = netdev_priv(dev);
	ubr->dev = dev;

	random_ether_addr(dev->dev_addr);

	dev->tx_queue_len	= 0; /* A queue is silly for a loopback device */
	dev->features		= NETIF_F_FRAGLIST
						| NETIF_F_HIGHDMA
						| NETIF_F_LLTX;
	dev->flags		= IFF_BROADCAST | IFF_MULTICAST;
	dev->destructor		= free_netdev;

	dev->do_ioctl = ubr_dev_ioctl;
	dev->get_stats = ubr_get_stats;
	dev->hard_start_xmit = ubr_xmit;
	dev->open = ubr_open;
	dev->stop = ubr_stop;

#ifdef HAVE_CHANGE_RX_FLAGS
	dev->change_rx_flags = ubr_change_rx_flags;
#endif

	err = register_netdev(dev);
	if (err) {
		free_netdev(dev);
		dev = ERR_PTR(err);
		goto out;
	}

	netif_carrier_off(dev);

	rtnl_lock();
	list_add(&ubr->list, &ubr_list);
	rtnl_unlock();

out:
	return err;
}

static int ubr_atto_master(struct net_device *master_dev, int ifindex)
{
	struct net_device *dev1, *vlan_dev;
	struct ubr_private *ubr0 = netdev_priv(master_dev);
	int err = -ENODEV;

	if (ubr0->slave_dev != NULL)
		return -EBUSY;

	dev1 = __dev_get_by_index(ifindex);
	if (!dev1)
		goto out;

	memcpy(master_dev->dev_addr, dev1->dev_addr, ETH_ALEN);
	call_netdevice_notifiers(NETDEV_CHANGEADDR, master_dev);
	ubr0->slave_dev = dev1;

	// Update all VLAN sub-devices' MAC address
	for_each_netdev(vlan_dev) {
		struct vlan_dev_info *vdi;

		if (!(vlan_dev->priv_flags & IFF_802_1Q_VLAN))
			continue;

		if ((vdi = netdev_priv(vlan_dev)) &&
		     vdi->real_dev == master_dev) {
			struct sockaddr addr;

			memcpy(addr.sa_data, dev1->dev_addr, ETH_ALEN);
			addr.sa_family = master_dev->type;

			err = dev_set_mac_address(vlan_dev, &addr);
			if (err)
				printk(KERN_ERR "can't set MAC for device %s (error %d)\n",
						vlan_dev->name, err);
		}
	}

	ubr_handle_frame_hook = ubr_handle_frame;

	if (master_dev->flags & IFF_PROMISC)
		dev_set_promiscuity(dev1, 1);

	netif_carrier_on(master_dev);
	err = 0;

out:
	return err;
}

static int ubr_detach(struct net_device *master_dev, int ifindex)
{
	struct net_device *dev1;
	struct ubr_private *ubr0 = netdev_priv(master_dev);
	int err = -EINVAL;

	dev1 = __dev_get_by_index(ifindex);
	if (!dev1)
		goto out;

	if (ubr0->slave_dev != dev1)
		goto out;
	ubr0->slave_dev = NULL;

	if (master_dev->flags & IFF_PROMISC)
		dev_set_promiscuity(dev1, -1);

	ubr_handle_frame_hook = NULL;
	err = 0;

out:
	return err;
}

#define SHOW_BUF_MAX_LEN	4096

static long ubr_show(char *buf, long len)
{
	long written = 0;
	struct ubr_private *ubr_item;

	if (len == 0 || len > SHOW_BUF_MAX_LEN)
		len = SHOW_BUF_MAX_LEN;

	list_for_each_entry(ubr_item, &ubr_list, list) {
		written += snprintf(buf + written, len - written, "%-16s %02hx%02hx%02hx%02hx%02hx%02hx\t",
				ubr_item->dev->name, ubr_item->dev->dev_addr[0], ubr_item->dev->dev_addr[1],
				ubr_item->dev->dev_addr[2], ubr_item->dev->dev_addr[3], ubr_item->dev->dev_addr[4],
				ubr_item->dev->dev_addr[5]);
		if (written >= len - 2)
			break;

		if (ubr_item->slave_dev == NULL)
			written += sprintf(buf + written, "-\n");
		else
			written += snprintf(buf + written, len - written, "%s\n", ubr_item->slave_dev->name);
		if (written >= len - 1)
			break;
	}

	return written;
}

int ubr_ioctl_deviceless_stub(unsigned int cmd, void __user *uarg)
{
	char buf[IFNAMSIZ];

	switch (cmd) {
	case SIOCUBRADDBR:
	case SIOCUBRDELBR:
		if (copy_from_user(buf, uarg, IFNAMSIZ))
			return -EFAULT;

		buf[IFNAMSIZ-1] = 0;
		if (cmd == SIOCUBRADDBR)
			return ubr_alloc_master(buf);

		return ubr_free_master(buf);
	case SIOCUBRSHOW:
		{
			char *buf_;
			long res;
			struct {
				long len;
				char *buf;
			} args;

			if (copy_from_user(&args, uarg, sizeof(args)))
				return -EFAULT;
			buf_ = kmalloc(SHOW_BUF_MAX_LEN, GFP_KERNEL);
			if (buf_ == NULL)
				return -ENOMEM;
			memset(buf_, 0, SHOW_BUF_MAX_LEN);
			res = ubr_show(buf_, args.len);
			if (copy_to_user(args.buf, buf_, res) ||
					copy_to_user(uarg, &res, sizeof(long))) {
				kfree(buf_);
				return -EFAULT;
			}
			kfree(buf_);
			return 0;
		}
	}
	return -EOPNOTSUPP;
}

static int ubr_dev_ioctl(struct net_device *dev, struct ifreq *rq, int cmd)
{
	switch (cmd) {
	case SIOCBRADDIF:
		return ubr_atto_master(dev, rq->ifr_ifindex);

	case SIOCBRDELIF:
		return ubr_detach(dev, rq->ifr_ifindex);

	}
	return -EOPNOTSUPP;
}

static int ubr_dev_event(
		struct notifier_block *unused,
		unsigned long event,
		void *ptr)
{
	struct net_device *pdev = ptr;
	struct ubr_private *ubr_item;

	switch (event) {
		case NETDEV_UNREGISTER:
			list_for_each_entry(ubr_item, &ubr_list, list) {
				if (ubr_item->slave_dev == pdev) {
					/* delif */
					ubr_handle_frame_hook = NULL;
					ubr_item->slave_dev = NULL;
				}
			}
			break;
	}
	return NOTIFY_DONE;
}

static struct notifier_block ubr_device_notifier = {
	.notifier_call  = ubr_dev_event,
};

static int __init ubridge_init(void)
{
	ubrioctl_set(ubr_ioctl_deviceless_stub);
	printk(KERN_INFO "ubridge: %s, %s\n", DRV_DESCRIPTION, DRV_VERSION);
	if (register_netdevice_notifier(&ubr_device_notifier))
		printk(KERN_ERR "%s: Error regitering notifier\n", __func__);
	return 0;
}

static void __exit ubridge_exit(void)
{
	struct ubr_private *ubr, *tmp;

	unregister_netdevice_notifier(&ubr_device_notifier);
	rtnl_lock();
	list_for_each_entry_safe(ubr, tmp, &ubr_list, list) {
		ubr_deregister(ubr->dev);
	}
	rtnl_unlock();
	ubrioctl_set(NULL);

	printk(KERN_INFO "ubridge: driver unloaded\n");
}

/*
module_param_call(newif, ubr_newif, ubr_noget, NULL, S_IWUSR);
module_param_call(attachif, ubr_attachif, ubr_noget, NULL, S_IWUSR);
module_param_call(detachif, ubr_detachif, ubr_noget, NULL, S_IWUSR);
module_param_call(delif, ubr_delif, ubr_noget, NULL, S_IWUSR);
*/
module_init(ubridge_init);
module_exit(ubridge_exit);
MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_AUTHOR(DRV_COPYRIGHT);
MODULE_LICENSE("GPL");

