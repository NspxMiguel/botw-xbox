#include "../../runtime/cemu_shims/LatteWordBatch.h"

#include <cassert>
#include <deque>
#include <iostream>
#include <thread>

struct CountingMutex
{
	unsigned locks = 0;
	void lock()
	{
		++locks;
	}
	void unlock()
	{
	}
};

int main()
{
	LatteWordBatch<4> batch;
	using Result = LatteWordBatch<4>::Result;
	std::deque<uint32_t> queue;
	CountingMutex mutex;
	uint32_t word = 99;
	assert(batch.Read(queue, mutex, word) == Result::Empty && word == 99);
	for (uint32_t i = 0; i < 6; ++i)
		queue.push_back(i);
	assert(batch.Read(queue, mutex, word) == Result::Refilled && word == 0);
	const auto locks = mutex.locks;
	queue.push_back(6); // Producer appends while the consumer owns a partial packet.
	for (uint32_t i = 1; i < 4; ++i)
		assert(batch.Read(queue, mutex, word) == Result::Buffered && word == i);
	assert(mutex.locks == locks);
	assert(batch.Read(queue, mutex, word) == Result::Refilled && word == 4);
	assert(batch.Read(queue, mutex, word) == Result::Buffered && word == 5);
	assert(batch.Read(queue, mutex, word) == Result::Buffered && word == 6);
	assert(!batch.HasBufferedWords());
	assert(batch.Read(queue, mutex, word) == Result::Empty);

	// Exercise publication between empty/refill boundaries with real locking.
	LatteWordBatch<> concurrent;
	std::mutex queueMutex;
	std::thread producer([&] {
		for (uint32_t i = 0; i < 100000; ++i)
		{
			std::lock_guard lock(queueMutex);
			queue.push_back(i);
		}
	});
	for (uint32_t i = 0; i < 100000;)
	{
		if (concurrent.Read(queue, queueMutex, word) != LatteWordBatch<>::Result::Empty)
			assert(word == i++);
		else
			std::this_thread::yield();
	}
	producer.join();
	assert(!concurrent.HasBufferedWords() && queue.empty());
	std::cout << "Ring word batch tests passed (FIFO, partial packets, empty/refill, concurrent publication).\n";
}
