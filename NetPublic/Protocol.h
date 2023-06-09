#pragma once
#include <iostream>

static constexpr int MAX_PAYLOAD_SIZE = 65535;

template <typename T>
class ThreadLocalMemoryPool;

enum class PACKET_TYPE : short
{
	SYSTEM,
	USER
};

#pragma pack(1)
struct HEADER
{
	PACKET_TYPE type;
	short		length;
};

class MESSAGE
{
	friend class NetServer;
	friend class NetClient;
	friend class ThreadLocalMemoryPool<MESSAGE>;

private:
	MESSAGE() = default;

public:
	bool put(const void* payload, int size)
	{
		if (header.length + size > MAX_PAYLOAD_SIZE)
			return false;

		std::memcpy(this->payload + header.length, payload, size);
		header.length += size;
		return true;
	}

	char* GetPayload() { return payload; }
	short GetPayloadSize() { return header.length; }
	void  Reset() { header.length = 0; }

private:
	HEADER header;
	char   payload[MAX_PAYLOAD_SIZE];
};
#pragma pack()
