#pragma once
#include <vector>
#include <condition_variable>
#include <cstring>
#include <mutex>
class RingBuffer
{
public:
	explicit RingBuffer(size_t size);
	~RingBuffer();

	bool   put(const char* pData, size_t size);
	bool   peek(char* pBuffer, size_t size);
	bool   move_tail(size_t size);
	char*  head_pointer();
	char*  tail_pointer();
	char*  start_pointer();
	bool   empty();
	bool   full();
	void   Reset();
	size_t direct_enqueue_size();
	size_t direct_dequeue_size();
	size_t free_space() const;
	size_t size_in_use() const;

private:
	size_t m_size;
	size_t m_head;
	size_t m_tail;
	size_t m_used;
	char*  m_buffer;

	std::mutex				m_mutex;
	std::condition_variable m_cond;
};
