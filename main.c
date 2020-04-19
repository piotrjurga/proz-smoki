// problemy:
// - nie wspieramy działania wiecznie, bo kiedyś requests_received się przepełni i będzie klapa
// - requests to powinien być circular buffer a nie tablica dynamiczna
//

//
// CONFIG
//

#if 0
#define RELEASE
#endif

#define NECRO_TAIL_COUNT 3
#define NECRO_BODY_COUNT 3
#define NECRO_HEAD_COUNT 3
#define DESK_COUNT   3
#define DRAGON_COUNT 3

// time in seconds
#define TASK_GENERATION_DELAY 0.1

//
// HEADERS
//

#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <mpi.h>

// dynamic array and hashmap implementation
#define STB_DS_IMPLEMENTATION
#include "stb_ds.h"

typedef int8_t  s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef struct timespec timespec;

inline timespec get_time() {
    timespec temp;
    //clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &temp);
    clock_gettime(CLOCK_MONOTONIC, &temp);
    return temp;
}

timespec time_diff(timespec start, timespec end) {
    timespec temp;
    if ((end.tv_nsec-start.tv_nsec)<0) {
        temp.tv_sec = end.tv_sec-start.tv_sec-1;
        temp.tv_nsec = 1000000000+end.tv_nsec-start.tv_nsec;
    } else {
        temp.tv_sec = end.tv_sec-start.tv_sec;
        temp.tv_nsec = end.tv_nsec-start.tv_nsec;
    }
    return temp;
}

inline bool time_less_than(timespec a, timespec b) {
    timespec res = time_diff(a, b);
    return res.tv_sec < 0 || res.tv_nsec < 0;
}

#ifdef RELEASE
#define mpi_assert(x)
#else
#define mpi_assert(x) do {\
    if (!(x)) {\
        fprintf(stderr, "%s:%d:%s Assertion failed: %s\n", __FILE__, __LINE__, __func__, #x);\
        MPI_Abort(MPI_COMM_WORLD, -1);\
    }\
} while (0)
#endif

#define TASK_GENERATOR   0
#define NECRO_TAIL_BEGIN 1
#define NECRO_TAIL_END   (NECRO_TAIL_BEGIN+NECRO_TAIL_COUNT)
#define NECRO_BODY_BEGIN (NECRO_TAIL_END)
#define NECRO_BODY_END   (NECRO_TAIL_END+NECRO_BODY_COUNT)
#define NECRO_HEAD_BEGIN (NECRO_BODY_END)
#define NECRO_HEAD_END   (NECRO_BODY_END+NECRO_HEAD_COUNT)

#define PROCESS_COUNT NECRO_HEAD_END

typedef enum {
    TaskGenerator,
    NecroTail,
    NecroBody,
    NecroHead,
} ProcessType;

ProcessType get_process_type(int n) {
    mpi_assert(n >= 0 && n < PROCESS_COUNT);

    if      (n == TASK_GENERATOR) return TaskGenerator;
    else if (n < NECRO_TAIL_END)  return NecroTail;
    else if (n < NECRO_BODY_END)  return NecroBody;
    else                          return NecroHead;
    //else if (n < NECRO_HEAD_END)  return NecroHead;
}

typedef enum {
    MsgNewTask,
    MsgRingRequest,
    MsgRingAccept,
    MsgFree,
    MsgToken,
} MessageType;

union __MAX_RING_ARRAY{
    u32 a[NECRO_HEAD_COUNT];
    u32 b[NECRO_TAIL_COUNT];
    u32 c[NECRO_BODY_COUNT];
};

typedef union {
    struct { //MsgToken
        u32 requests_handled;
        u32 current_task[sizeof(union __MAX_RING_ARRAY) / sizeof(u32)];
    } token;
} Message;

//
// CODE
//

void necro_tail_loop(u32 rank) {
    u32 my_index = rank - NECRO_TAIL_BEGIN;
    bool has_token = (rank == NECRO_TAIL_BEGIN);
    u32 requests_received = 0;
    u32 *requests = 0;
    bool pass_token_to_first_free = false;
    u32 last_task_completed[NECRO_TAIL_COUNT] = {};
    u32 current_task[NECRO_TAIL_COUNT] = {};

    for (;;) {
        Message msg = {};
        MPI_Status status = {};
        MPI_Recv(&msg, sizeof(msg), MPI_BYTE, MPI_ANY_SOURCE,
                 MPI_ANY_TAG, MPI_COMM_WORLD, &status);
        switch (status.MPI_TAG) {
            case MsgRingRequest: {
                requests_received++;
                if (has_token) {
                    current_task[my_index] = requests_received;

                    // pass the token to next free process
                    u32 next = my_index;
                    bool found = false;
                    for (u32 i = 1; i < NECRO_TAIL_COUNT; i++) {
                        u32 index = (my_index + i) % NECRO_TAIL_COUNT;
                        if (msg.token.current_task[index] == 0) {
                            next = index;
                            found = true;
                            break;
                        }
                    }
                    if (found) {
                        mpi_assert(next != my_index);
                        u32 next_rank = next + NECRO_TAIL_BEGIN;

                        Message new_token = {};
                        new_token.token.requests_handled = requests_received;
                        memcpy(new_token.token.current_task, current_task,
                               NECRO_TAIL_COUNT*sizeof(u32));
                        MPI_Send(&new_token, sizeof(new_token), MPI_BYTE,
                                 next_rank, MsgToken, MPI_COMM_WORLD);
                    } else {
                        pass_token_to_first_free = true;
                    }

                    MPI_Send(0, 0, MPI_BYTE, status.MPI_SOURCE,
                             MsgRingAccept, MPI_COMM_WORLD);
                } else {
                    // process doesn't have the token
                    arrpush(requests, status.MPI_SOURCE);
                }
            } break;
            case MsgToken: {
                // fix token info
                for (u32 i = 0; i < NECRO_TAIL_COUNT; i++) {
                    // TODO(piotr): implement
                    if (msg.token.current_task[i] == last_task_completed[i]) {
                        msg.token.current_task[i] = 0;
                    }
                }
            } break;
            case MsgFree: {
                if (pass_token_to_first_free) {
                    pass_token_to_first_free = false;
                    // TODO(piotr): pass token
                }
            } break;
        }
    }
}

void necro_body_loop(u32 rank) {
    for (;;) {
        // TODO(piotr):
    }
}

void necro_head_loop(u32 rank) {
    for (;;) {
        // TODO(piotr):
    }
}

void task_generator_loop() {
    for (;;) {
        for (int i = NECRO_HEAD_BEGIN; i < NECRO_HEAD_END; i++) {
            MPI_Send(0, 0, MPI_BYTE, i, MsgNewTask, MPI_COMM_WORLD);
        }
        sleep(TASK_GENERATION_DELAY);
    }
}


int main(int argc, char **argv) {
    int size, rank, len;
    char processor[100];
    MPI_Init(&argc, &argv);

    MPI_Comm_size(MPI_COMM_WORLD, &size);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Get_processor_name(processor, &len);

    if (size != PROCESS_COUNT && rank == 0) {
        printf("Got %d processes, wanted %d\n", PROCESS_COUNT, size);
        mpi_assert(!"Incorrect process count!");
    }

    ProcessType my_type = get_process_type(rank);
    switch (my_type) {
        case TaskGenerator:
            task_generator_loop();
            break;
        case NecroTail:
            necro_tail_loop(rank);
            break;
        case NecroBody:
            necro_body_loop(rank);
            break;
        case NecroHead:
            necro_head_loop(rank);
            break;
    }

    MPI_Finalize();
}
