#include "NetClient.h"

bool NetClient::Connect(const char* ip, short port, bool tcpNagleOn)
{
	if (ip == nullptr)
		return false;

	WSADATA wsa;
	if (!WSAStartup(MAKEWORD(2, 2), &wsa))
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

	for (int i = 0; i < WORKER_THREAD_CNT; ++i)
	{
		m_vecWorkerThread.push_back(std::thread([this]() { WorkerThread(); }));
	}

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
	return false;
}

void NetClient::PostRecv()
{
	WSABUF recvBuf[2];

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
