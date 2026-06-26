#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Ring buffer (circular buffer) with overwrite-on-overflow semantics.
 *
 * Capacity is fixed at creation time.  The buffer tracks a head pointer
 * (next write position) and a tail pointer (next read position).  When
 * the buffer is full, the oldest element is overwritten and tail is
 * advanced by one (overwrite semantics).
 *
 * All operations use modular indexing internally so there is no
 * wraparound cost beyond the modulo operation.
 */

/* Append len bytes of data to the ring buffer.
 * If the buffer becomes full, the oldest bytes are overwritten and
 * the tail pointer advances (overwrite semantics).
 * All pointers must be non-NULL and capacity > 0, otherwise this is a no-op.
 */
void ring_append(char *buffer, size_t capacity, size_t *head, size_t *tail,
                 const char *data, size_t len);

/* Read up to max_len-1 bytes from the ring buffer into dest.
 * Returns the number of bytes copied (0 if empty).
 * dest is always null-terminated on success (max_len > 0).
 * All pointers must be non-NULL and max_len > 0, otherwise returns 0.
 */
size_t ring_read(char *buffer, size_t capacity, size_t *head, size_t *tail,
                 char *dest, size_t max_len);

/* Return the number of bytes available for reading. */
size_t ring_available(size_t head, size_t tail);

/* Reset head and tail to zero (discard all data). */
void ring_reset(size_t *head, size_t *tail);

#ifdef __cplusplus
}
#endif

#endif /* RING_BUFFER_H */
