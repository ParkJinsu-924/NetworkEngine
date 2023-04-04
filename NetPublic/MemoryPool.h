#pragma once
#include <vector>
#include <mutex>
template <typename T>
class MemoryPool
{
public:
	MemoryPool(size_t maxCount)
	{
		for (size_t i = 0; i < maxCount; ++i)
			m_vecPool.push_back(new (std::nothrow) T);
	}

	~MemoryPool()
	{
		for (auto member : m_vecPool)
			delete member;
	}

public:
	T* Allocate()
	{
		std::lock_guard<std::mutex> lock(m_lock);
		T*							ptr = nullptr;
		if (m_vecPool.size() > 0)
		{
			ptr = m_vecPool.back();
			m_vecPool.pop_back();
		}
		else
		{
			ptr = new T;
		}
		return ptr;
	}

	void Deallocate(T* p)
	{
		std::lock_guard<std::mutex> lock(m_lock);
		m_vecPool.push_back(p);
	}

private:
	std::vector<T*> m_vecPool;
	int				m_iMaxCount = 0;
	std::mutex		m_lock;
};
