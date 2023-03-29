#pragma once
#pragma pack(1)
struct HEADER
{
	short length;
};

struct MESSAGE
{
	HEADER header;
	char payload[5000];
};
#pragma pack()
