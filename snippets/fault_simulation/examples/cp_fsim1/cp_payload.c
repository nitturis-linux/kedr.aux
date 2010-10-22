//#include <linux/version.h>
#include <linux/module.h>
#include <linux/init.h>

#include <linux/kernel.h>	/* printk() */
#include <linux/slab.h>		/* kmalloc() */
//#include <linux/fs.h>		/* everything... */
#include <linux/errno.h>	/* error codes */

//#include <asm/uaccess.h>	/* copy_*_user */

#include <cp_controller/controller_common.h>
#include <fault_simulation/fault_simulation.h>

MODULE_AUTHOR("Eugene");
MODULE_LICENSE("GPL");

struct kedr_simulation_point *kmalloc_point = NULL, *kmca_point = NULL;

static inline int kedr_fsim_simulate_kmalloc(size_t size)
{
	return kedr_fsim_simulate(kmalloc_point, &size);
}
/* ================================================================ */
/* Declarations of replacement functions (should be the same as for 
 * the target functions but with a different name.) 
 * */
static void*
repl___kmalloc(size_t size, gfp_t flags);

static void 
repl_kfree(const void* p);

static void*
repl_kmem_cache_alloc(struct kmem_cache* mc, gfp_t flags);

static void 
repl_kmem_cache_free(struct kmem_cache* mc, void* p);
/* ================================================================ */

/* Names and addresses of the functions of interest */
static void* target_func_addrs[] = {
	(void*)&__kmalloc,
	(void*)&kfree,
	(void*)&kmem_cache_alloc,
	(void*)&kmem_cache_free
};

/* Addresses of the replacement functions */
static void* repl_func_addrs[] = {
	(void*)&repl___kmalloc,
	(void*)&repl_kfree,
	(void*)&repl_kmem_cache_alloc,
	(void*)&repl_kmem_cache_free
};

static struct kedr_payload payload = {
	.mod 			= THIS_MODULE,
	.target_func_addrs 	= &target_func_addrs[0],
	.repl_func_addrs 	= &repl_func_addrs[0],
	.num_func_addrs		= ARRAY_SIZE(target_func_addrs)
};
/* ================================================================ */

static void
cfake_cleanup_module(void)
{
	kedr_payload_unregister(&payload);
	kedr_fsim_point_unregister(kmalloc_point);
	kedr_fsim_point_unregister(kmca_point);
	
	printk(KERN_INFO "[cp_fsim1] Cleanup successful\n");
	return;
}

static int __init
cfake_init_module(void)
{
	BUG_ON(	ARRAY_SIZE(target_func_addrs) != 
		ARRAY_SIZE(repl_func_addrs));
	
	kmalloc_point = kedr_fsim_point_register("kmalloc", "size_t");
	if(!kmalloc_point)
	{
		printk(KERN_INFO "Cannot register fault simulation point \"%s\".",
			"kmalloc");
		return 1;
	}
	kmca_point = kedr_fsim_point_register("kmem_cahce_alloc", NULL);
	if(!kmalloc_point)
	{
		printk(KERN_INFO "Cannot register fault simulation point \"%s\".",
			"kmem_cahce_alloc");
		kedr_fsim_point_unregister(kmalloc_point);
		return 1;
	}
	
	printk(KERN_INFO "[cp_fsim1] Initializing\n");
	return kedr_payload_register(&payload);
}

module_init(cfake_init_module);
module_exit(cfake_cleanup_module);
/* ================================================================ */

/* Definitions of replacement functions
 */
static void*
repl___kmalloc(size_t size, gfp_t flags)
{
	void* result = kedr_fsim_simulate_kmalloc(size) ? NULL
					: __kmalloc(size, flags);
	printk(	KERN_INFO "[cp_fsym1] Called: "
		"__kmalloc(%zu, %x), result: %p, in init: %s\n",
		size, 
		(unsigned int)flags,
		result,
		(kedr_target_module_in_init() ? "yes" : "no")
	);
	return result;
}

static void 
repl_kfree(const void* p)
{
	kfree(p);
	printk(	KERN_INFO "[cp_fsym1] Called: "
		"kfree(%p)\n",
		p
	);
	return;
}

static void*
repl_kmem_cache_alloc(struct kmem_cache* mc, gfp_t flags)
{
	void* result = kedr_fsim_simulate(kmca_point, NULL) ? NULL
					: kmem_cache_alloc(mc, flags);
	printk(	KERN_INFO "[cp_fsym1] Called: "
		"kmem_cache_alloc(%p, %x), result: %p\n",
		mc, 
		(unsigned int)flags,
		result
	);
	return result;
}

static void 
repl_kmem_cache_free(struct kmem_cache* mc, void* p)
{
	kmem_cache_free(mc, p);
	printk(	KERN_INFO "[cp_fsym1] Called: "
		"kmem_cache_free(%p, %p)\n",
		mc,
		p
	);
	return;
}
/* ================================================================ */

