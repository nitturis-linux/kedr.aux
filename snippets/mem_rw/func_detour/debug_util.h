#ifndef DEBUG_UTIL_H_1104_INCLUDED
#define DEBUG_UTIL_H_1104_INCLUDED

/* debug_util.h - utility functions for output of debug data, etc. 
 * 
 * The functions listed below that return void print an error message to 
 * the system log if an error occurs. */

/* Initialize debugging facilities. 
 * The function creates files in debugfs if necessary, etc.
 * Returns 0 on success, negative error code on failure.
 *
 * This function should usually be called from the module's initialization
 * function. */
int 
debug_util_init(void);

/* Finalize output subsystem.
 * The function removes files if debug_util_init() created some, etc.
 *
 * This function should usually be called from the module's cleanup
 * function. */
void
debug_util_fini(void);

/* Clears the output data. For example, it may clear the contents of the 
 * files that stored information for the previous analysis session for 
 * the target module.
 *
 * This function should usually be called from on_target_load() handler
 * or the like to clear the old data. */
void
debug_util_clear(void);

/* Output a string pointed to by 's' to a debug 'stream' (usually, a file 
 * in debugfs).
 *
 * This function cannot be used in atomic context. */
void
debug_util_print_string(const char *s);

/* Output a sequence of bytes of length 'count' as is to a debug 'stream' 
 * 
 * The caller must ensure that the memory area pointed to by 'bytes' is at 
 * least 'count' bytes is size.
 *
 * This function cannot be used in atomic context. */
void
debug_util_print_raw_bytes(const void *bytes, unsigned int count);

/* Output the given u64 value using the specified format string 'fmt'. 
 * The format string must contain "%llu", "%llx" or the like. 
 * 
 * This function cannot be used in atomic context. */
void
debug_util_print_u64(u64 data, const char *fmt);

/* Output a sequence of bytes of length 'count' to a debug 'stream'. 
 * Each byte is output as a hex number, the consecutive bytes are separated
 * by spaces: 0D FA 7E and so forth.
 * 
 * The caller must ensure that the memory area pointed to by 'bytes' is at 
 * least 'count' bytes is size.
 *
 * This function cannot be used in atomic context. */
void
debug_util_print_hex_bytes(const void *bytes, unsigned int count);

#endif // DEBUG_UTIL_H_1104_INCLUDED
