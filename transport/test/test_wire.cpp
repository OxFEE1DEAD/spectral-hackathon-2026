// Datagram framing. The truncation cases matter most: this parser eats
// untrusted bytes off the network, and under -fsanitize=address a missing
// bounds check here shows up as a crash rather than as a silent bad read.
#include "wire.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "check.h"
#include "message.h"

namespace {

void test_roundtrip() {
  std::vector<uint8_t> buf(wire::kMaxDatagram);
  wire::Builder b(buf.data(), wire::kMaxDatagram);
  b.begin(/*dgram_id=*/7, /*stream_id=*/1, wire::kEncRaw, wire::kRoleData);
  CHECK(b.empty());

  // Three frames of the sizes the harness actually emits.
  std::vector<std::vector<uint8_t>> frames;
  const uint32_t sizes[] = {sizeof(msg::Trade), sizeof(msg::Bbo), 64};
  for (unsigned i = 0; i < 3; ++i) {
    std::vector<uint8_t> f(sizes[i]);
    for (uint32_t j = 0; j < sizes[i]; ++j) f[j] = static_cast<uint8_t>(i * 31 + j);
    CHECK(b.append_raw(f.data(), sizes[i], 1000 + i, 5000 + i));
    frames.push_back(std::move(f));
  }
  CHECK(!b.empty());

  wire::Reader r;
  CHECK(r.reset(b.data(), b.size()));
  CHECK_EQ(r.n_msgs(), 3u);
  CHECK(r.encoding() == wire::kEncRaw);
  CHECK(r.role() == wire::kRoleData);
  CHECK_EQ(r.header()->dgram_id, 7u);
  CHECK_EQ(r.header()->stream_id, 1u);
  // Bases come from the first message appended, which is what the codec will
  // later delta-code against.
  CHECK_EQ(r.header()->first_msg_seq, 1000u);
  CHECK_EQ(r.header()->base_ts_ns, 5000u);

  for (unsigned i = 0; i < 3; ++i) {
    const uint8_t* f = nullptr;
    uint32_t len = 0;
    CHECK(r.next_raw(&f, &len));
    CHECK_EQ(len, sizes[i]);
    CHECK(std::memcmp(f, frames[i].data(), len) == 0);
  }
  const uint8_t* f = nullptr;
  uint32_t len = 0;
  CHECK(!r.next_raw(&f, &len));
  CHECK(r.complete());
}

void test_capacity_refused() {
  // append_raw must refuse rather than grow: that refusal is what keeps a
  // datagram inside one MTU and therefore unfragmented.
  std::vector<uint8_t> buf(wire::kMaxDatagram);
  wire::Builder b(buf.data(), wire::kMaxDatagram);
  b.begin(1, 0, wire::kEncRaw, wire::kRoleData);

  std::vector<uint8_t> frame(sizeof(msg::OrderBook), 0xab);
  unsigned appended = 0;
  while (b.append_raw(frame.data(), static_cast<uint32_t>(frame.size()),
                      appended, appended)) {
    ++appended;
    CHECK(b.size() <= wire::kMaxDatagram);
  }
  CHECK(appended > 0);
  CHECK(b.size() <= wire::kMaxDatagram);

  wire::Reader r;
  CHECK(r.reset(b.data(), b.size()));
  CHECK_EQ(r.n_msgs(), appended);
  unsigned got = 0;
  const uint8_t* f = nullptr;
  uint32_t len = 0;
  while (r.next_raw(&f, &len)) {
    CHECK_EQ(len, frame.size());
    ++got;
  }
  CHECK_EQ(got, appended);
  CHECK(r.complete());
}

void test_malformed() {
  std::vector<uint8_t> buf(wire::kMaxDatagram);
  wire::Builder b(buf.data(), wire::kMaxDatagram);
  b.begin(1, 0, wire::kEncRaw, wire::kRoleData);
  std::vector<uint8_t> frame(128, 0x5a);
  for (int i = 0; i < 4; ++i)
    CHECK(b.append_raw(frame.data(), 128, static_cast<uint64_t>(i), 0));
  const uint32_t full = b.size();

  wire::Reader r;

  // Shorter than a header.
  CHECK(!r.reset(b.data(), 4));
  CHECK(!r.reset(b.data(), sizeof(wire::DgramHeader) - 1));

  // Wrong magic: a stray datagram on the port must not be parsed at all.
  std::vector<uint8_t> bad(b.data(), b.data() + full);
  bad[0] ^= 0xff;
  CHECK(!r.reset(bad.data(), full));

  // Truncated at every offset. Each must stop cleanly, hand back only the
  // whole messages it could decode, and never read past the buffer.
  for (uint32_t cut = sizeof(wire::DgramHeader); cut < full; ++cut) {
    std::vector<uint8_t> part(b.data(), b.data() + cut);
    CHECK(r.reset(part.data(), cut));
    unsigned got = 0;
    const uint8_t* f = nullptr;
    uint32_t len = 0;
    while (r.next_raw(&f, &len)) {
      CHECK(len == 128);
      ++got;
    }
    CHECK(got <= 4);
    CHECK(got == (cut - sizeof(wire::DgramHeader)) /
                     (sizeof(wire::RawRecord) + 128));
    CHECK(!r.complete());
  }

  // A header claiming more messages than the bytes can hold.
  std::vector<uint8_t> lying(b.data(), b.data() + full);
  reinterpret_cast<wire::DgramHeader*>(lying.data())->n_msgs = 100;
  CHECK(r.reset(lying.data(), full));
  unsigned got = 0;
  const uint8_t* f = nullptr;
  uint32_t len = 0;
  while (r.next_raw(&f, &len)) ++got;
  CHECK_EQ(got, 4u);
  CHECK(!r.complete());
}

void test_flags() {
  for (auto enc : {wire::kEncRaw, wire::kEncCodec}) {
    for (auto role : {wire::kRoleData, wire::kRoleParity, wire::kRoleRepair}) {
      const uint8_t fl = wire::make_flags(enc, role);
      CHECK(wire::encoding_of(fl) == enc);
      CHECK(wire::role_of(fl) == role);
    }
  }
}

}  // namespace

int main() {
  test_roundtrip();
  test_capacity_refused();
  test_malformed();
  test_flags();
  return check_report("wire");
}
