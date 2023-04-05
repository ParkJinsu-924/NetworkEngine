#pragma once
#pragma comment(lib, "ws2_32.lib")
#include <iostream>
#include <winsock2.h>
#include <WS2tcpip.h>
#include <vector>
#include <thread>
#include <atomic>
#include <concurrent_unordered_map.h>
#include <concurrent_queue.h>

#include "MemoryPool.h"
#include "ThreadLocalMemoryPool.h"
#include "RingBuffer.h"
#include "Protocol.h"

class SESSION
{
public:
	SESSION()
	: recvQ(RINGBUFFER_SIZE)
	{
	}

	void Reset()
	{
		sessionSocket = 0;
		ZeroMemory(&recvOverlapped, sizeof(recvOverlapped));
		ZeroMemory(&sendOverlapped, sizeof(sendOverlapped));
		recvQ.Reset();
	}

public:
	void ResetRecvOverlapped()
	{
		ZeroMemory(&recvOverlapped, sizeof(recvOverlapped));
	}

	void ResetSendOverlapped()
	{
		ZeroMemory(&sendOverlapped, sizeof(sendOverlapped));
	}

public:
	SOCKET									sessionSocket;
	bool									disconnectFlag;
	OVERLAPPED								recvOverlapped;
	OVERLAPPED								sendOverlapped;
	RingBuffer								recvQ;
	Concurrency::concurrent_queue<MESSAGE*> sendQ;
	Concurrency::concurrent_queue<MESSAGE*> sendPendingQ;
};

class NetClient
{
public:
	NetClient();
	bool Connect(const char* ip, short port, bool tcpNagleOn);
	bool Send(MESSAGE* pMessage);

	MESSAGE* AllocateMessage();
	bool	 FreeMessage(MESSAGE* pMessage);

	virtual void OnRecv(MESSAGE* pMessage) = 0;

private:
	void WorkerThread();
	void SendThread();
	void PostRecv();
	void PostSend();
	void AfterRecvProcess(DWORD transferredBytes);
	void AfterSendProcess();

	void PrintError(int errorcode, int line);

private:
	SESSION m_Session;
	HANDLE  m_hIocp;

	std::vector<std::thread> m_vecWorkerThread;
	std::thread				 m_SendThread;

	ThreadLocalMemoryPool<MESSAGE> m_MessagePool;
};
