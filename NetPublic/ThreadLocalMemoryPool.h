#pragma once
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include <queue>
#include <concurrent_unordered_map.h>
#include <concurrent_queue.h>

template <typename T>
class ThreadLocalMemoryPool
{
	struct BLOCK;
	using QUEUE_PTR = Concurrency::concurrent_queue<BLOCK*>*;

private:
	struct BLOCK
	{
		T		  data;
		QUEUE_PTR pQueue;
	};

public:
	ThreadLocalMemoryPool(size_t block_count)
	: block_count_(block_count)
	{
		block_size_ = sizeof(BLOCK);
	}

	T* Allocate()
	{
		BLOCK* block = nullptr;
		auto&  queue = block_queue();

		while (true)
		{
			if (!queue.try_pop(block))
			{
				BLOCK* block_chunk = new (std::nothrow) BLOCK[block_count_];
				if (block_chunk == nullptr)
					return nullptr;

				for (size_t i = 0; i < block_count_; ++i)
				{
					BLOCK* block = block_chunk + i;
					block->pQueue = &queue;
					queue.push(block);
				}
			}
			else
			{
				break;
			}
		}

		return &block->data;
	}

	void Free(T* data)
	{
		if (data == nullptr)
			return;

		BLOCK* block = reinterpret_cast<BLOCK*>(data);
		block->pQueue->push(block);
	}

private:
	size_t block_count_;
	size_t block_size_;

private:
	Concurrency::concurrent_queue<BLOCK*>& block_queue()
	{
		static thread_local Concurrency::concurrent_queue<BLOCK*> queue;
		return queue;
	}
};