#pragma once
class SystemPacketHeader
{
protected:
	int size;
	int type;

public:
	SystemPacketHeader();
	~SystemPacketHeader() = default;

	void SetSize(int size) { this->size = size; }
	int  GetSize() { return size; };

	void SetType(int type) { this->type = type; }
	int  GetType() { return type; }

private:
	void Reset();
};
