#pragma once
#include "SystemPacketHeader.h"

class SystemPacket_TestPacket : public SystemPacketHeader
{
public:
	SystemPacket_TestPacket();
	int m_iTest;
	int m_iMind;
	int m_iIndex;
};
