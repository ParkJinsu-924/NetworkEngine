#pragma once
#include <iostream>

static constexpr int MAX_PAYLOAD_SIZE = 65535;

template<typename T>
class ThreadLocalMemoryPool;

template<typename T>
class MemoryPool;

#pragma pack(1)
struct HEADER
{
	short length;
};

class MESSAGE
{
	friend class NetServer;
	friend class ThreadLocalMemoryPool<MESSAGE>;
	friend class MemoryPool<MESSAGE>;

private:
	MESSAGE() = default;

public:
	bool put(const void* payload, int size)
	{
		if (header.length + size > MAX_PAYLOAD_SIZE)
			return false;

		std::memcpy(this->payload + header.length, payload, size);
		header.length += size;
	}

	char* GetPayload()
	{
		return payload;	
	}

	void Reset()
	{
		header.length = 0;
	}

private:
	HEADER header;
	char   payload[MAX_PAYLOAD_SIZE];
};
#pragma pack()
