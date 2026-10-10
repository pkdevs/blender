/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "GHOST_IOSMessageQueue.hh"
#include <cassert>
#include <chrono>
#include <memory>
#include <thread>

/* The app wakes Blender or UIKit here; this test has neither. */
void GHOST_ios_mailbox_published() {}

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

  struct Edit { int token, value; bool preview; };
  GHOST_IOSMessageQueue<Edit> edits;
  for (int i = 0; i < 1000; ++i) { edits.push({1, i, true}); }
  edits.push({1, 1000, false});
  edits.push({2, 0, false});
  edits.push({1, 1001, true});
  edits.push({1, 1002, true});
  auto reduced = edits.take_coalesced([](const Edit &a, const Edit &b) {
    return a.preview && a.token == b.token;
  });
  assert(reduced.size() == 3);
  assert(reduced[0].value == 1000 && !reduced[0].preview);
  assert(reduced[1].token == 2 && reduced[2].value == 1002);

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
