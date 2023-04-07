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
		releaseFlag = true;
		ZeroMemory(&recvOverlapped, sizeof(recvOverlapped));
		ZeroMemory(&sendOverlapped, sizeof(sendOverlapped));
		recvQ.Reset();
		ioCount = 0;
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

	bool IsReleased() { return releaseFlag; }
	void SetReleaseState(bool release) { releaseFlag = release; }

public:
	SOCKET									sessionSocket;
	bool									releaseFlag;
	OVERLAPPED								recvOverlapped;
	OVERLAPPED								sendOverlapped;
	RingBuffer								recvQ;
	std::atomic<int>						ioCount;
	std::mutex								lock;
	Concurrency::concurrent_queue<MESSAGE*> sendQ;
	Concurrency::concurrent_queue<MESSAGE*> sendPendingQ;
};

class NetClient
{
public:
	NetClient();
	bool Connect(const char* ip, short port, bool tcpNagleOn);
	bool Send(MESSAGE* pMessage);
	bool Disconnect();

	MESSAGE* AllocateMessage();
	bool	 FreeMessage(MESSAGE* pMessage);

	virtual void OnConnect() = 0;
	virtual void OnRecv(MESSAGE* pMessage) = 0;
	virtual void OnDisconnect() = 0;

private:
	void WorkerThread();
	void SendThread();
	void PostRecv();
	void PostSend();
	void AfterRecvProcess(DWORD transferredBytes);
	void AfterSendProcess();

	void ReleaseSession();
	bool PreventRelease();
	bool UnlockPrevent();

	SESSION& GetSession() { return m_Session; }

	bool Initialize();

	void PrintError(int errorcode, int line);

private:
	SESSION m_Session;
	HANDLE  m_hIocp;
	bool	m_AlreadyInitialized;

	std::vector<std::thread> m_vecWorkerThread;
	std::thread				 m_SendThread;

	ThreadLocalMemoryPool<MESSAGE> m_MessagePool;
};
