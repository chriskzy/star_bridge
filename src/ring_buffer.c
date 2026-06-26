#include "ring_buffer.h"
#include <stddef.h>

void ring_append(char *buffer, size_t capacity, size_t *head, size_t *tail,
                 const char *data, size_t len) {
    if (!buffer || !head || !tail || !data || capacity == 0) return;
    for (size_t i = 0; i < len; i++) {
        buffer[*head % capacity] = data[i];
        (*head)++;
        if (*head - *tail >= capacity) {
            *tail = *head - capacity + 1;
        }
    }
}

size_t ring_read(char *buffer, size_t capacity, size_t *head, size_t *tail,
                 char *dest, size_t max_len) {
    if (!buffer || !head || !tail || !dest || max_len == 0) return 0;
    size_t available = *head - *tail;
    if (available >= max_len) available = max_len - 1;
    for (size_t i = 0; i < available; i++) {
        dest[i] = buffer[(*tail + i) % capacity];
    }
    dest[available] = '\0';
    *tail += available;
    return available;
}

size_t ring_available(size_t head, size_t tail) {
    return head - tail;
}

void ring_reset(size_t *head, size_t *tail) {
    if (head) *head = 0;
    if (tail) *tail = 0;
}
