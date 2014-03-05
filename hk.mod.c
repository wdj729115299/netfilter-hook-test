#include <linux/module.h>
#include <linux/vermagic.h>
#include <linux/compiler.h>

MODULE_INFO(vermagic, VERMAGIC_STRING);

struct module __this_module
__attribute__((section(".gnu.linkonce.this_module"))) = {
 .name = KBUILD_MODNAME,
 .init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
 .exit = cleanup_module,
#endif
 .arch = MODULE_ARCH_INIT,
};

static const struct modversion_info ____versions[]
__used
__attribute__((section("__versions"))) = {
	{ 0xf723ef3a, "module_layout" },
	{ 0x91033013, "kmalloc_caches" },
	{ 0xc8b57c27, "autoremove_wake_function" },
	{ 0x95e1dce2, "dev_get_by_name" },
	{ 0x47c7b0d2, "cpu_number" },
	{ 0x48eb0c0d, "__init_waitqueue_head" },
	{ 0x141bf93d, "misc_register" },
	{ 0x417b5b28, "netif_rx_ni" },
	{ 0xcfcaad8a, "dev_alloc_skb" },
	{ 0xf97456ea, "_raw_spin_unlock_irqrestore" },
	{ 0xee566a87, "current_task" },
	{ 0x50eedeb8, "printk" },
	{ 0x2f287f0d, "copy_to_user" },
	{ 0xb4390f9a, "mcount" },
	{ 0x16305289, "warn_slowpath_null" },
	{ 0x2e493880, "skb_push" },
	{ 0x846cb106, "skb_pull" },
	{ 0xe1782b27, "init_net" },
	{ 0x4292364c, "schedule" },
	{ 0x62090a07, "kfree_skb" },
	{ 0xdce5ff01, "eth_type_trans" },
	{ 0x861049fb, "kmem_cache_alloc_trace" },
	{ 0x21fb443e, "_raw_spin_lock_irqsave" },
	{ 0x37a0cba, "kfree" },
	{ 0x622fa02a, "prepare_to_wait" },
	{ 0x75bb675a, "finish_wait" },
	{ 0xca0bfc6a, "skb_dequeue" },
	{ 0x1bde3bb7, "dev_queue_xmit" },
	{ 0xda67bc21, "skb_put" },
	{ 0x362ef408, "_copy_from_user" },
	{ 0x98eebddc, "misc_deregister" },
};

static const char __module_depends[]
__used
__attribute__((section(".modinfo"))) =
"depends=";


MODULE_INFO(srcversion, "B64AD9D92677D2DE94A0ACC");
