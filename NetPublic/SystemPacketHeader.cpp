#include "SystemPacketHeader.h"

SystemPacketHeader::SystemPacketHeader()
{
	Reset();
}

void SystemPacketHeader::Reset()
{
	size = 0;
	type = 0;
}
