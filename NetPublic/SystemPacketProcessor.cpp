#include "SystemPacketProcessor.h"
#include "SystemPacket.h"

bool SystemPacketProcessor::InitializeSystemPacket()
{
	RegisterProcessor<SystemPacket_TestPacket>(std::bind(&SystemPacketProcessor::Process_TestPacket, this, std::placeholders::_1, std::placeholders::_2));

	return true;
}

void SystemPacketProcessor::Process_TestPacket(SESSION* pSession, SystemPacketHeader* pSystemPacket)
{
	if (pSession == nullptr || pSystemPacket == nullptr)
		return;
}
