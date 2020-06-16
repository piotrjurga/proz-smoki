// problemy:
// - nie wspieramy działania wiecznie, kiedyś requests_received się przepełni
// - requests to powinien być circular buffer a nie tablica dynamiczna,
//   przez to kopiujemy całą tablicę częściej niż trzeba

//
// CONFIG
//

#if 0
#define RELEASE
#endif

#define NECRO_TAIL_COUNT 2
#define NECRO_BODY_COUNT 3
#define NECRO_HEAD_COUNT 4
#define DESK_COUNT   6
#define DRAGON_COUNT 3

// time in seconds
#define TASK_GENERATION_DELAY 0.01

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

// because C is underspecified, we want types with guaranteed size
typedef int8_t  s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#ifdef RELEASE
#define mpi_assert(x)
#define log(...)
#else
#define mpi_assert(x) do {\
    if (!(x)) {\
        fprintf(stderr, "%s:%d:%s Assertion failed: %s\n", __FILE__, __LINE__, __func__, #x);\
        MPI_Abort(MPI_COMM_WORLD, -1);\
    }\
} while (0)
#define log(format, ...) printf("%s(%u):\t" format "\n", get_process_type_string(rank), rank, ##__VA_ARGS__)
#endif

#define TASK_GENERATOR   0
#define NECRO_TAIL_BEGIN 1
#define NECRO_TAIL_END   (NECRO_TAIL_BEGIN+NECRO_TAIL_COUNT)
#define NECRO_BODY_BEGIN (NECRO_TAIL_END)
#define NECRO_BODY_END   (NECRO_TAIL_END+NECRO_BODY_COUNT)
#define NECRO_HEAD_BEGIN (NECRO_BODY_END)
#define NECRO_HEAD_END   (NECRO_BODY_END+NECRO_HEAD_COUNT)
#define DEBUG_PRINTER    (NECRO_HEAD_END)

#ifdef RELEASE
#define PROCESS_COUNT NECRO_HEAD_END
#else
#define PROCESS_COUNT (DEBUG_PRINTER+1)
#endif

typedef enum {
    TaskGenerator,
    NecroTail,
    NecroBody,
    NecroHead,
    DebugPrinter,
} ProcessType;

ProcessType get_process_type(u32 n) {
    mpi_assert(n >= 0 && n < PROCESS_COUNT);

    if      (n == TASK_GENERATOR) return TaskGenerator;
    else if (n < NECRO_TAIL_END)  return NecroTail;
    else if (n < NECRO_BODY_END)  return NecroBody;
    else if (n < NECRO_HEAD_END)  return NecroHead;
    else return DebugPrinter;
}

char *get_process_type_string(u32 n) {
    ProcessType type = get_process_type(n);
    switch (type) {
        case TaskGenerator: return "Task";
        case NecroTail:     return "Tail";
        case NecroBody:     return "Body";
        case NecroHead:     return "Head";
        case DebugPrinter:  return "Dbug";
    }
    return "????";
}

typedef enum {
    MsgNewTask,
    MsgRingRequest,
    MsgRingRelease,
    MsgRingAccept,
    MsgDeskRequest,
    MsgDeskAck,
    MsgSkeletonRequest,
    MsgSkeletonAck,
    MsgFree,
    MsgToken,
} MessageType;

char *get_msg_type_string(MessageType type) {
    switch (type) {
        case MsgNewTask:         return "NewTask";
        case MsgRingRequest:     return "RingReq";
        case MsgRingRelease:     return "RingRel";
        case MsgRingAccept:      return "RingAcc";
        case MsgDeskRequest:     return "DeskReq";
        case MsgDeskAck:         return "DeskAck";
        case MsgSkeletonRequest: return "SkelReq";
        case MsgSkeletonAck:     return "SkelAck";
        case MsgFree:            return "Free";
        case MsgToken:           return "Token";
    }
    return "????";
}

union __MAX_RING_ARRAY{
    u32 a[NECRO_HEAD_COUNT];
    u32 b[NECRO_TAIL_COUNT];
    u32 c[NECRO_BODY_COUNT];
};

#define MAX_RING_COUNT (sizeof(union __MAX_RING_ARRAY) / sizeof(u32))

typedef struct {
    u32 requests_handled[PROCESS_COUNT];
    u32 current_task[MAX_RING_COUNT];
} MessageToken;

typedef union {
    MessageToken token; // used by MsgToken
    u32 task_id;        // used by MsgFree, MsgDesk- and MsgSkeleton-
} Message;

#ifdef RELEASE
#define send_debug(...)
#else
#define send_debug(type, dest) do {\
    MessageType m_type = type;\
    u32 _dest = (dest);\
    MPI_Send(&_dest, sizeof(_dest), MPI_BYTE, DEBUG_PRINTER, m_type, MPI_COMM_WORLD);\
} while (0)
#endif

#define send_msg2(type, dest) do {\
    MessageType m_type = type;\
    MPI_Send(0, 0, MPI_BYTE, dest, m_type, MPI_COMM_WORLD);\
    send_debug(type, dest);\
} while (0)

#define send_msg3(data, type, dest) do {\
    MessageType m_type = type;\
    MPI_Send(&(data), sizeof(data), MPI_BYTE, dest, m_type, MPI_COMM_WORLD);\
    send_debug(type, dest);\
} while (0)

#define GET_MACRO(_1, _2, _3, NAME, ...) NAME
#define send_msg(...) GET_MACRO(__VA_ARGS__, send_msg3, send_msg2)(__VA_ARGS__)

typedef struct {
    u32 index;
    u32 begin;
    u32 count;
    u32 requests_received[PROCESS_COUNT];
    u32 requests_handled[PROCESS_COUNT];
    u32 current_task_issuer;
    bool has_token;
    bool pass_token_to_first_free;
    u32 *requests;
    u32 last_task_completed[MAX_RING_COUNT];
    u32 current_task[MAX_RING_COUNT];
} Ring;

//
// CODE
//

bool ring_pass_token(Ring *r) {
    // find the next free process in the ring
    u32 next = r->index;
    bool found = false;
    for (u32 i = 1; i < r->count; i++) {
        u32 index = (r->index + i) % r->count;
        if (r->current_task[index] == 0) {
            next = index;
            found = true;
            break;
        }
    }

    if (found) {
        mpi_assert(next != r->index);
        u32 next_rank = r->begin + next;
        MessageToken new_token = {};
        memcpy(new_token.requests_handled,
               r->requests_handled,
               PROCESS_COUNT*sizeof(u32));
        memcpy(new_token.current_task,
               r->current_task,
               r->count*sizeof(u32));
        send_msg(new_token, MsgToken, next_rank);
        return true;
    } else {
        return false;
    }
}

void ring_accept_task(Ring *r) {
    mpi_assert(r->has_token);
    u32 source = r->requests[0];
    r->requests_handled[source]++;
    r->current_task_issuer = source;
    arrdel(r->requests, 0);
    r->has_token = false;
    u32 sum = 0;
    for (u32 i = 0; i < PROCESS_COUNT; i++) {
        sum += r->requests_handled[i];
    }
    r->current_task[r->index] = sum;
    if (!ring_pass_token(r)) {
        r->pass_token_to_first_free = true;
    }
}

void ring_handle_request(Ring *r, MPI_Status status) {
    u32 rank = r->begin + r->index;
    u32 source = status.MPI_SOURCE;
    r->requests_received[source]++;
    if (r->requests_handled[source] < r->requests_received[source]) {
        arrpush(r->requests, source);
        if (r->has_token) {
            log("source %u, take request %u, handled %u",
                source,
                r->requests_received[source],
                r->requests_handled[source]);
            ring_accept_task(r);
        }
    }
}

void ring_handle_token(Ring *r, MessageToken token) {
    mpi_assert(!r->has_token);
    r->has_token = true;
    // fix token info
    for (u32 i = 0; i < r->count; i++) {
        if (token.current_task[i] == r->last_task_completed[i]) {
            token.current_task[i] = 0;
        }
    }
    memcpy(r->current_task, token.current_task, r->count*sizeof(u32));

    // forget remembered requests if they were handled
    for (u32 i = 0; i < PROCESS_COUNT; i++) {
        mpi_assert(r->requests_handled[i] <= token.requests_handled[i]);
        while (r->requests_handled[i] < token.requests_handled[i]) {
            for (u32 j = 0; j < arrlenu(r->requests); j++) {
                if (r->requests[j] == i) {
                    arrdel(r->requests, j);
                    break;
                }
            }
            r->requests_handled[i]++;
        }
    }

    if (arrlen(r->requests) > 0) {
        ring_accept_task(r);
    }
}

void ring_handle_free(Ring *r, u32 task_id, MPI_Status status) {
    u32 rank = r->index + r->begin;
    u32 freed_index = (u32)status.MPI_SOURCE - r->begin;
    r->last_task_completed[freed_index] = task_id;
    r->current_task[freed_index] = 0;
    if (r->pass_token_to_first_free) {
        log("Passing token to first free");
        bool passed = ring_pass_token(r);
        if (passed) {
            r->pass_token_to_first_free = false;
        }
    }
}

void ring_loop(u32 rank, u32 ring_begin, u32 ring_count) {
    Ring r = {};
    r.index = rank - ring_begin; // index within the ring
    r.begin = ring_begin;
    r.count = ring_count;
    r.has_token = (rank == ring_begin);

    for (;;) {
        Message msg = {};
        MPI_Status status = {};
        MPI_Recv(&msg, sizeof(msg), MPI_BYTE, MPI_ANY_SOURCE,
                 MPI_ANY_TAG, MPI_COMM_WORLD, &status);
        switch (status.MPI_TAG) {
            case MsgRingRequest: {
                bool had_token = r.has_token;
                ring_handle_request(&r, status);
                if (had_token && !r.has_token) {
                    send_msg(MsgRingAccept, status.MPI_SOURCE);
                }
            } break;
            case MsgToken: {
                ring_handle_token(&r, msg.token);
                if (!r.has_token) { // task was accepted immediately
                    send_msg(MsgRingAccept, r.current_task_issuer);
                }
            } break;
            case MsgFree: {
                ring_handle_free(&r, msg.task_id, status);
            } break;
            case MsgRingRelease: {
                u32 task_id = r.current_task[r.index];
                r.last_task_completed[r.index] = task_id;
                r.current_task[r.index] = 0;
                // broadcast MsgFree in the ring
                for (u32 i = 0; i < r.count; i++) {
                    if (i == r.index) continue;
                    send_msg(task_id, MsgFree, r.begin+i);
                }
            } break;
        }
    }
}

typedef struct {
    u32 process_id; // the requesting process
    u32 task_id; // current task handled by the requesting process
} QueueElement;

// a resource acquired using the modified Ricart-Agrawala algorithm
typedef struct {
    QueueElement *queue;
    u32 ack_received;
    bool acquired;
    MessageType request_type;
    MessageType ack_type;
} Resource;

void resource_handle_request(Resource *resource, u32 task_id, u32 source) {
    if (!resource->acquired) {
        send_msg(task_id, resource->ack_type, source);
    } else {
        QueueElement el;
        el.process_id = source;
        el.task_id = task_id;
        arrpush(resource->queue, el);
    }
}

void resource_free(Resource *resource) {
    resource->acquired = false;
    for (u32 i = 0; i < arrlenu(resource->queue); i++) {
        u32 process_id = resource->queue[i].process_id;
        u32 task_id = resource->queue[i].task_id;
        send_msg(task_id, resource->ack_type, process_id);
    }
    if (resource->queue) {
        arrdeln(resource->queue, 0, arrlen(resource->queue));
    }
}

void necro_head_loop(u32 rank) {
    enum State { // main cycle phase
        StateIdle,
        StateGetTheGuys,
        StateWaitForTheGuys,
        StateWaitForDesk,
        StateWaitForSkeleton
    } state;
    state = StateIdle;

    // body and tail necromants that will be acquired by the head necromant
    u32 body_rank = 0;
    u32 tail_rank = 0;

    Ring r = {};
    r.index = rank - NECRO_HEAD_BEGIN;
    r.begin = NECRO_HEAD_BEGIN;
    r.count = NECRO_HEAD_COUNT;
    r.has_token = (rank == NECRO_HEAD_BEGIN);

    Resource desk = {};
    desk.request_type = MsgDeskRequest;
    desk.ack_type     = MsgDeskAck;

    Resource skeleton = {};
    skeleton.request_type = MsgSkeletonRequest;
    skeleton.ack_type     = MsgSkeletonAck;

    for (;;) {
        Message msg = {};
        MPI_Status status = {};
        MPI_Recv(&msg, sizeof(msg), MPI_BYTE, MPI_ANY_SOURCE,
                 MPI_ANY_TAG, MPI_COMM_WORLD, &status);
        u32 source = status.MPI_SOURCE;

        switch (status.MPI_TAG) {
            case MsgNewTask: {
                bool had_token = r.has_token;
                ring_handle_request(&r, status);
                if (had_token && !r.has_token) {
                    mpi_assert(state == StateIdle);
                    state = StateGetTheGuys;
                }
            } break;
            case MsgToken: {
                ring_handle_token(&r, msg.token);
                if (!r.has_token) { // task was accepted immediately
                    mpi_assert(state == StateIdle);
                    state = StateGetTheGuys;
                }
            } break;
            case MsgFree: {
                ring_handle_free(&r, msg.task_id, status);
            } break;

            case MsgRingAccept: {
                if (state != StateWaitForTheGuys) {
                    log("current state: %d", state);
                    log("body: %d", body_rank);
                    log("tail: %d", tail_rank);
                    log("acc came from: %d", status.MPI_SOURCE);
                    mpi_assert(false);
                }
                ProcessType t = get_process_type(source);
                if (t == NecroTail) {
                    tail_rank = source;
                }
                if (t == NecroBody) {
                    body_rank = source;
                }
            } break;

            case MsgDeskRequest: {
                resource_handle_request(&desk, msg.task_id, source);
            } break;

            case MsgDeskAck: {
                u32 current_task = r.current_task[r.index];
                if (msg.task_id == current_task) {
                    desk.ack_received++;
                }
            } break;

            case MsgSkeletonRequest: {
                resource_handle_request(&skeleton, msg.task_id, source);
            } break;

            case MsgSkeletonAck: {
                u32 current_task = r.current_task[r.index];
                if (msg.task_id == current_task) {
                    skeleton.ack_received++;
                }
            } break;
        }

        switch (state) {
            case StateIdle: break;

            case StateGetTheGuys: {
                for (u32 i = NECRO_TAIL_BEGIN; i < NECRO_TAIL_END; i++) {
                    send_msg(MsgRingRequest, i);
                }
                for (u32 i = NECRO_BODY_BEGIN; i < NECRO_BODY_END; i++) {
                    send_msg(MsgRingRequest, i);
                }
                state = StateWaitForTheGuys;
            } break;

            case StateWaitForTheGuys: {
                if (body_rank && tail_rank) {
                    u32 current_task = r.current_task[r.index];
                    for (u32 i = NECRO_HEAD_BEGIN; i < NECRO_HEAD_END; i++) {
                        if (i == rank) continue;
                        send_msg(current_task, MsgDeskRequest, i);
                    }
                    state = StateWaitForDesk;
                    desk.ack_received = 0;
                }
            } break;

            case StateWaitForDesk: {
                if (desk.ack_received + DESK_COUNT > NECRO_HEAD_COUNT) {
                    log("Desk acquired after %u acks", desk.ack_received);
                    desk.acquired = true;

                    u32 current_task = r.current_task[r.index];
                    for (u32 i = NECRO_HEAD_BEGIN; i < NECRO_HEAD_END; i++) {
                        if (i == rank) continue;
                        send_msg(current_task, MsgSkeletonRequest, i);
                    }
                    state = StateWaitForSkeleton;
                    skeleton.ack_received = 0;
                }
            } break;

            case StateWaitForSkeleton: {
                if (desk.ack_received + DRAGON_COUNT > NECRO_HEAD_COUNT) {
                    log("Skeleton acquired after %u acks", desk.ack_received);
                    skeleton.acquired = true;

                    log("got body %u and tail %u\n\n", body_rank, tail_rank);
                    send_msg(MsgRingRelease, body_rank);
                    send_msg(MsgRingRelease, tail_rank);
                    body_rank = 0;
                    tail_rank = 0;
                    state = StateIdle;

                    u32 task_id = r.current_task[r.index];
                    r.current_task[r.index] = 0;
                    r.last_task_completed[r.index] = task_id;

                    for (u32 i = 0; i < r.count; i++) {
                        if (i == r.index) continue;
                        send_msg(task_id, MsgFree, r.begin+i);
                    }
                    
                    resource_free(&desk);
                    resource_free(&skeleton);
                }
            } break;
        }
    }
}

void task_generator_loop() {
    u32 task_count = 0;
    for (;;) {
        for (u32 i = NECRO_HEAD_BEGIN; i < NECRO_HEAD_END; i++) {
            send_msg(MsgNewTask, i);
        }
        task_count++;
        //if (task_count >= 3) break;
        usleep(1000*1000*TASK_GENERATION_DELAY);
    }
}

void debug_printer_loop(u32 rank) {
    for (;;) {
        u32 target;
        MPI_Status status = {};
        MPI_Recv(&target, sizeof(target), MPI_BYTE, MPI_ANY_SOURCE,
                 MPI_ANY_TAG, MPI_COMM_WORLD, &status);

        u32 source = status.MPI_SOURCE;
        char *msg_type = get_msg_type_string((MessageType)status.MPI_TAG);
        printf("%s(%u)->%s(%u): %s\n",
                get_process_type_string(source), source,
                get_process_type_string(target), target,
                msg_type);
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
        printf("Got %d processes, wanted %d\n", size, PROCESS_COUNT);
        mpi_assert(!"Incorrect process count!");
    }

    ProcessType my_type = get_process_type(rank);
    switch (my_type) {
        case TaskGenerator:
            task_generator_loop();
            break;
        case NecroTail:
            ring_loop((u32)rank, NECRO_TAIL_BEGIN, NECRO_TAIL_COUNT);
            break;
        case NecroBody:
            ring_loop((u32)rank, NECRO_BODY_BEGIN, NECRO_BODY_COUNT);
            break;
        case NecroHead:
            necro_head_loop((u32)rank);
            break;
        case DebugPrinter:
            debug_printer_loop((u32)rank);
            break;
    }

    MPI_Finalize();
}
