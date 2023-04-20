#pragma once
#include <unordered_map>
#include <functional>
#include "Protocol.h"
#include "SystemPacketHeader.h"

class SESSION;
class SystemPacketProcessor
{
public:
	template <typename Type>
	bool RegisterProcessor(std::function<void(SESSION*, SystemPacketHeader*)> fProcessor)
	{
		Type packet;

		auto it = m_mapProcessors.find(packet.GetType());
		if (it != m_mapProcessors.end())
			return false;

		m_mapProcessors.insert(std::make_pair(packet.GetType(), [=](SESSION* pSession, SystemPacketHeader* pPacket) {
			fProcessor(pSession, pPacket);
		}));
	}

	bool RunProcessor(SESSION* pSession, MESSAGE* pMessage)
	{
		if (pSession == nullptr || pMessage == nullptr)
			return false;

		char* pPayload = pMessage->GetPayload();
		if (pPayload == nullptr)
			return false;

		SystemPacketHeader* pSystemPacket = reinterpret_cast<SystemPacketHeader*>(pPayload);

		auto it = m_mapProcessors.find(pSystemPacket->GetType());
		if (it == m_mapProcessors.end())
			return false;

		auto processor = it->second;
		processor(pSession, pSystemPacket);

		return true;
	}

private:
	std::unordered_map<int, std::function<void(SESSION*, SystemPacketHeader*)>> m_mapProcessors;
};
