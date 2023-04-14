#pragma once
#include <unordered_map>
#include <functional>

class MESSAGE;
class SystemPacketProcessor
{
public:
	template<typename Type>
	bool RegisterProcessor(std::function<void(MESSAGE*)> fProcessor)
	{
		Type packet;

		auto it = m_mapProcessors.find(packet.GetType());
		if (it != m_mapProcessors.end())
			return false;

		m_mapProcessors.insert(std::make_pair(packet.GetType(), [])
	}

private:
	std::unordered_map<int, std::function<void(MESSAGE*)>> m_mapProcessors;
};
