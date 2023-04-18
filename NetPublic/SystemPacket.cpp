#include "SystemPacketType.h"
#include "SystemPacket.h"

SystemPacket_TestPacket::SystemPacket_TestPacket()
{
	SetType(ePacketType_Test);
	SetSize(sizeof(SystemPacket_TestPacket));
}
