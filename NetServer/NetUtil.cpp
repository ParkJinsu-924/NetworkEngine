#include <iostream>
#include "NetUtil.h"

void NetUtil::PrintError(int errorcode, int line)
{
	std::cout << "ERROR : Need to release : ErrorCode : " << errorcode << " : LINE : " << line << std::endl;
}

long long NetUtil::MakeSessionUID(int sessionIdx, int sessionId)
{
	long long sessionUID = 0;
	sessionUID = sessionIdx;
	sessionUID = sessionUID << 32;
	sessionUID += sessionId;
	return sessionUID;
}

int NetUtil::GetSessionIndexPart(long long sessionUID)
{
	return sessionUID >> 32;
}
