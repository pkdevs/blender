/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "GHOST_IOSMessageQueue.hh"
#include <cassert>
#include <chrono>
#include <memory>
#include <thread>

struct Message {
  int producer, sequence;
  std::shared_ptr<std::atomic<bool>> alive;
};

int main()
{
  GHOST_IOSMessageQueue<Message> queue;
  auto lifetime = std::make_shared<std::atomic<bool>>(true);
  constexpr int producers = 4, count = 20000;
  std::atomic<int> ready{0};
  std::vector<std::thread> threads;
  for (int p = 0; p < producers; p++) {
    threads.emplace_back([&, p] {
      ready++;
      for (int i = 0; i < count; i++) {
        queue.push({p, i, lifetime});
      }
    });
  }
  while (ready != producers) {
    std::this_thread::yield();
  }
  /* Stalling the engine must not stop producers. */
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  int last[producers] = {-1, -1, -1, -1};
  int received = 0;
  while (received < producers * count) {
    for (auto &message : queue.take()) {
      assert(message.sequence == ++last[message.producer]);
      assert(message.alive->load());
      received++;
    }
  }
  for (auto &thread : threads) {
    thread.join();
  }
  assert(queue.take().empty());

  queue.push({0, 0, lifetime});
  lifetime->store(false);
  auto replacement = std::make_shared<std::atomic<bool>>(true);
  queue.push({0, 1, replacement});
  auto events = queue.take();
  assert(!events[0].alive->load() && events[1].alive->load());

  GHOST_IOSLatest<std::unique_ptr<int>> latest;
  std::atomic<bool> done{false};
  std::thread publisher([&] {
    for (int i = 0; i < 100000; i++) {
      latest.publish(std::make_unique<int>(i));
    }
    done = true;
  });
  int previous = -1;
  std::unique_ptr<int> value;
  while (!done || previous != 99999) {
    if (latest.take(value)) {
      assert(*value > previous);
      previous = *value;
    }
  }
  publisher.join();
  assert(!latest.take(value));
}
