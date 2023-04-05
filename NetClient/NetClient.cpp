#include "NetClient.h"
#include "GlobalValue.h"

#define PRINT_ERROR() PrintError(WSAGetLastError(), __LINE__);

NetClient::NetClient()
: m_MessagePool(3000)
{
}

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

	// create thread
	for (int i = 0; i < WORKER_THREAD_CNT; ++i)
	{
		m_vecWorkerThread.push_back(std::thread([this]()
												{ WorkerThread(); }));
	}
	m_SendThread = std::thread([this]()
							   { SendThread(); });

	// connect
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

bool NetClient::Send(MESSAGE* pMessage)
{
	if (pMessage == nullptr)
		return false;

	m_Session.sendQ.push(pMessage);
	return true;
}

MESSAGE* NetClient::AllocateMessage()
{
	MESSAGE* pMessage = m_MessagePool.Allocate();
	if (pMessage == nullptr)
		return nullptr;

	pMessage->Reset();

	return pMessage;
}

bool NetClient::FreeMessage(MESSAGE* pMessage)
{
	if (pMessage == nullptr)
		return false;

	m_MessagePool.Free(pMessage);
	return true;
}

void NetClient::WorkerThread()
{
	while (true)
	{
		OVERLAPPED* pOverlapped = nullptr;
		DWORD		transferredBytes = 0;
		SESSION*	pSession = nullptr;

		GetQueuedCompletionStatus(m_hIocp, &transferredBytes, (PULONG_PTR)&pSession, &pOverlapped, INFINITE);
		if (pOverlapped == nullptr)
		{
			PostQueuedCompletionStatus(m_hIocp, NULL, NULL, NULL);
			break;
		}

		// error_operation_aborted for CancelIo
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

	int freeSize = (int)recvQ.free_space();
	int directEnqueueSize = (int)recvQ.direct_enqueue_size();

	int	   bufCount = 1;
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
	int	  result = WSARecv(m_Session.sessionSocket, recvBuf, bufCount, nullptr, &flags, &m_Session.recvOverlapped, nullptr);
	if (result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING)
	{
		// need to release sesion
	}
}

void NetClient::PostSend()
{
	// when sendPendingQ is not empty, send Process is not over
	if (!m_Session.sendPendingQ.empty() || m_Session.sendQ.empty())
		return;

	WSABUF sendBuf[MAX_WSABUF_SIZE];

	int		 wsaBufIdx = 0;
	MESSAGE* pMessage = nullptr;
	while (m_Session.sendQ.try_pop(pMessage))
	{
		if (pMessage == nullptr)
		{
			// Call Release Session
			return;
		}

		sendBuf[wsaBufIdx].buf = (char*)pMessage;
		sendBuf[wsaBufIdx].len = sizeof(pMessage->header) + pMessage->header.length;

		++wsaBufIdx;

		m_Session.sendPendingQ.push(pMessage);

		if (wsaBufIdx >= MAX_WSABUF_SIZE)
			break;
	}

	m_Session.ResetSendOverlapped();

	DWORD flags = 0;
	int	  result = WSASend(m_Session.sessionSocket, sendBuf, wsaBufIdx, nullptr, flags, &m_Session.sendOverlapped, nullptr);
	if (result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING)
	{
		PRINT_ERROR();
		// release session
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

		MESSAGE* pMessage = AllocateMessage();
		if (pMessage == nullptr)
			return;

		recvQ.peek((char*)pMessage, headerSize + header.length);
		recvQ.move_tail(headerSize + header.length);

		OnRecv(pMessage);
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

		FreeMessage(pMessage);
	}
}

void NetClient::PrintError(int errorcode, int line)
{
	std::cout << "ERROR : Need to release : ErrorCode : " << errorcode << " : LINE : " << line << std::endl;
}

class TestClient : public NetClient
{
	void OnRecv(MESSAGE* pMessage)
	{
		std::cout << pMessage->GetPayload() << std::endl;
		FreeMessage(pMessage);
	}
};

TestClient nl;

void SendingThread()
{
	const char* payload = "abcdefghijkmlnopqrstuvwxyz1234567890";
	while (true)
	{
		MESSAGE* pMessage = nl.AllocateMessage();
		pMessage->put(payload, rand() % 37);
		pMessage->put("\0", 1);

		nl.Send(pMessage);
		Sleep(10);
	}
}

int main()
{
	bool f = nl.Connect("127.0.0.1", 27931, false);
	if (f == false)
	{
		std::cout << "sdfsdf" << std::endl;
	}

	std::thread th1([]()
					{ SendingThread(); });
	std::thread th2([]()
					{ SendingThread(); });
	std::thread th3([]()
					{ SendingThread(); });
	std::thread th4([]()
					{ SendingThread(); });

	Sleep(INFINITE);
}
