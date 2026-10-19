#include <assert.h>
#include <stdio.h>
#include <thread>
#include "../ESP32Drop/examples/SerialPhotoBridge/frame_policy.h"
#include "../ESP32Drop/src/airdrop/core/ad_power_gate.h"

static void quiet(FramePolicy &p, uint32_t now) {
  for (unsigned i = 0; i <= 10; ++i) p.sample(now - 500 + i * 50, true, false);
}
int main() {
  FrameNfcWake wake;
  assert(!wake.sample(true, true)); // same field as shutdown is not a new tap
  assert(!wake.sample(false, false)); // an I2C failure is not a clear field
  assert(!wake.sample(true, true));
  assert(!wake.sample(true, false));
  assert(!wake.sample(false, true)); // no reboot on invalid reads
  assert(wake.sample(true, true));
  // A short 5 ms pulse at any phase of the old 50 ms polling period must be
  // observable with the new standby schedule (assuming the loop keeps up).
  for (uint32_t phase = 0; phase < 50; ++phase) {
    FrameNfcWake tap; bool caught = false;
    const uint32_t start = 100 + phase;
    for (uint32_t t = 0; t < 200; t += FrameNfcWake::POLL_MS)
      caught |= tap.sample(true, t >= start && t < start + 5);
    assert(caught);
  }
  FramePolicy p; p.begin(100);
  quiet(p, 180099); assert(!p.due(180099));
  p.sample(180100, true, false); assert(p.due(180100));
  assert(!p.due(180251)); // stale NFC sample cannot authorize OFF
  p.sample(180300, true, true); assert(!p.due(180300));
  p.sample(180350, false, false); assert(!p.due(180350));
  quiet(p, 181000); assert(p.due(181000));
  p.hold = true; assert(!p.due(181000));
  p.manual = true; assert(p.due(181000));
  p.update(181000, true); assert(!p.due(181000));
  p.update(500000, true); quiet(p, 500000); assert(!p.due(500000));
  p.manual = false; p.hold = false;
  p.result(500000, true); p.update(501000, false);
  quiet(p, 530999); assert(!p.due(530999));
  p.sample(531000, true, false); assert(p.due(531000));
  p.result(531000, false); quiet(p, 561000); assert(!p.due(561000));
  quiet(p, 711000); assert(p.due(711000));
  p.renew(711000); assert(!p.due(711000));
  p.off = true; quiet(p, 999000); assert(!p.due(999000));
  FramePolicy wrap; wrap.begin(0xffff0000u);
  uint32_t deadline = 0xffff0000u + FramePolicy::IDLE_MS;
  quiet(wrap, deadline); assert(wrap.due(deadline));

  AdPowerGate gate;
  assert(gate.enter()); assert(gate.enter()); assert(!gate.close());
  gate.leave(); assert(!gate.close()); gate.leave(); assert(gate.close());
  assert(!gate.enter()); assert(!gate.busy()); assert(gate.close());
  // Exercise admission racing shutdown: the winner excludes the other path.
  for (int i = 0; i < 500; ++i) {
    AdPowerGate race; bool entered = false;
    std::thread worker([&] { entered = race.enter(); });
    bool closed = race.close(); worker.join();
    assert(entered != closed);
    if (entered) { assert(!race.close()); race.leave(); assert(race.close()); }
    assert(!race.enter());
  }
  puts("Frame policy: NFC rearm/short pulses/idle/save/retry/busy/manual/hold/RF freshness/wrap passed; 500 admission races passed");
}
