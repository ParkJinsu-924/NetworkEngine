#include "NetClient.h"
#include "GlobalValue.h"

#define PRINT_ERROR() PrintError(WSAGetLastError(), __LINE__);

bool NetClient::Connect(const char* ip, short port, bool tcpNagleOn)
{
	if (ip == nullptr)
		return false;

	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		return false;

	const SOCKET clientSocket = socket(AF_INET, SOCK_STREAM, 0);
	if (clientSocket == INVALID_SOCKET)
		return false;

	m_Session.sessionSocket = clientSocket;

	BOOL flag = true;
	if (setsockopt(clientSocket, SOL_SOCKET, SO_KEEPALIVE, (char*)&flag, sizeof(flag)) == SOCKET_ERROR)
		return false;

	DWORD optval = true;
	if (setsockopt(clientSocket, IPPROTO_TCP, TCP_NODELAY, (char*)&optval, sizeof(optval)) == SOCKET_ERROR)
		return false;

	linger lingerOpt;
	lingerOpt.l_onoff = 1;
	lingerOpt.l_linger = 0;
	if (setsockopt(clientSocket, SOL_SOCKET, SO_LINGER, (char*)&lingerOpt, sizeof(lingerOpt)) == SOCKET_ERROR)
		return false;

	constexpr int WORKER_THREAD_CNT = 4;
	m_hIocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, NULL, WORKER_THREAD_CNT);
	if (m_hIocp == NULL)
		return false;

	//init pool
	m_pMessagePool = new MemoryPool<MESSAGE>(TOTAL_MESSAGE_COUNT_IN_MEMORY_POOL);

	//create thread
	for (int i = 0; i < WORKER_THREAD_CNT; ++i)
	{
		m_vecWorkerThread.push_back(std::thread([this]() { WorkerThread(); }));
	}
	m_SendThread = std::thread([this]() { SendThread(); });

	//connect
	SOCKADDR_IN addr;
	addr.sin_family = AF_INET;
	InetPtonA(AF_INET, ip, &addr.sin_addr);
	addr.sin_port = htons(port);
	if (connect(clientSocket, (SOCKADDR*)&addr, sizeof(addr)) == SOCKET_ERROR)
		return false;

	if (CreateIoCompletionPort((HANDLE)clientSocket, m_hIocp, NULL, NULL) == NULL)
		return false;

	PostRecv();

	return true;
}

bool NetClient::Send(char* pPacket, int size)
{
	if (pPacket == nullptr)
		return false;

	MESSAGE* pMessage = m_pMessagePool->Allocate();
	if (pMessage == nullptr)
		return false;

	pMessage->header.length = size;
	memcpy(&pMessage->payload, pPacket, size);

	m_Session.sendQ.push(pMessage);

	return true;
}

void NetClient::WorkerThread()
{
	while (true)
	{
		OVERLAPPED* pOverlapped = nullptr;
		DWORD		transferredBytes = 0;
		SESSION*	pSession = nullptr;

		BOOL success = GetQueuedCompletionStatus(m_hIocp, &transferredBytes, (PULONG_PTR)&pSession, &pOverlapped, INFINITE);
		if (!success || pOverlapped == nullptr)
		{
			// disconnected
			break;
		}

		//error_operation_aborted for CancelIo
		if (transferredBytes == 0 || pOverlapped->Internal == ERROR_OPERATION_ABORTED)
		{
			// release
			break;
		}

		if (&m_Session.recvOverlapped == pOverlapped)
		{
			AfterRecvProcess(transferredBytes);
		}
		else if (&m_Session.sendOverlapped == pOverlapped)
		{
			AfterSendProcess();
		}
	}
}

void NetClient::SendThread()
{
	while (true)
	{
		Sleep(1);

		PostSend();
	}
}

void NetClient::PostRecv()
{
	RingBuffer& recvQ = m_Session.recvQ;

	int freeSize = recvQ.free_space();
	int directEnqueueSize = recvQ.direct_enqueue_size();

	int	bufCount = 1;
	WSABUF recvBuf[2];
	recvBuf[0].buf = recvQ.head_pointer();
	recvBuf[0].len = directEnqueueSize;
	if (directEnqueueSize < freeSize)
	{
		recvBuf[1].buf = recvQ.start_pointer();
		recvBuf[1].len = freeSize - directEnqueueSize;
		++bufCount;
	}

	m_Session.ResetRecvOverlapped();

	DWORD flags = 0;
	int   result = WSARecv(m_Session.sessionSocket, recvBuf, bufCount, nullptr, &flags, &m_Session.recvOverlapped, nullptr);
	if (result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING)
	{
		//need to release sesion
	}
}

void NetClient::PostSend()
{
	//when sendPendingQ is not empty, send Process is not over
	if (!m_Session.sendPendingQ.empty() || m_Session.sendQ.empty())
		return;

	constexpr int MAX_HOLD_MESSAGE = 128;
	WSABUF		  sendBuf[MAX_HOLD_MESSAGE];

	int		 wsaBufIdx = 0;
	MESSAGE* pMessage = nullptr;
	while (m_Session.sendQ.try_pop(pMessage))
	{
		if (pMessage == nullptr)
		{
			//Call Release Session
			return;
		}

		sendBuf[wsaBufIdx].buf = (char*)&pMessage->header;
		sendBuf[wsaBufIdx].len = sizeof(pMessage->header) + pMessage->header.length;

		++wsaBufIdx;

		m_Session.sendPendingQ.push(pMessage);
	}

	m_Session.ResetSendOverlapped();

	DWORD flags = 0;
	int   result = WSASend(m_Session.sessionSocket, sendBuf, wsaBufIdx, nullptr, flags, &m_Session.sendOverlapped, nullptr);
	if (result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING)
	{
		PRINT_ERROR();
		//release session
	}
}

void NetClient::AfterRecvProcess(DWORD transferredBytes)
{
	RingBuffer& recvQ = m_Session.recvQ;
	recvQ.move_head(transferredBytes);

	while (true)
	{
		HEADER header;

		const size_t headerSize = sizeof(header);
		const size_t useSize = recvQ.size_in_use();
		if (useSize <= sizeof(header))
			break;

		if (recvQ.peek((char*)&header, headerSize) == false)
		{
			PRINT_ERROR();
			break;
		}

		if (header.length >= RINGBUFFER_SIZE - headerSize)
		{
			PRINT_ERROR();
			break;
		}

		if ((short)(useSize - headerSize) < header.length)
			break;

		//need to change
		/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
		char* pBuffer = new char[header.length]; // MemoryPoolTLS::Alloc(header.length)
		ZeroMemory(pBuffer, header.length);
		recvQ.move_tail(headerSize);
		recvQ.peek((char*)pBuffer, header.length);
		recvQ.move_tail(header.length);

		OnRecv(pBuffer, header.length);

		delete[] pBuffer;
		/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	}

	PostRecv();
}

void NetClient::AfterSendProcess()
{
	MESSAGE* pMessage = nullptr;
	while (m_Session.sendPendingQ.try_pop(pMessage))
	{
		if (pMessage == nullptr)
			continue;

		m_pMessagePool->Deallocate(pMessage);
	}
}

void NetClient::PrintError(int errorcode, int line)
{
	std::cout << "ERROR : Need to release : ErrorCode : " << errorcode << " : LINE : " << line << std::endl;
}
