/* debug_util.c - utility functions for output of debug data, etc. */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/debugfs.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <asm/uaccess.h>

#include "debug_util.h"

/* ====================================================================== */
/* A directory for output files in debugfs. */
struct dentry *debug_dir_dentry = NULL;
const char *debug_dir_name = "kedr_sample";

/* The file in debugfs. */
struct dentry *debug_out_file = NULL;
const char *debug_out_name = "output";

/* ====================================================================== */
/* A buffer that accumulates strings sent to it by debug_util_print_*().
 * The buffer grows automatically when necessary.
 * The operations with such buffer should be performed with the 
 * corresponding mutex locked (see 'lock' field in the structure). 
 *
 * The caller must also ensure that no other operations with a buffer can
 * occur during the creation and descruction of the buffer.
 */
struct debug_output_buffer
{
	/* the buffer itself */
	char *buf; 

	/* the size of the buffer */
	size_t size; 

	/* length of the data stored (excluding the terminating '\0')*/
	size_t data_len; 

	/* the mutex to guard access to the buffer */
	struct mutex *lock; 
};

/* Default size of the buffer, in bytes. */
#define DEBUG_OUTPUT_BUFFER_SIZE 1000

/* A mutex to guard access to the buffer. */
DEFINE_MUTEX(output_buffer_mutex);

struct debug_output_buffer output_buffer = {
	.buf = NULL,
	.size = 0,
	.data_len = 0,
	.lock = &output_buffer_mutex
};

/* ====================================================================== */
/* Initialize the buffer: allocate memory, etc.
 * Returns 0 on success, a negative error code on failure. */
static int
output_buffer_init(struct debug_output_buffer *ob)
{
	BUG_ON(ob == NULL);

	ob->buf = (char *)kzalloc(DEBUG_OUTPUT_BUFFER_SIZE, GFP_KERNEL);
	if (ob->buf == NULL)
		return -ENOMEM;

	ob->buf[0] = 0;
	ob->size = DEBUG_OUTPUT_BUFFER_SIZE;
	ob->data_len = 0;

	return 0;
}

/* Destroys the buffer: releases the memory pointed to by 'ob->buf', etc. */
static void
output_buffer_destroy(struct debug_output_buffer *ob)
{
	BUG_ON(ob == NULL);

	ob->data_len = 0;
	ob->size = 0;
	kfree(ob->buf);
	return;
}

/* Enlarge the buffer to make it at least 'new_size' bytes in size.
 * If 'new_size' is less than or equal to 'ob->size', the function does 
 * nothing.
 * If there is not enough memory, the function outputs an error to 
 * the system log, leaves the buffer intact and returns -ENOMEM.
 * 0 is returned in case of success. */
static int
output_buffer_resize(struct debug_output_buffer *ob, size_t new_size)
{
	size_t size;
	void *p;
	BUG_ON(ob == NULL);

	if (ob->size >= new_size)
		return 0;
	
	/* Allocate memory in the multiples of the default size. */
	size = (new_size / DEBUG_OUTPUT_BUFFER_SIZE + 1) * 
		DEBUG_OUTPUT_BUFFER_SIZE;
	p = krealloc(ob->buf, size, GFP_KERNEL | __GFP_ZERO);
	if (p == NULL) {
	/* [NB] If krealloc fails to allocate memory, it should leave the 
	 * buffer intact, so its previous contents could still be used. */
		pr_err("[sample] output_buffer_resize: "
	"not enough memory to resize the output buffer to %zu bytes\n",
			size);
		return -ENOMEM;
	}
    
	ob->buf = p;
	ob->size = size;

	return 0;
}

/* Appends the specified byte sequence to the buffer, enlarging the latter 
 * if necessary. */
static void
output_buffer_append_bytes(struct debug_output_buffer *ob, 
	const void *bytes, unsigned int count)
{
	int ret;

	BUG_ON(ob == NULL);
	BUG_ON(ob->buf[ob->data_len] != 0);
	BUG_ON(bytes == NULL);
	
	if (count == 0)
		return; /* nothing to do */

	/* Make sure the buffer is large enough. 
	 * (+1) is just in case we are to output a string and count is
	 * the length of the string exluding the terminating '\0'.
	 * The terminating null byte will be there automatically, see
	 * how the buffer is allocated and resized.
	 * Note also that the buffer is not going to shrink */
	ret = output_buffer_resize(ob, ob->data_len + count + 1);
	if (ret != 0)
		return; /* the error has already been reported to the log */

	memcpy(&(ob->buf[ob->data_len]), bytes, count);
	ob->data_len += count;
	return;
}

/* Appends the specified string to the buffer, enlarging the latter 
 * if necessary. */
static void
output_buffer_append_string(struct debug_output_buffer *ob, const char *s)
{
	size_t len = strlen(s);
	if (len == 0)
		return; /* nothing to do */
	
	output_buffer_append_bytes(ob, s, (unsigned int)len);
	/* The terminating \0 is already in place because the buffer is
	 * filled with 0s on allocation and when it is resized. It never 
	 * shrinks, so the sequence of bytes in it always ends with \0. */
	
	return;
}

/* ====================================================================== */
/* A convenience macro to define variable of type struct file_operations
 * for a read only file in debugfs associated with the specified output
 * buffer.
 * 
 * __fops - the name of the variable
 * __ob - pointer to the output buffer (struct debug_output_buffer *)
 */
#define DEBUG_UTIL_DEFINE_FOPS_RO(__fops, __ob)                         \
static int __fops ## _open(struct inode *inode, struct file *filp)      \
{                                                                       \
        BUILD_BUG_ON(sizeof(*(__ob)) !=                                 \
                sizeof(struct debug_output_buffer));                    \
        filp->private_data = (void *)(__ob);                            \
        return nonseekable_open(inode, filp);                           \
}                                                                       \
static const struct file_operations __fops = {                          \
        .owner      = THIS_MODULE,                                      \
        .open       = __fops ## _open,                                  \
        .release    = debug_release_common,                             \
        .read       = debug_read_common,                                \
};

/* Helpers for file operations common to all read-only files in this 
 * example. */
static int 
debug_release_common(struct inode *inode, struct file *filp)
{
	filp->private_data = NULL;
	/* nothing more to do: open() did not allocate any resources */
	return 0;
}

static ssize_t 
debug_read_common(struct file *filp, char __user *buf, size_t count,
	loff_t *f_pos)
{
	ssize_t ret = 0;
	size_t data_len;
	loff_t pos = *f_pos;
	struct debug_output_buffer *ob = 
		(struct debug_output_buffer *)filp->private_data;

	if (ob == NULL) 
		return -EINVAL;

	if (mutex_lock_killable(ob->lock) != 0)
	{
		pr_warning("[sample] debug_read_common: "
			"got a signal while trying to acquire a mutex.\n");
		return -EINTR;
	}

	data_len = ob->data_len;

	/* Reading outside of the data buffer is not allowed */
	if ((pos < 0) || (pos > data_len)) {
		ret = -EINVAL;
		goto out;
	}

	/* EOF reached or 0 bytes requested */
	if ((count == 0) || (pos == data_len)) {
		ret = 0; 
		goto out;
	}

	if (pos + count > data_len) 
		count = data_len - pos;
	if (copy_to_user(buf, &(ob->buf[pos]), count) != 0) {
		ret = -EFAULT;
		goto out;
	}

	mutex_unlock(ob->lock);

	*f_pos += count;
	return count;

out:
	mutex_unlock(ob->lock);
	return ret;
}

/* Definition of file_operations structure for the file in debugfs. */
DEBUG_UTIL_DEFINE_FOPS_RO(fops_output_ro, &output_buffer);

/* ====================================================================== */
int 
debug_util_init(void)
{
	int ret = 0;

	ret = output_buffer_init(&output_buffer);
	if (ret != 0) {
		pr_err("[sample] "
			"failed to create the output buffer\n");
		goto fail;
	}

	debug_dir_dentry = debugfs_create_dir(debug_dir_name, NULL);
	if (IS_ERR(debug_dir_dentry)) {
		pr_err("[sample] debugfs is not supported\n");
		debug_dir_dentry = NULL;
		ret = -ENODEV;
		goto fail_ob;
	}

	if (debug_dir_dentry == NULL) {
		pr_err("[sample] "
			"failed to create a directory in debugfs\n");
		ret = -EINVAL;
		goto fail_ob;
	}
	
	debug_out_file = debugfs_create_file(debug_out_name, S_IRUGO,
		debug_dir_dentry, NULL, &fops_output_ro);
	if (debug_out_file == NULL) {
		pr_err("[sample] "
			"failed to create output file in debugfs\n");
		ret = -EINVAL;
		goto fail_files;
	}

	return 0;

fail_files:
	debugfs_remove(debug_dir_dentry);
fail_ob:
	output_buffer_destroy(&output_buffer);
fail:
	return ret;
}

void
debug_util_fini(void)
{
	if (debug_out_file != NULL)
		debugfs_remove(debug_out_file);
	if (debug_dir_dentry != NULL)
		debugfs_remove(debug_dir_dentry);
	output_buffer_destroy(&output_buffer);
}

void
debug_util_clear(void)
{
	/* Clear the data without actually releasing memory. 
	 * No need for locking as the caller ensures that no output may 
	 * interfere. */
	memset(output_buffer.buf, 0, output_buffer.size);
	output_buffer.data_len = 0;
}

void
debug_util_print_string(const char *s)
{
	BUG_ON(output_buffer.buf == NULL); 
    
	if (mutex_lock_killable(output_buffer.lock) != 0)
	{
		pr_warning("[sample] debug_util_print_string: "
			"got a signal while trying to acquire a mutex.\n");
		return;
	}
	output_buffer_append_string(&output_buffer, s);
	mutex_unlock(output_buffer.lock);
	return;
}

void
debug_util_print_raw_bytes(const void *bytes, unsigned int count)
{
	BUG_ON(output_buffer.buf == NULL);
	BUG_ON(bytes == NULL);
    
	if (mutex_lock_killable(output_buffer.lock) != 0)
	{
		pr_warning("[sample] debug_util_print_raw_bytes: "
			"got a signal while trying to acquire a mutex.\n");
		return;
	}
	output_buffer_append_bytes(&output_buffer, bytes, count);
	mutex_unlock(output_buffer.lock);
	return;
}

void
debug_util_print_u64(u64 data, const char *fmt)
{
	char one_char[1]; /* for the 1st call to snprintf */
	char *buf = NULL;
	int len;

	BUG_ON(fmt == NULL);

	len = snprintf(&one_char[0], 1, fmt, data);
	buf = (char *)kzalloc(len + 1, GFP_KERNEL);
	if (buf == NULL) {
		pr_err("[sample] debug_util_print_u64: "
		"not enough memory to prepare a message of size %d\n",
			len);
		return;
	}
	snprintf(buf, len + 1, fmt, data);
	debug_util_print_string(buf);
	kfree(buf);
	return;
}

#define NUM_CHARS_HEX_BYTE 2
void
debug_util_print_hex_bytes(const void *bytes, unsigned int count)
{
	const char *fmt = "%.2hhX";
	const char *separator = " "; 
	static char buf[NUM_CHARS_HEX_BYTE + 1];
	unsigned int i;
	
	BUG_ON(output_buffer.buf == NULL);
	BUG_ON(bytes == NULL);
		
	if (count == 0)
		return; 
	
	if (mutex_lock_killable(output_buffer.lock) != 0)
	{
		pr_warning("[sample] debug_util_print_hex_bytes: "
			"got a signal while trying to acquire a mutex.\n");
		return;
	}
	
	snprintf(buf, NUM_CHARS_HEX_BYTE + 1, fmt, 
		*((u8 *)bytes));
	output_buffer_append_bytes(&output_buffer, buf, NUM_CHARS_HEX_BYTE);
	
	for (i = 1; i < count; ++i) {
		output_buffer_append_bytes(&output_buffer, separator, 1);
		
		snprintf(buf, NUM_CHARS_HEX_BYTE + 1, fmt, 
			*((u8 *)bytes + i));
		output_buffer_append_bytes(&output_buffer, buf, 
			NUM_CHARS_HEX_BYTE);
	}
	mutex_unlock(output_buffer.lock);
	return;
}

/* ====================================================================== */
