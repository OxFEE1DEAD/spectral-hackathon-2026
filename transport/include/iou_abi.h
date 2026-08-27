// The io_uring kernel ABI, declared locally.
//
// Why this file exists rather than an #include of <liburing.h>: the interface we need
// is a stable kernel ABI, and declaring it here keeps the relay buildable with nothing
// but a C++17 compiler. That matters more than it might seem, because userspace headers
// and libraries can lag the running kernel by years -- and when they do, depending on
// them blocks work on a kernel that is already capable of it. Declaring the ABI directly
// makes the build depend on the kernel actually running, which is the thing whose
// behaviour is being measured.
//
// Declaring an ABI by hand is only safe if wrong values fail loudly, so there are two
// independent guards:
//
//   * Compile time -- every structure below carries static_asserts on its size and on
//     the offset of each field the kernel reads. A mistyped field order cannot build.
//   * Run time -- probe_ops() asks the kernel which opcodes it implements via
//     IORING_REGISTER_PROBE, and Ring::open() refuses to start unless the operations it
//     is about to submit are among them. A wrong opcode fails at startup with a
//     message, not as mysterious latency.
//
// The values were taken from the upstream v6.9 uapi header and cross-checked field by
// field against it; that header is dual licensed (GPL-2.0 WITH Linux-syscall-note) OR
// MIT. Nothing here is copied from it -- these are our own declarations of the same
// interface, covering only the operations the relay issues.
#pragma once

#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>

namespace iou::abi {

// ---------------------------------------------------------------------------
// System calls. io_uring has no libc wrappers, so these go through syscall(2).
// ---------------------------------------------------------------------------
inline constexpr long kSysSetup = 425;
inline constexpr long kSysEnter = 426;
inline constexpr long kSysRegister = 427;

// ---------------------------------------------------------------------------
// Submission and completion entries.
//
// The submission entry is one 64-byte cache line and the kernel reads it field by
// field, so the layout is not negotiable. Upstream expresses the trailing fields as
// unions across every operation type; we name only the members the relay sets and
// assert their offsets, which is both shorter and harder to get quietly wrong.
// ---------------------------------------------------------------------------
struct Sqe {
  uint8_t opcode;
  uint8_t flags;       // IOSQE_*
  uint16_t ioprio;     // per-op flags; IORING_RECV_MULTISHOT for RECV
  int32_t fd;          // socket, or fixed-file index with IOSQE_FIXED_FILE
  uint64_t off;        // unused by SEND/RECV (upstream: off / addr2)
  uint64_t addr;       // buffer address
  uint32_t len;        // buffer length
  uint32_t op_flags;   // msg_flags for SEND/RECV (upstream: a union of *_flags)
  uint64_t user_data;  // returned verbatim in the completion
  uint16_t buf_index;  // buffer group id when IOSQE_BUFFER_SELECT is set
  uint16_t personality;
  int32_t file_index;
  uint64_t addr3;
  uint64_t pad2;
};

static_assert(sizeof(Sqe) == 64, "submission entry must be 64 bytes");
static_assert(offsetof(Sqe, opcode) == 0, "Sqe layout");
static_assert(offsetof(Sqe, flags) == 1, "Sqe layout");
static_assert(offsetof(Sqe, ioprio) == 2, "Sqe layout");
static_assert(offsetof(Sqe, fd) == 4, "Sqe layout");
static_assert(offsetof(Sqe, off) == 8, "Sqe layout");
static_assert(offsetof(Sqe, addr) == 16, "Sqe layout");
static_assert(offsetof(Sqe, len) == 24, "Sqe layout");
static_assert(offsetof(Sqe, op_flags) == 28, "Sqe layout");
static_assert(offsetof(Sqe, user_data) == 32, "Sqe layout");
static_assert(offsetof(Sqe, buf_index) == 40, "Sqe layout");
static_assert(offsetof(Sqe, personality) == 42, "Sqe layout");
static_assert(offsetof(Sqe, file_index) == 44, "Sqe layout");
static_assert(offsetof(Sqe, addr3) == 48, "Sqe layout");

struct Cqe {
  uint64_t user_data;  // whatever the submission put there
  int32_t res;         // bytes transferred, or -errno
  uint32_t flags;      // IORING_CQE_F_*, plus a buffer id in the top 16 bits
};

static_assert(sizeof(Cqe) == 16, "completion entry must be 16 bytes");
static_assert(offsetof(Cqe, res) == 8, "Cqe layout");
static_assert(offsetof(Cqe, flags) == 12, "Cqe layout");

// ---------------------------------------------------------------------------
// Ring geometry, returned by io_uring_setup() and used to find each shared field
// inside the mmapped regions.
// ---------------------------------------------------------------------------
struct SqRingOffsets {
  uint32_t head, tail, ring_mask, ring_entries, flags, dropped, array, resv1;
  uint64_t resv2;
};

struct CqRingOffsets {
  uint32_t head, tail, ring_mask, ring_entries, overflow, cqes, flags, resv1;
  uint64_t resv2;
};

static_assert(sizeof(SqRingOffsets) == 40, "SqRingOffsets layout");
static_assert(sizeof(CqRingOffsets) == 40, "CqRingOffsets layout");
static_assert(offsetof(CqRingOffsets, flags) == 24, "CqRingOffsets layout");

struct Params {
  uint32_t sq_entries;
  uint32_t cq_entries;
  uint32_t flags;
  uint32_t sq_thread_cpu;
  uint32_t sq_thread_idle;
  uint32_t features;
  uint32_t wq_fd;
  uint32_t resv[3];
  SqRingOffsets sq_off;
  CqRingOffsets cq_off;
};

static_assert(sizeof(Params) == 120, "Params layout");
static_assert(offsetof(Params, features) == 20, "Params layout");
static_assert(offsetof(Params, sq_off) == 40, "Params layout");
static_assert(offsetof(Params, cq_off) == 80, "Params layout");

// ---------------------------------------------------------------------------
// Provided-buffer ring: the mechanism that lets one multishot receive keep
// completing without us re-submitting anything. We publish buffers by advancing
// `tail`; the kernel consumes them and reports which one it used via the buffer id
// in the completion flags.
// ---------------------------------------------------------------------------
struct Buf {
  uint64_t addr;
  uint32_t len;
  uint16_t bid;
  uint16_t resv;
};

// The kernel overlays this on the same memory as the buffer array: `tail` sits at
// offset 14, which is the `resv` field of buffer slot zero. Slot zero is still a
// perfectly usable buffer, because publishing one writes only addr/len/bid and never
// touches resv. The static_assert below pins that coincidence down, since the whole
// scheme silently breaks if the two ever stop lining up.
struct BufRing {
  uint64_t resv1;
  uint32_t resv2;
  uint16_t resv3;
  uint16_t tail;
};

struct BufReg {
  uint64_t ring_addr;
  uint32_t ring_entries;
  uint16_t bgid;
  uint16_t flags;
  uint64_t resv[3];
};

static_assert(sizeof(Buf) == 16, "Buf layout");
static_assert(sizeof(BufRing) == 16, "BufRing must overlay one Buf slot");
static_assert(offsetof(BufRing, tail) == 14, "BufRing tail must land in Buf::resv");
static_assert(sizeof(BufReg) == 40, "BufReg layout");
static_assert(offsetof(BufReg, bgid) == 12, "BufReg layout");

// Registered NAPI busy polling. This is the io_uring counterpart of SO_BUSY_POLL: it
// lets the kernel poll the device queue on the thread that is waiting for completions,
// instead of leaving delivery to a softirq on some other core. On the measured path
// that mechanism is worth 1.7x at the median and 3.4x at p99.9, so an io_uring receive
// path without it starts at a large handicap.
struct Napi {
  uint32_t busy_poll_to;      // microseconds
  uint8_t prefer_busy_poll;
  uint8_t pad[3];
  uint64_t resv;
};

static_assert(sizeof(Napi) == 16, "Napi layout");
static_assert(offsetof(Napi, prefer_busy_poll) == 4, "Napi layout");

struct ProbeOp {
  uint8_t op;
  uint8_t resv;
  uint16_t flags;
  uint32_t resv2;
};

struct Probe {
  uint8_t last_op;
  uint8_t ops_len;
  uint16_t resv;
  uint32_t resv2[3];
  // Followed by ops_len ProbeOp entries; allocate with ProbeBuf below.
};

static_assert(sizeof(ProbeOp) == 8, "ProbeOp layout");
static_assert(sizeof(Probe) == 16, "Probe layout");

// ---------------------------------------------------------------------------
// Constants. Grouped as upstream groups them; only what the relay uses.
// ---------------------------------------------------------------------------

// io_uring_setup() flags
inline constexpr uint32_t kSetupSqPoll = 1u << 1;         // kernel-side submission thread
inline constexpr uint32_t kSetupSqAff = 1u << 2;          // pin that thread to sq_thread_cpu
inline constexpr uint32_t kSetupCqSize = 1u << 3;
inline constexpr uint32_t kSetupSubmitAll = 1u << 7;
inline constexpr uint32_t kSetupCoopTaskrun = 1u << 8;
inline constexpr uint32_t kSetupTaskrunFlag = 1u << 9;
inline constexpr uint32_t kSetupSingleIssuer = 1u << 12;  // one submitting thread, ever
inline constexpr uint32_t kSetupDeferTaskrun = 1u << 13;  // run completions only when we ask
inline constexpr uint32_t kSetupNoSqArray = 1u << 16;

// Operations
inline constexpr uint8_t kOpNop = 0;
inline constexpr uint8_t kOpSendmsg = 9;
inline constexpr uint8_t kOpRecvmsg = 10;
inline constexpr uint8_t kOpSend = 26;
inline constexpr uint8_t kOpRecv = 27;
inline constexpr uint8_t kOpSendZc = 47;

// Sqe::flags
inline constexpr uint8_t kSqeFixedFile = 1u << 0;
inline constexpr uint8_t kSqeBufferSelect = 1u << 5;
inline constexpr uint8_t kSqeCqeSkipSuccess = 1u << 6;

// Sqe::ioprio, for RECV
inline constexpr uint16_t kRecvMultishot = 1u << 1;

// io_uring_enter() flags
inline constexpr uint32_t kEnterGetevents = 1u << 0;
inline constexpr uint32_t kEnterSqWakeup = 1u << 1;
inline constexpr uint32_t kEnterRegisteredRing = 1u << 4;

// Submission ring flags, read from shared memory
inline constexpr uint32_t kSqNeedWakeup = 1u << 0;  // SQPOLL thread has parked
inline constexpr uint32_t kSqCqOverflow = 1u << 1;

// Cqe::flags
inline constexpr uint32_t kCqeFBuffer = 1u << 0;         // top bits carry a buffer id
inline constexpr uint32_t kCqeFMore = 1u << 1;           // multishot stays armed
inline constexpr uint32_t kCqeFSockNonempty = 1u << 2;   // more data already queued
inline constexpr uint32_t kCqeBufferShift = 16;

// mmap() offsets
inline constexpr uint64_t kOffSqRing = 0ull;
inline constexpr uint64_t kOffCqRing = 0x8000000ull;
inline constexpr uint64_t kOffSqes = 0x10000000ull;

// io_uring_register() opcodes
inline constexpr uint32_t kRegFiles = 2;
inline constexpr uint32_t kRegProbe = 8;
inline constexpr uint32_t kRegPbufRing = 22;
inline constexpr uint32_t kUnregPbufRing = 23;
inline constexpr uint32_t kRegNapi = 27;
inline constexpr uint32_t kUnregNapi = 28;

// Params::features
inline constexpr uint32_t kFeatSingleMmap = 1u << 0;
inline constexpr uint32_t kFeatNodrop = 1u << 1;
inline constexpr uint32_t kFeatExtArg = 1u << 8;

// ProbeOp::flags
inline constexpr uint16_t kOpSupported = 1u << 0;

// ---------------------------------------------------------------------------
// Syscall wrappers.
// ---------------------------------------------------------------------------
inline int setup(uint32_t entries, Params* p) {
  return static_cast<int>(::syscall(kSysSetup, entries, p));
}

inline int enter(int fd, uint32_t to_submit, uint32_t min_complete, uint32_t flags) {
  // sig/sz are only meaningful with kEnterExtArg, which we do not use.
  return static_cast<int>(
      ::syscall(kSysEnter, fd, to_submit, min_complete, flags, nullptr, 0));
}

// nr_args is per-opcode and the kernel validates it before dispatch, so getting it
// wrong reads exactly like an unsupported opcode. Registered NAPI in particular wants
// 1 here, not 0.
inline int register_(int fd, uint32_t opcode, void* arg, uint32_t nr_args) {
  return static_cast<int>(::syscall(kSysRegister, fd, opcode, arg, nr_args));
}

// Enough room for every opcode the kernel could report.
struct ProbeBuf {
  Probe hdr;
  ProbeOp ops[256];
};

// Ask the kernel which operations it implements. Returns false if the probe itself is
// unavailable, in which case the caller should not treat absence as a verdict.
inline bool probe_ops(int ring_fd, ProbeBuf* out) {
  *out = ProbeBuf{};
  return register_(ring_fd, kRegProbe, out, 256) == 0;
}

inline bool op_supported(const ProbeBuf& p, uint8_t op) {
  return op <= p.hdr.last_op && (p.ops[op].flags & kOpSupported) != 0;
}

// ---------------------------------------------------------------------------
// Shared-memory access helpers.
//
// The head and tail cursors live in memory shared with the kernel, so ordinary loads
// and stores are not enough: the store that publishes a submission must be visible
// only after the entry it refers to, and the load of a completion tail must not be
// reordered ahead of the entries it covers. These wrap the two orderings that matters,
// so no call site has to remember which is which.
// ---------------------------------------------------------------------------
inline uint32_t load_acquire(const uint32_t* p) {
  return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

inline void store_release(uint32_t* p, uint32_t v) {
  __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

inline uint32_t load_relaxed(const uint32_t* p) {
  return __atomic_load_n(p, __ATOMIC_RELAXED);
}

inline void store_relaxed(uint16_t* p, uint16_t v) {
  __atomic_store_n(p, v, __ATOMIC_RELAXED);
}

inline void store_release(uint16_t* p, uint16_t v) {
  __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

}  // namespace iou::abi
