/* ========================================================================
 * Copyright (C) 2010, Institute for System Programming 
 *                     of the Russian Academy of Sciences (ISPRAS)
 * Authors: 
 *      Eugene A. Shatokhin <spectre@ispras.ru>
 *      Andrey V. Tsyvarev  <tsyvarev@ispras.ru>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 ======================================================================== */

#include <linux/version.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/kernel.h>	/* printk() */
#include <linux/slab.h>		/* kmalloc() */
#include <linux/fs.h>		/* everything... */
#include <linux/errno.h>	/* error codes */
#include <linux/cdev.h>

#include <linux/mutex.h>

#include <asm/uaccess.h>	/* copy_*_user */

#include <linux/stacktrace.h>

#include "cfake.h"

MODULE_AUTHOR("Eugene");
MODULE_LICENSE("GPL");

/* How many stack entries (above the call point) we are interested in. 
 * Must be less than or equal to KEDR_MAX_FRAMES (see below).
 */
#define TEST_STACK_ENTRIES 5

/* parameters */
int cfake_major = CFAKE_MAJOR;
int cfake_minor = 0;
int cfake_ndevices = CFAKE_NDEVICES;
unsigned long cfake_buffer_size = CFAKE_BUFFER_SIZE;
unsigned long cfake_block_size = CFAKE_BLOCK_SIZE;

module_param(cfake_major, int, S_IRUGO);
module_param(cfake_minor, int, S_IRUGO);
module_param(cfake_ndevices, int, S_IRUGO);
module_param(cfake_buffer_size, ulong, S_IRUGO);
module_param(cfake_block_size, ulong, S_IRUGO);

/* ================================================================ */
/* Main operations - declarations */

int 
cfake_open(struct inode *inode, struct file *filp);

int 
cfake_release(struct inode *inode, struct file *filp);

ssize_t 
cfake_read(struct file *filp, char __user *buf, size_t count,
	loff_t *f_pos);

ssize_t 
cfake_write(struct file *filp, const char __user *buf, size_t count,
	loff_t *f_pos);

int 
cfake_ioctl(struct inode *inode, struct file *filp,
	unsigned int cmd, unsigned long arg);

loff_t 
cfake_llseek(struct file *filp, loff_t off, int whence);

/* ================================================================ */

struct cfake_dev *cfake_devices;	/* created in cfake_init_module() */

struct file_operations cfake_fops = {
	.owner =    THIS_MODULE,
	.llseek =   cfake_llseek,
	.read =     cfake_read,
	.write =    cfake_write,
	.ioctl =    cfake_ioctl,
	.open =     cfake_open,
	.release =  cfake_release,
};
/* ================================================================ */
/* Stack-related helpers */

/* Maximum number of stack frames 'up' from the call point that can be 
 * saved.
 */
#define KEDR_MAX_FRAMES 16

/* Maxiumum number of stack frames 'below' the call point. These are 
 * usually from the implementation of save_stack_trace() + one frame for
 * kedr_save_stack_impl().
 */
#define KEDR_LOWER_FRAMES 6

/* Maximum number of frames (entries) to store internally. 
 * Technically, it is (KEDR_MAX_FRAMES + KEDR_LOWER_FRAMES) rounded up to
 * the next multiple of 16.
 * The additional entries may help handle the case when there are more than
 * KEDR_LOWER_FRAMES lower frames.
 * 
 * [NB] To round up a nonnegative number x to a multiple of N, use
 * (x + N - 1) & ~(N - 1) 
 */
#define KEDR_NUM_FRAMES_INTERNAL \
    ((KEDR_MAX_FRAMES + KEDR_LOWER_FRAMES + 15) & ~15)

/* kedr_save_stack_trace_impl() saves up to 'max_entries' stack trace
 * entries in the 'entries' array provided by the caller. 
 * After the call, *nr_entries will contain the number of entries actually
 * saved. 
 *
 * The difference from save_stack_trace() is that only the entries from 
 * above the call point will be saved. That is, the first entry will
 * correspond to the caller of the function that called 
 * kedr_save_stack_trace_impl(), etc. We are not often interested in the 
 * entries corresponding to the implementation of save_stack_trace(),
 * that is why the 'lower' entries will be omitted.
 *
 * This function is intended to be used in the replacement functions, so
 * it is not that important that it will not store the entry corresponding 
 * to its direct caller.
 * The question that a function "asks" by calling 
 * kedr_save_stack_trace_impl() is "Who called me?"
 *
 * 'max_entries' should not exceed KEDR_MAX_FRAMES.
 *
 * 'entries' should have space for at least 'max_entries' elements.
 *
 * 'first_entry' is the first stack entry we are interested in. It is 
 * usually obtained with __builtin_return_address(0). 
 * [NB] If the results of save_stack_trace() are not reliable (e.g. if that
 * function is a no-op), 'entries[0]' will contain the value of 
 * 'first_entry' and '*nr_entries' will be 1.
 */
void
kedr_save_stack_trace_impl(unsigned long *entries, unsigned int max_entries,
    unsigned int *nr_entries,
    unsigned long first_entry)
{
    unsigned int i = 0;
    int found = 0;
    unsigned long stack_entries[KEDR_NUM_FRAMES_INTERNAL];
    struct stack_trace trace = {
		.nr_entries = 0,
		.entries = &stack_entries[0],
        
        /* Request as many entries as we can. */
		.max_entries = KEDR_NUM_FRAMES_INTERNAL,
            
        /* We need all frames, we'll do filtering ourselves. */
		.skip = 0
	}; 
    
    BUG_ON(entries == NULL);
    BUG_ON(nr_entries == NULL);
    BUG_ON(max_entries > KEDR_MAX_FRAMES);
    
    if (max_entries == 0) {
        *nr_entries = 0;
        return;
    }
    
    save_stack_trace(&trace);
    
    /* At least one entry will be stored. */
    *nr_entries = 1;
    entries[0] = first_entry;
    
    for (i = 0; i < trace.nr_entries; ++i)
    {
        if (*nr_entries >= max_entries) break;

        if (found) {
            entries[*nr_entries] = stack_entries[i];
            ++(*nr_entries);
        } else if (stack_entries[i] == first_entry) 
        {
            found = 1;
        }
    }
    
    return;    
}

/* A helper macro to obtain the call stack (this is a macro because it
 * allows using __builtin_return_address(0) to obtain the first stack 
 * entry).
 */
#define kedr_save_stack_trace(entries_, max_entries_, nr_entries_) \
    kedr_save_stack_trace_impl(entries_, max_entries_, \
        nr_entries_, \
        (unsigned long)__builtin_return_address(0))

/* ================================================================ */

/* Set up the char_dev structure for the device. */
void cfake_setup_cdevice(struct cfake_dev *dev, int index)
{
	int err;
    
    unsigned int i = 0;
    unsigned int nr_entries = 0;
    unsigned long stack_entries[TEST_STACK_ENTRIES];

	int devno = MKDEV(cfake_major, cfake_minor + index);
    
	cdev_init(&dev->cdevice, &cfake_fops);
	dev->cdevice.owner = THIS_MODULE;
	dev->cdevice.ops = &cfake_fops;
	
	err = cdev_add(&dev->cdevice, devno, 1);
	if (err)
	{
		printk(KERN_NOTICE "[cr_target] Error %d while trying to add cfake%d",
			err, index);
	}
	else
	{
		dev->dev_added = 1;
	}
    
    kedr_save_stack_trace(&stack_entries[0], TEST_STACK_ENTRIES, 
        &nr_entries);
    printk(KERN_INFO 
        "[cr_target] cfake_setup_cdevice(), stack entries: %u\n", 
        nr_entries
    );
    for (i = 0; i < nr_entries; ++i)
    {
        /* Print the address similar to how dump_stack() does it.
         * "%pS" is used to resolve the address automatically.
         */
        printk(KERN_INFO "[cr_target] stack entry #%u: [<%p>] %pS\n", 
            i, (void *)(stack_entries[i]), (void *)(stack_entries[i])
        );
    }
    
	return;
}

/* ================================================================ */
static void
cfake_cleanup_module(void)
{
	int i;
	dev_t devno = MKDEV(cfake_major, cfake_minor);
    
    unsigned int j = 0;
    unsigned int nr_entries = 0;
    unsigned long stack_entries[TEST_STACK_ENTRIES];
	
	printk(KERN_INFO "[cr_target] == Cleaning up ==\n");
	
	/* Get rid of our char dev entries */
	if (cfake_devices) {
		for (i = 0; i < cfake_ndevices; ++i) {
			kfree(cfake_devices[i].data);
			if (cfake_devices[i].dev_added)
			{
				cdev_del(&cfake_devices[i].cdevice);
			}
		}
		kfree(cfake_devices);
	}

	/* cleanup_module is never called if registering failed */
	unregister_chrdev_region(devno, cfake_ndevices);
    
    kedr_save_stack_trace(&stack_entries[0], TEST_STACK_ENTRIES, 
        &nr_entries);
    printk(KERN_INFO 
        "[cr_target] cfake_cleanup_module(), stack entries: %u\n", 
        nr_entries
    );
    for (j = 0; j < nr_entries; ++j)
    {
        /* Print the address similar to how dump_stack() does it.
         * "%pS" is used to resolve the address automatically.
         */
        printk(KERN_INFO "[cr_target] stack entry #%u: [<%p>] %pS\n", 
            j, (void *)(stack_entries[j]), (void *)(stack_entries[j])
        );
    }
    
	return;
}

static int __init
cfake_init_module(void)
{
	int result = 0;
	int i;
	dev_t dev = 0;
	
	if (cfake_ndevices <= 0)
	{
		printk(KERN_WARNING "[cr_target] Invalid value of cfake_ndevices: %d\n", 
			cfake_ndevices);
		result = -EINVAL;
		return result;
	}
	
	printk(KERN_INFO "[cr_target] == Initializing ==\n");
	
	/* Get a range of minor numbers to work with, asking for a dynamic
	major number unless directed otherwise at load time.
	*/
	if (cfake_major > 0) {
		dev = MKDEV(cfake_major, cfake_minor);
		result = register_chrdev_region(dev, cfake_ndevices, "cfake");
	} else {
		result = alloc_chrdev_region(&dev, cfake_minor, cfake_ndevices,
				"cfake");
		cfake_major = MAJOR(dev);
	}
	if (result < 0) {
		printk(KERN_WARNING "[cr_target] can't get major number %d\n", cfake_major);
		return result;
	}
	
	/* Allocate the array of devices */
	cfake_devices = (struct cfake_dev*)kmalloc(
		cfake_ndevices * sizeof(struct cfake_dev), 
		GFP_KERNEL);
	if (cfake_devices == NULL) {
		result = -ENOMEM;
		goto fail;
	}
	memset(cfake_devices, 0, cfake_ndevices * sizeof(struct cfake_dev));
	
	/* Initialize each device. */
	for (i = 0; i < cfake_ndevices; ++i) {
		cfake_devices[i].buffer_size = cfake_buffer_size;
		cfake_devices[i].block_size = cfake_block_size;
		cfake_devices[i].dev_added = 0;
		mutex_init(&cfake_devices[i].cfake_mutex);
		
		/* memory is to be allocated in open() */
		cfake_devices[i].data = NULL; 
		
		cfake_setup_cdevice(&cfake_devices[i], i);
	}
	
	return 0; /* success */

fail:
	cfake_cleanup_module();
	return result;
}

static void __exit
cfake_exit_module(void)
{
	cfake_cleanup_module();
	return;
}

module_init(cfake_init_module);
module_exit(cfake_exit_module);
/* ================================================================ */

int 
cfake_open(struct inode *inode, struct file *filp)
{
    unsigned int i = 0;
    unsigned int nr_entries = 0;
    unsigned long stack_entries[TEST_STACK_ENTRIES];
    
	unsigned int mj = imajor(inode);
	unsigned int mn = iminor(inode);
	
	struct cfake_dev *dev = NULL;

   	if (mj != cfake_major || mn < cfake_minor || 
		mn >= cfake_minor + cfake_ndevices)
	{
		printk(KERN_WARNING "[cr_target] No device found with MJ=%d and MN=%d\n", 
			mj, mn);
		return -ENODEV; /* No such device */
	}
	
	kedr_save_stack_trace(&stack_entries[0], TEST_STACK_ENTRIES, 
        &nr_entries);
    printk(KERN_INFO "[cr_target] open(), stack entries: %u\n", nr_entries);
    for (i = 0; i < nr_entries; ++i)
    {
        /* Print the address similar to how dump_stack() does it.
         * "%pS" is used to resolve the address automatically.
         */
        printk(KERN_INFO "[cr_target] stack entry #%u: [<%p>] %pS\n", 
            i, (void *)(stack_entries[i]), (void *)(stack_entries[i])
        );
    }
	
	/* store a pointer to struct cfake_dev here for other methods */
	dev = &cfake_devices[mn - cfake_minor];
	filp->private_data = dev; 
	
	if (inode->i_cdev != &dev->cdevice)
	{
		printk(KERN_WARNING "[cr_target] open: internal error\n");
		return -ENODEV; /* No such device */
	}
	
	/* if opened the 1st time, allocate the buffer */
	if (dev->data == NULL)
	{
		dev->data = (unsigned char*)kmalloc(
			dev->buffer_size, 
			GFP_KERNEL);
		if (dev->data == NULL)
		{
			printk(KERN_WARNING "[cr_target] open: out of memory\n");
			return -ENOMEM;
		}
		memset(dev->data, 0, dev->buffer_size);
	}
	return 0;
}

int 
cfake_release(struct inode *inode, struct file *filp)
{
	return 0;
}

ssize_t 
cfake_read(struct file *filp, char __user *buf, size_t count, 
	loff_t *f_pos)
{
	struct cfake_dev *dev = (struct cfake_dev *)filp->private_data;
	ssize_t retval = 0;
	
	if (mutex_lock_killable(&dev->cfake_mutex))
	{
		return -EINTR;
	}
	
	if (*f_pos >= dev->buffer_size) /* EOF */
	{
		goto out;
	}
	
	if (*f_pos + count > dev->buffer_size)
	{
		count = dev->buffer_size - *f_pos;
	}
	
	if (count > dev->block_size)
	{
		count = dev->block_size;
	}
	
	if (copy_to_user(buf, &(dev->data[*f_pos]), count) != 0)
	{
		retval = -EFAULT;
		goto out;
	}
	
	*f_pos += count;
	retval = count;
	
out:
  	mutex_unlock(&dev->cfake_mutex);
	return retval;
}
                
ssize_t 
cfake_write(struct file *filp, const char __user *buf, size_t count, 
	loff_t *f_pos)
{
	struct cfake_dev *dev = (struct cfake_dev *)filp->private_data;
	ssize_t retval = 0;
	
	if (mutex_lock_killable(&dev->cfake_mutex))
	{
		return -EINTR;
	}
	
	if (*f_pos >= dev->buffer_size) /* EOF */
	{
		goto out;
	}
	
	if (*f_pos + count > dev->buffer_size)
	{
		count = dev->buffer_size - *f_pos;
	}
	
	if (count > dev->block_size)
	{
		count = dev->block_size;
	}
	
	if (copy_from_user(&(dev->data[*f_pos]), buf, count) != 0)
	{
		retval = -EFAULT;
		goto out;
	}
	
	*f_pos += count;
	retval = count;
	
out:
  	mutex_unlock(&dev->cfake_mutex);
	return retval;
}

int 
cfake_ioctl(struct inode *inode, struct file *filp,
                 unsigned int cmd, unsigned long arg)
{
	int err = 0;
	int retval = 0;
	int fill_char;
	unsigned int block_size = 0;
	struct cfake_dev *dev = (struct cfake_dev *)filp->private_data;

	/*
	 * extract the type and number bitfields, and don't decode
	 * wrong cmds: return ENOTTY (inappropriate ioctl) before access_ok()
	 */
	if (_IOC_TYPE(cmd) != CFAKE_IOCTL_MAGIC) return -ENOTTY;
	if (_IOC_NR(cmd) > CFAKE_IOCTL_NCODES) return -ENOTTY;

	if (_IOC_DIR(cmd) & _IOC_READ)
	{
		err = !access_ok(VERIFY_WRITE, (void __user *)arg, _IOC_SIZE(cmd));
	}
	else if (_IOC_DIR(cmd) & _IOC_WRITE)
	{
		err =  !access_ok(VERIFY_READ, (void __user *)arg, _IOC_SIZE(cmd));
	}
	if (err) 
	{
		return -EFAULT;
	}
	
	/* Begin critical section */
	if (mutex_lock_killable(&dev->cfake_mutex))
	{
		return -EINTR;
	}
	
	switch(cmd) {
	case CFAKE_IOCTL_RESET:
		memset(dev->data, 0, dev->buffer_size);
		break;
	
	case CFAKE_IOCTL_FILL:
		retval = get_user(fill_char, (int __user *)arg);
		if (retval == 0) /* success */
		{
			memset(dev->data, fill_char, dev->buffer_size);
		}
		break;
	
	case CFAKE_IOCTL_LFIRM:
		/* Assume that only an administrator can load the 'firmware' */ 
		if (!capable(CAP_SYS_ADMIN))
		{
			retval = -EPERM;
			break;
		}
		
		memset(dev->data, 0, dev->buffer_size);
		strcpy((char*)(dev->data), "Hello, hacker!\n");
		break;
	
	case CFAKE_IOCTL_RBUFSIZE:
		retval = put_user(dev->buffer_size, (unsigned long __user *)arg);
		break;
	
	case CFAKE_IOCTL_SBLKSIZE:
		retval = get_user(block_size, (unsigned long __user *)arg);
		if (retval != 0) break;
		
		retval = put_user(dev->block_size, (unsigned long __user *)arg);
		if (retval != 0) break;
		
		dev->block_size = block_size;
		break;
	
	default:  /* redundant, as 'cmd' was checked against CFAKE_IOCTL_NCODES */
		retval = -ENOTTY;
	}
	
	/* End critical section */
	mutex_unlock(&dev->cfake_mutex);
	return retval;
}

loff_t 
cfake_llseek(struct file *filp, loff_t off, int whence)
{
	struct cfake_dev *dev = (struct cfake_dev *)filp->private_data;
	loff_t newpos = 0;
	
	switch(whence) {
	  case 0: /* SEEK_SET */
		newpos = off;
		break;

	  case 1: /* SEEK_CUR */
		newpos = filp->f_pos + off;
		break;

	  case 2: /* SEEK_END */
		newpos = dev->buffer_size + off;
		break;

	  default: /* can't happen */
		return -EINVAL;
	}
	if (newpos < 0) return -EINVAL;
	filp->f_pos = newpos;
	return newpos;
}

/* ================================================================ */
