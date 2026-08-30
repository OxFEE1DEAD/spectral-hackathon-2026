// AF_XDP backend for the send path.
//
// This is the third submission mechanism, after kernel UDP sockets and io_uring. It goes
// a step further than either: the relay writes a complete Ethernet frame into memory the
// NIC driver reads from, so the kernel's IP and UDP layers, its routing lookup, its
// netfilter hooks and its queueing discipline are all skipped. What the relay gives up in
// exchange is that it must now build those headers itself, and be right about them.
//
// What this can and cannot demonstrate on the measured hardware, stated up front because
// it bounds the whole result:
//
//   * The NIC driver here implements XDP but **not** AF_XDP zero-copy -- there is no xsk
//     pool support in it at all, and binding with XDP_ZEROCOPY is refused. So the only
//     available mode is XDP_COPY, where transmission still runs through xsk_generic_xmit,
//     which allocates an skb and hands it to the driver via dev_direct_xmit.
//   * Copy mode therefore removes protocol processing but *keeps* skb allocation and the
//     driver transmit path. It is a partial bypass, and it is entirely possible for it to
//     be slower than a plain connected send(): we have added userspace header
//     construction and ring bookkeeping without removing the allocation.
//
// That prediction is worth making before measuring rather than after. The honest reason to
// run this experiment anyway is decomposition: comparing kernel UDP against AF_XDP copy
// mode isolates the cost of the protocol stack from the cost of skb allocation plus the
// driver, which no other configuration separates.
//
// Two operational facts follow from using AF_XDP at all:
//
//   * It needs CAP_NET_RAW, so the sender runs as root. That is a real deployment cost
//     and it is reported at startup rather than hidden.
//   * The bind is TX-oriented and deliberately conservative: XDP_COPY only, never
//     XDP_ZEROCOPY, because zero-copy asks the driver to re-provision the queue. No XDP
//     program is loaded and nothing is redirected, so the interface keeps delivering
//     received traffic to the kernel exactly as before. The receive half of AF_XDP does
//     require a program on the interface, which is a separate and more invasive step.
#pragma once

#include <arpa/inet.h>
#include <linux/if_ether.h>
#include <linux/if_xdp.h>
#include <net/if.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <array>
#include <cstring>
#include <string>
#include <vector>

#include "udp_backend.h"

namespace xdp {

// The kernel rejects a bind that carries only one direction, so a transmit-only socket
// still has to create all four rings. The fill and receive rings stay empty for the
// lifetime of the process, which costs nothing: without an XDP program the kernel never
// redirects anything into them.
inline constexpr uint32_t kTxRingSize = 2048;
inline constexpr uint32_t kCompRingSize = 2048;
inline constexpr uint32_t kRxRingSize = 64;    // never used, required by bind
inline constexpr uint32_t kFillRingSize = 64;  // never used, required by bind

// One frame per datagram in flight. 4096 x 2048 B is 8 MiB, comfortably inside the 64 MiB
// memlock ceiling these hosts impose, and deep enough that a full transmit ring plus a
// fan-out batch never runs the pool dry.
inline constexpr uint32_t kFrameCount = 4096;
inline constexpr uint32_t kFrameSize = 2048;

// The three headers the relay now writes itself, declared here rather than taken from
// <linux/ip.h>: that header collides with the <netinet/ip.h> the socket path already
// includes, and spelling the layout out is worth more than the two lines it saves.
// Every multi-byte field is network order.
struct EthHdr {
  uint8_t dst[6];
  uint8_t src[6];
  uint16_t ethertype;
} __attribute__((packed));

struct Ipv4Hdr {
  uint8_t ihl_version;  // low nibble IHL, high nibble version
  uint8_t tos;
  uint16_t tot_len;
  uint16_t id;
  uint16_t frag_off;
  uint8_t ttl;
  uint8_t protocol;
  uint16_t check;
  uint32_t saddr;
  uint32_t daddr;
} __attribute__((packed));

struct UdpHdr {
  uint16_t source;
  uint16_t dest;
  uint16_t len;
  uint16_t check;
} __attribute__((packed));

static_assert(sizeof(EthHdr) == 14, "Ethernet header must be 14 bytes");
static_assert(sizeof(Ipv4Hdr) == 20, "IPv4 header must be 20 bytes without options");
static_assert(sizeof(UdpHdr) == 8, "UDP header must be 8 bytes");

inline constexpr uint32_t kHeaderBytes =
    sizeof(EthHdr) + sizeof(Ipv4Hdr) + sizeof(UdpHdr);
inline constexpr uint8_t kIpProtoUdp = 17;
inline constexpr uint8_t kIpTosLowDelay = 0x10;
inline constexpr uint16_t kIpDontFragment = 0x4000;

struct Options {
  // The sockets the neighbour warm-up uses; the AF_XDP path itself has no socket options.
  udp::Options sock;
  std::string ifname;         // interface to transmit through; required
  uint32_t queue_id = 0;      // transmit queue to bind
  uint16_t src_port = 50000;  // source port stamped into every datagram
  // Compute the UDP checksum. Optional over IPv4, and off by default: the framing header
  // the receiver parses already carries a magic and a version and validates every frame
  // length, so a corrupted datagram is rejected as malformed rather than delivered. The
  // knob exists so that trade is visible instead of buried.
  bool udp_checksum = false;
};

// One-at-a-time internet checksum. Only ever runs over a 20-byte header, so the
// unrolled-and-vectorised versions are not worth the reading cost.
inline uint16_t inet_csum(const void* data, size_t len) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint32_t sum = 0;
  for (; len > 1; len -= 2, p += 2) {
    sum += static_cast<uint32_t>(p[0]) << 8 | p[1];
  }
  if (len == 1) sum += static_cast<uint32_t>(p[0]) << 8;
  while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
  return htons(static_cast<uint16_t>(~sum));
}

// A shared-memory ring cursor pair, as the kernel exposes it.
struct RingView {
  uint32_t* producer = nullptr;
  uint32_t* consumer = nullptr;
  uint32_t* flags = nullptr;
  void* desc = nullptr;
  uint32_t mask = 0;
  uint32_t cached_producer = 0;
};

inline uint32_t load_acquire(const uint32_t* p) {
  return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
inline void store_release(uint32_t* p, uint32_t v) {
  __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

// Read the interface's own MAC. Cheaper and more portable than an ioctl dance, and this
// runs once at startup.
inline bool read_local_mac(const std::string& ifname, uint8_t out[6]) {
  const std::string path = "/sys/class/net/" + ifname + "/address";
  FILE* f = fopen(path.c_str(), "r");
  if (f == nullptr) {
    fprintf(stderr, "cannot read MAC of %s: %s\n", ifname.c_str(),
            std::strerror(errno));
    return false;
  }
  unsigned v[6] = {0};
  const int n = fscanf(f, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
  fclose(f);
  if (n != 6) {
    fprintf(stderr, "unexpected MAC format for %s\n", ifname.c_str());
    return false;
  }
  for (int i = 0; i < 6; ++i) out[i] = static_cast<uint8_t>(v[i]);
  return true;
}

// The next hop's MAC, taken from the kernel's own neighbour cache.
//
// Implementing ARP here would mean reimplementing something the kernel is already doing
// correctly on this interface. Instead the caller sends one ordinary datagram to the
// destination first, which makes the kernel resolve it, and then we read the answer.
//
// The limitation is worth naming: because the relay's own traffic never goes through the
// kernel afterwards, nothing refreshes that entry. The cached MAC stays *correct* -- a
// peer's MAC does not change under it -- but a peer genuinely replaced mid-run would not
// be noticed. For a measurement run that is acceptable; for production it would want a
// netlink subscription.
inline bool neighbour_mac(const std::string& ifname, const sockaddr_in& peer,
                          uint8_t out[6]) {
  char want[INET_ADDRSTRLEN] = {0};
  inet_ntop(AF_INET, &peer.sin_addr, want, sizeof(want));
  FILE* f = fopen("/proc/net/arp", "r");
  if (f == nullptr) return false;
  char line[512];
  if (fgets(line, sizeof(line), f) == nullptr) {  // header row
    fclose(f);
    return false;
  }
  bool found = false;
  while (fgets(line, sizeof(line), f) != nullptr) {
    char ip[64], hw[64], dev[64], t1[64], t2[64], t3[64];
    if (sscanf(line, "%63s %63s %63s %63s %63s %63s", ip, t1, t2, hw, t3, dev) != 6) {
      continue;
    }
    if (std::strcmp(ip, want) != 0 || std::strcmp(dev, ifname.c_str()) != 0) continue;
    if (std::strcmp(hw, "00:00:00:00:00:00") == 0) continue;  // incomplete entry
    unsigned v[6] = {0};
    if (sscanf(hw, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6) {
      for (int i = 0; i < 6; ++i) out[i] = static_cast<uint8_t>(v[i]);
      found = true;
    }
    break;
  }
  fclose(f);
  return found;
}

// Provoke the kernel into resolving a destination, so neighbour_mac() has something to
// read. It is sent through an ordinary socket and is the only traffic this backend ever
// puts through the stack.
//
// Deliberately addressed to the discard port rather than to the peer's data port.
// Resolution depends on the address, not the port, and a stray byte arriving on the port
// a receiver is listening on shows up in its statistics as a malformed frame -- which is
// exactly what happened the first time this ran, and is the kind of self-inflicted noise
// that later gets mistaken for a transport defect.
inline constexpr uint16_t kNeighbourWarmPort = 9;  // RFC 863 discard

inline void warm_neighbour(const sockaddr_in& peer) {
  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return;
  sockaddr_in probe = peer;
  probe.sin_port = htons(kNeighbourWarmPort);
  const uint8_t byte = 0;
  sendto(fd, &byte, 1, MSG_DONTWAIT, reinterpret_cast<const sockaddr*>(&probe),
         sizeof(probe));
  close(fd);
}

// The source address to stamp into our frames is whichever one the kernel would have
// chosen for this destination. Asking it directly -- connect an unused datagram socket
// and read back the local end -- gets the routing decision right for free, where reading
// an interface address would guess at it.
inline bool source_ipv4_for(const sockaddr_in& peer, in_addr* out) {
  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return false;
  bool ok = false;
  if (connect(fd, reinterpret_cast<const sockaddr*>(&peer), sizeof(peer)) == 0) {
    sockaddr_in local{};
    socklen_t len = sizeof(local);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
      *out = local.sin_addr;
      ok = true;
    }
  }
  close(fd);
  return ok;
}

class Sender {
 public:
  ~Sender() {
    if (tx_map_ != nullptr) munmap(tx_map_, tx_bytes_);
    if (cq_map_ != nullptr) munmap(cq_map_, cq_bytes_);
    if (rx_map_ != nullptr) munmap(rx_map_, rx_bytes_);
    if (fq_map_ != nullptr) munmap(fq_map_, fq_bytes_);
    if (umem_ != nullptr) munmap(umem_, static_cast<size_t>(kFrameCount) * kFrameSize);
    if (fd_ >= 0) close(fd_);
  }

  bool open(const std::vector<std::string>& peers, uint16_t default_port,
            const Options& opts) {
    if (peers.empty()) {
      fprintf(stderr, "no peers given\n");
      return false;
    }
    if (opts.ifname.empty()) {
      fprintf(stderr, "--backend xdp requires --xdp-iface IFACE\n");
      return false;
    }
    opts_ = opts;

    addrs_.resize(peers.size());
    for (size_t i = 0; i < peers.size(); ++i) {
      if (!udp::resolve(peers[i], default_port, &addrs_[i])) return false;
    }

    if (!read_local_mac(opts.ifname, src_mac_)) return false;
    if (!source_ipv4_for(addrs_[0], &src_ip_)) {
      fprintf(stderr, "cannot determine the source address for %s\n",
              udp::describe(addrs_[0]).c_str());
      return false;
    }

    // Resolve every destination through the kernel once, before we stop using it.
    for (const sockaddr_in& a : addrs_) warm_neighbour(a);
    usleep(300000);
    dst_mac_.resize(addrs_.size());
    for (size_t i = 0; i < addrs_.size(); ++i) {
      if (!neighbour_mac(opts.ifname, addrs_[i], dst_mac_[i].data())) {
        fprintf(stderr,
                "no neighbour entry for %s on %s -- the destination must be reachable "
                "on that interface without a router hop\n",
                udp::describe(addrs_[i]).c_str(), opts.ifname.c_str());
        return false;
      }
    }

    if (!setup_socket()) return false;
    build_templates();
    // Every frame starts free; the completion ring hands them back as they drain.
    free_frames_.resize(kFrameCount);
    for (uint32_t i = 0; i < kFrameCount; ++i) free_frames_[i] = i;
    free_top_ = kFrameCount;
    return true;
  }

  size_t peer_count() const { return addrs_.size(); }
  const std::vector<sockaddr_in>& peers() const { return addrs_; }
  const char* method_name() const { return "afxdp-copy"; }
  uint64_t completion_errors() const { return completion_errors_; }
  uint64_t kicks() const { return kicks_; }
  uint64_t pool_stalls() const { return pool_stalls_; }

  // Put one datagram on every destination. Returns how many it reached, keeping the same
  // contract as the other two backends -- the descriptors are published and kicked before
  // returning, and frames are only reused once the kernel has completed them.
  int send_all(const void* payload, uint32_t len) {
    reap();
    const uint32_t n = static_cast<uint32_t>(addrs_.size());
    if (free_top_ < n || tx_space() < n) {
      // Drain and retry rather than dropping silently: a datagram lost here would look
      // exactly like a datagram lost on the wire, and they call for opposite responses.
      for (int spin = 0; spin < 1024 && (free_top_ < n || tx_space() < n); ++spin) {
        kick();
        reap();
      }
      if (free_top_ < n || tx_space() < n) {
        ++pool_stalls_;
        return 0;
      }
    }

    uint32_t prod = tx_.cached_producer;
    xdp_desc* ring = static_cast<xdp_desc*>(tx_.desc);
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t frame = free_frames_[--free_top_];
      const uint64_t addr = static_cast<uint64_t>(frame) * kFrameSize;
      uint8_t* p = umem_ + addr;
      // Header template first, then the payload behind it. One copy per destination,
      // which is what the kernel path costs too -- it copies into each skb.
      std::memcpy(p, templates_[i].data(), kHeaderBytes);
      std::memcpy(p + kHeaderBytes, payload, len);
      finish_headers(p, i, len);

      ring[prod & tx_.mask].addr = addr;
      ring[prod & tx_.mask].len = kHeaderBytes + len;
      ring[prod & tx_.mask].options = 0;
      ++prod;
    }
    tx_.cached_producer = prod;
    store_release(tx_.producer, prod);
    kick();
    return static_cast<int>(n);
  }

 private:
  using Mac = std::array<uint8_t, 6>;

  uint32_t tx_space() const {
    const uint32_t used = tx_.cached_producer - load_acquire(tx_.consumer);
    return kTxRingSize - used;
  }

  // In copy mode the transmit path only runs when we ask it to, so the kick is not
  // optional. It is one system call for the whole fan-out batch, which is the same shape
  // io_uring achieves, and it is counted so the cost is reported rather than assumed.
  void kick() {
    if (sendto(fd_, nullptr, 0, MSG_DONTWAIT, nullptr, 0) < 0) {
      if (errno != ENOBUFS && errno != EAGAIN && errno != EBUSY && errno != EINTR) {
        ++completion_errors_;
      }
    }
    ++kicks_;
  }

  // Take back every frame the kernel has finished with.
  void reap() {
    const uint32_t prod = load_acquire(cq_.producer);
    uint32_t cons = *cq_.consumer;
    if (prod == cons) return;
    const uint64_t* ring = static_cast<const uint64_t*>(cq_.desc);
    while (cons != prod) {
      const uint64_t addr = ring[cons & cq_.mask];
      const uint32_t frame = static_cast<uint32_t>(addr / kFrameSize);
      if (frame < kFrameCount && free_top_ < kFrameCount) {
        free_frames_[free_top_++] = frame;
      }
      ++cons;
    }
    store_release(cq_.consumer, cons);
  }

  bool setup_socket() {
    // Best effort: the UMEM is small enough for the default ceiling, so a refusal here
    // is not fatal and should not read like one.
    rlimit rl{RLIM_INFINITY, RLIM_INFINITY};
    setrlimit(RLIMIT_MEMLOCK, &rl);

    const size_t umem_bytes = static_cast<size_t>(kFrameCount) * kFrameSize;
    void* m = mmap(nullptr, umem_bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (m == MAP_FAILED) {
      fprintf(stderr, "mmap UMEM (%zu bytes) failed: %s\n", umem_bytes,
              std::strerror(errno));
      return false;
    }
    umem_ = static_cast<uint8_t*>(m);

    fd_ = socket(AF_XDP, SOCK_RAW, 0);
    if (fd_ < 0) {
      fprintf(stderr, "socket(AF_XDP) failed: %s\n", std::strerror(errno));
      if (errno == EPERM) {
        fprintf(stderr, "  AF_XDP needs CAP_NET_RAW -- run the sender as root\n");
      }
      return false;
    }

    xdp_umem_reg reg{};
    reg.addr = reinterpret_cast<uint64_t>(umem_);
    reg.len = umem_bytes;
    reg.chunk_size = kFrameSize;
    reg.headroom = 0;
    if (setsockopt(fd_, SOL_XDP, XDP_UMEM_REG, &reg, sizeof(reg)) != 0) {
      fprintf(stderr, "XDP_UMEM_REG failed: %s\n", std::strerror(errno));
      return false;
    }

    // All four, because a single-direction bind is refused. See kRxRingSize.
    const uint32_t fq = kFillRingSize, cq = kCompRingSize;
    const uint32_t rx = kRxRingSize, tx = kTxRingSize;
    if (setsockopt(fd_, SOL_XDP, XDP_UMEM_FILL_RING, &fq, sizeof(fq)) != 0 ||
        setsockopt(fd_, SOL_XDP, XDP_UMEM_COMPLETION_RING, &cq, sizeof(cq)) != 0 ||
        setsockopt(fd_, SOL_XDP, XDP_RX_RING, &rx, sizeof(rx)) != 0 ||
        setsockopt(fd_, SOL_XDP, XDP_TX_RING, &tx, sizeof(tx)) != 0) {
      fprintf(stderr, "AF_XDP ring setup failed: %s\n", std::strerror(errno));
      return false;
    }

    xdp_mmap_offsets off{};
    socklen_t olen = sizeof(off);
    if (getsockopt(fd_, SOL_XDP, XDP_MMAP_OFFSETS, &off, &olen) != 0) {
      fprintf(stderr, "XDP_MMAP_OFFSETS failed: %s\n", std::strerror(errno));
      return false;
    }

    if (!map_ring(off.tx, kTxRingSize, sizeof(xdp_desc), XDP_PGOFF_TX_RING, &tx_,
                  &tx_map_, &tx_bytes_) ||
        !map_ring(off.cr, kCompRingSize, sizeof(uint64_t),
                  XDP_UMEM_PGOFF_COMPLETION_RING, &cq_, &cq_map_, &cq_bytes_) ||
        !map_ring(off.rx, kRxRingSize, sizeof(xdp_desc), XDP_PGOFF_RX_RING, &rx_,
                  &rx_map_, &rx_bytes_) ||
        !map_ring(off.fr, kFillRingSize, sizeof(uint64_t), XDP_UMEM_PGOFF_FILL_RING,
                  &fq_, &fq_map_, &fq_bytes_)) {
      return false;
    }

    sockaddr_xdp sx{};
    sx.sxdp_family = AF_XDP;
    sx.sxdp_ifindex = if_nametoindex(opts_.ifname.c_str());
    if (sx.sxdp_ifindex == 0) {
      fprintf(stderr, "unknown interface %s\n", opts_.ifname.c_str());
      return false;
    }
    sx.sxdp_queue_id = opts_.queue_id;
    // Copy mode only, and never XDP_ZEROCOPY: zero-copy asks the driver to re-provision
    // the queue, and this driver does not implement xsk pools in any case.
    sx.sxdp_flags = XDP_COPY;
    if (bind(fd_, reinterpret_cast<sockaddr*>(&sx), sizeof(sx)) != 0) {
      fprintf(stderr, "AF_XDP bind (%s queue %u, copy mode) failed: %s\n",
              opts_.ifname.c_str(), opts_.queue_id, std::strerror(errno));
      if (errno == EBUSY) {
        fprintf(stderr, "  another socket already owns that queue\n");
      } else if (errno == EINVAL) {
        fprintf(stderr, "  queue %u must exist on this interface\n", opts_.queue_id);
      }
      return false;
    }
    tx_.cached_producer = *tx_.producer;
    return true;
  }

  bool map_ring(const xdp_ring_offset& off, uint32_t entries, size_t desc_size,
                off_t pgoff, RingView* view, void** map_out, size_t* bytes_out) {
    const size_t bytes = off.desc + static_cast<size_t>(entries) * desc_size;
    void* m = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                   fd_, pgoff);
    if (m == MAP_FAILED) {
      fprintf(stderr, "mmap AF_XDP ring failed: %s\n", std::strerror(errno));
      return false;
    }
    uint8_t* base = static_cast<uint8_t*>(m);
    view->producer = reinterpret_cast<uint32_t*>(base + off.producer);
    view->consumer = reinterpret_cast<uint32_t*>(base + off.consumer);
    view->flags = reinterpret_cast<uint32_t*>(base + off.flags);
    view->desc = base + off.desc;
    view->mask = entries - 1;
    *map_out = m;
    *bytes_out = bytes;
    return true;
  }

  // Everything about a destination that never changes, precomputed once so the send path
  // only fills in the length and the checksum.
  void build_templates() {
    templates_.assign(addrs_.size(), std::vector<uint8_t>(kHeaderBytes, 0));
    for (size_t i = 0; i < addrs_.size(); ++i) {
      uint8_t* p = templates_[i].data();
      EthHdr* eth = reinterpret_cast<EthHdr*>(p);
      std::memcpy(eth->dst, dst_mac_[i].data(), 6);
      std::memcpy(eth->src, src_mac_, 6);
      eth->ethertype = htons(ETH_P_IP);

      Ipv4Hdr* ip = reinterpret_cast<Ipv4Hdr*>(p + sizeof(EthHdr));
      ip->ihl_version = 0x45;       // IPv4, 5 words of header, no options
      ip->tos = kIpTosLowDelay;     // same request the socket path makes
      ip->id = 0;
      ip->frag_off = htons(kIpDontFragment);  // we never fragment; see the write-up
      ip->ttl = 64;
      ip->protocol = kIpProtoUdp;
      ip->saddr = src_ip_.s_addr;
      ip->daddr = addrs_[i].sin_addr.s_addr;

      UdpHdr* udp = reinterpret_cast<UdpHdr*>(p + sizeof(EthHdr) + sizeof(Ipv4Hdr));
      udp->source = htons(opts_.src_port);
      udp->dest = addrs_[i].sin_port;
      udp->check = 0;
    }
  }

  // The per-datagram part: lengths, then the header checksum over the result.
  void finish_headers(uint8_t* p, size_t peer, uint32_t payload_len) {
    Ipv4Hdr* ip = reinterpret_cast<Ipv4Hdr*>(p + sizeof(EthHdr));
    UdpHdr* udp = reinterpret_cast<UdpHdr*>(p + sizeof(EthHdr) + sizeof(Ipv4Hdr));
    const uint16_t udp_len = static_cast<uint16_t>(sizeof(UdpHdr) + payload_len);
    ip->tot_len = htons(static_cast<uint16_t>(sizeof(Ipv4Hdr) + udp_len));
    ip->check = 0;
    ip->check = inet_csum(ip, sizeof(Ipv4Hdr));
    udp->len = htons(udp_len);
    udp->check = 0;
    if (opts_.udp_checksum) {
      udp->check = udp_checksum(ip, udp, payload_len);
      // Zero is reserved to mean "no checksum", so a computed zero is sent as all ones.
      if (udp->check == 0) udp->check = 0xffff;
    }
    (void)peer;
  }

  uint16_t udp_checksum(const Ipv4Hdr* ip, const UdpHdr* udp,
                        uint32_t payload_len) const {
    // Pseudo-header, then the datagram, folded the usual way.
    const uint16_t udp_len = static_cast<uint16_t>(sizeof(UdpHdr) + payload_len);
    uint32_t sum = 0;
    const uint8_t* s = reinterpret_cast<const uint8_t*>(&ip->saddr);
    const uint8_t* d = reinterpret_cast<const uint8_t*>(&ip->daddr);
    for (int i = 0; i < 4; i += 2) sum += (uint32_t)s[i] << 8 | s[i + 1];
    for (int i = 0; i < 4; i += 2) sum += (uint32_t)d[i] << 8 | d[i + 1];
    sum += kIpProtoUdp;
    sum += udp_len;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(udp);
    size_t len = udp_len;
    for (; len > 1; len -= 2, p += 2) sum += (uint32_t)p[0] << 8 | p[1];
    if (len == 1) sum += (uint32_t)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return htons(static_cast<uint16_t>(~sum));
  }

  int fd_ = -1;
  Options opts_;
  uint8_t* umem_ = nullptr;
  void* tx_map_ = nullptr;
  void* cq_map_ = nullptr;
  void* rx_map_ = nullptr;
  void* fq_map_ = nullptr;
  size_t tx_bytes_ = 0, cq_bytes_ = 0, rx_bytes_ = 0, fq_bytes_ = 0;
  RingView tx_, cq_, rx_, fq_;

  std::vector<sockaddr_in> addrs_;
  std::vector<Mac> dst_mac_;
  std::vector<std::vector<uint8_t>> templates_;
  uint8_t src_mac_[6] = {0};
  in_addr src_ip_{};

  std::vector<uint32_t> free_frames_;
  uint32_t free_top_ = 0;
  uint64_t completion_errors_ = 0;
  uint64_t kicks_ = 0;
  uint64_t pool_stalls_ = 0;
};

}  // namespace xdp
