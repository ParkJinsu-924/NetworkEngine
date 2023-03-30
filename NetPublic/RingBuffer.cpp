#include "RingBuffer.h"
#include <algorithm>

RingBuffer::RingBuffer(size_t size)
: m_size(size)
, m_head(0)
, m_tail(0)
, m_used(0)
, m_buffer(new char[size])
{
}

RingBuffer::~RingBuffer()
{
	delete[] m_buffer;
}

bool RingBuffer::put(const char* pData, size_t size)
{
	std::unique_lock<std::mutex> lock(m_mutex);

	if (size > free_space())
	{
		return false;
	}

	// copy data to buffer
	size_t copySize = std::min(size, m_size - m_head);
	std::memcpy(&m_buffer[m_head], pData, copySize);
	std::memcpy(m_buffer, pData + copySize, size - copySize);

	m_head = (m_head + size) % m_size;
	m_used += size;

	//m_cond.notify_all();

	return true;
}

bool RingBuffer::peek(char* pBuffer, size_t size)
{
	std::unique_lock<std::mutex> lock(m_mutex);

	if (size > m_used)
	{
		return false;
	}

	// copy data to buffer
	size_t copySize = std::min(size, m_size - m_tail);
	std::memcpy(pBuffer, &m_buffer[m_tail], copySize);
	std::memcpy(pBuffer + copySize, m_buffer, size - copySize);

	return true;
}

void RingBuffer::move_head(size_t size)
{
	std::unique_lock<std::mutex> lock(m_mutex);

	if (size > m_size)
	{
		return;
	}

	m_head = (m_head + size) % m_size;
	m_used += size;
	
	return;
}

bool RingBuffer::move_tail(size_t size)
{
	std::unique_lock<std::mutex> lock(m_mutex);

	if (size > m_used)
	{
		return false;
	}

	m_tail = (m_tail + size) % m_size;
	m_used -= size;

	return true;
}

size_t RingBuffer::direct_enqueue_size()
{
	if (full())
	{
		return 0;
	}

	if (m_head >= m_tail)
	{
		return m_size - m_head;
	}
	return m_tail - m_head;
}

size_t RingBuffer::direct_dequeue_size()
{
	if (empty())
	{
		return 0;
	}

	if (m_tail >= m_head)
	{
		return m_size - m_tail;
	}
	return m_head - m_tail;
}

char* RingBuffer::head_pointer()
{
	return &m_buffer[m_head];
}

char* RingBuffer::tail_pointer()
{
	return &m_buffer[m_tail];
}

char* RingBuffer::start_pointer()
{
	return m_buffer;
}

bool RingBuffer::empty()
{
	std::unique_lock<std::mutex> lock(m_mutex);
	return m_used == 0;
}

bool RingBuffer::full()
{
	std::unique_lock<std::mutex> lock(m_mutex);
	return m_used == m_size;
}

void RingBuffer::Reset()
{
	std::unique_lock<std::mutex> lock(m_mutex);

	m_head = 0;
	m_tail = 0;
	m_used = 0;
}

size_t RingBuffer::free_space() const
{
	return m_size - m_used;
}

size_t RingBuffer::size_in_use() const
{
	return m_used;
}
