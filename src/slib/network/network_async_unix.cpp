/*
 *   Copyright (c) 2008-2018 SLIBIO <https://github.com/SLIBIO>
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in
 *   all copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 *   THE SOFTWARE.
 */

#include "slib/network/definition.h"

#if defined(SLIB_PLATFORM_IS_UNIX)

#include "network_async.h"

namespace slib
{
	
	namespace priv
	{
		namespace network_async
		{
			
			class AsyncTcpSocketInstanceImpl : public AsyncTcpSocketInstance
			{
			public:
				AtomicRef<AsyncStreamRequest> m_requestReading;
				AtomicRef<AsyncStreamRequest> m_requestWriting;
				sl_uint32 m_sizeWritten;
				
				sl_bool m_flagConnecting;
				
			public:
				AsyncTcpSocketInstanceImpl()
				{
					m_sizeWritten = 0;
					m_flagConnecting = sl_false;
				}
				
				~AsyncTcpSocketInstanceImpl()
				{
					close();
				}
				
			public:
				static Ref<AsyncTcpSocketInstanceImpl> create(const Ref<Socket>& socket)
				{
					if (socket.isNotNull()) {
						if (socket->setNonBlockingMode(sl_true)) {
							sl_file handle = (sl_file)(socket->getHandle());
							if (handle != SLIB_FILE_INVALID_HANDLE) {
								Ref<AsyncTcpSocketInstanceImpl> ret = new AsyncTcpSocketInstanceImpl();
								if (ret.isNotNull()) {
									ret->m_socket = socket;
									ret->setHandle(handle);
									return ret;
								}
							}
						}
					}
					return sl_null;
				}
				
				void close() override
				{
					AsyncTcpSocketInstance::close();
					setHandle(SLIB_FILE_INVALID_HANDLE);
					m_socket.setNull();
				}
				
				void processRead(sl_bool flagError)
				{
					Ref<Socket> socket = m_socket;
					if (socket.isNull()) {
						return;
					}
					Ref<AsyncStreamRequest> request = m_requestReading;
					m_requestReading.setNull();
					sl_size nQueue = getReadRequestsCount();
					Ref<Thread> thread = Thread::getCurrent();
					while (thread.isNull() || thread->isNotStopping()) {
						if (request.isNull()) {
							if (nQueue > 0) {
								nQueue--;
								popReadRequest(request);
								if (request.isNull()) {
									return;
								}
							} else {
								return;
							}
						}
						if (request->data && request->size) {
							sl_int32 n = socket->receive((char*)(request->data), request->size);
							if (n > 0) {
								_onReceive(request.get(), n, flagError);
							} else if (n < 0) {
								_onReceive(request.get(), 0, sl_true);
								return;
							} else {
								if (flagError) {
									_onReceive(request.get(), 0, sl_true);
								} else {
									m_requestReading = request;
								}
								return;
							}
						} else {
							_onReceive(request.get(), request->size, sl_false);
						}
						request.setNull();
					}
				}
				
				void processWrite(sl_bool flagError)
				{
					Ref<Socket> socket = m_socket;
					if (socket.isNull()) {
						return;
					}
					Ref<AsyncStreamRequest> request = m_requestWriting;
					m_requestWriting.setNull();
					sl_size nQueue = getWriteRequestsCount();
					Ref<Thread> thread = Thread::getCurrent();
					while (thread.isNull() || thread->isNotStopping()) {
						if (request.isNull()) {
							if (nQueue > 0) {
								nQueue--;
								popWriteRequest(request);
								if (request.isNull()) {
									return;
								} else {
									m_sizeWritten = 0;
								}
							} else {
								return;
							}
						}
						if (request->data && request->size) {
							sl_uint32 size = request->size - m_sizeWritten;
							sl_int32 n = socket->send((char*)(request->data) + m_sizeWritten, size);
							if (n > 0) {
								m_sizeWritten += n;
								if (m_sizeWritten >= request->size) {
									_onSend(request.get(), request->size, flagError);
								} else {
									m_requestWriting = request;
								}
							} else if (n < 0) {
								_onSend(request.get(), m_sizeWritten, sl_true);
								return;
							} else {
								if (flagError) {
									_onSend(request.get(), m_sizeWritten, sl_true);
								} else {
									m_requestWriting = request;
								}
								return;
							}
						} else {
							_onSend(request.get(), request->size, sl_false);
						}
						request.setNull();
					}
				}
				
				void onOrder() override
				{
					Ref<Socket> socket = m_socket;
					if (socket.isNull()) {
						return;
					}
					if (m_flagConnecting) {
						return;
					}
					if (m_flagRequestConnect) {
						m_flagRequestConnect = sl_false;
						if (socket->connect(m_addressRequestConnect)) {
							m_flagConnecting = sl_true;
						} else {
							_onConnect(sl_true);
						}
						return;
					}
					processRead(sl_false);
					processWrite(sl_false);
				}
				
				void onEvent(EventDesc* pev) override
				{
					sl_bool flagProcessed = sl_false;
					if (pev->flagIn) {
						processRead(pev->flagError);
						flagProcessed = sl_true;
					}
					if (pev->flagOut) {
						if (m_flagConnecting) {
							m_flagConnecting = sl_false;
							if (pev->flagError) {
								_onConnect(sl_true);
							} else {
								_onConnect(sl_false);
							}
						} else {
							processWrite(pev->flagError);
						}
						flagProcessed = sl_true;
					}
					if (!flagProcessed) {
						if (pev->flagError) {
							if (m_flagConnecting) {
								m_flagConnecting = sl_false;
								_onConnect(sl_true);
							} else {
								processRead(sl_true);
								processWrite(sl_true);
							}
						}
					}
					requestOrder();
				}
			};

			class AsyncTcpServerInstanceImpl : public AsyncTcpServerInstance
			{
			public:
				sl_bool m_flagListening;
				
			public:
				AsyncTcpServerInstanceImpl()
				{
					m_flagListening = sl_false;
				}
				
				~AsyncTcpServerInstanceImpl()
				{
					close();
				}
				
			public:
				static Ref<AsyncTcpServerInstanceImpl> create(const Ref<Socket>& socket)
				{
					if (socket.isNotNull()) {
						if (socket->setNonBlockingMode(sl_true)) {
							sl_file handle = (sl_file)(socket->getHandle());
							if (handle != SLIB_FILE_INVALID_HANDLE) {
								Ref<AsyncTcpServerInstanceImpl> ret = new AsyncTcpServerInstanceImpl();
								if (ret.isNotNull()) {
									ret->m_socket = socket;
									ret->setHandle(handle);
									return ret;
								}
							}
						}
					}
					return sl_null;
				}
				
				void close() override
				{
					AsyncTcpServerInstance::close();
					m_socket.setNull();
					setHandle(SLIB_FILE_INVALID_HANDLE);
				}
				
				void onOrder() override
				{
					Ref<Socket> socket = m_socket;
					if (socket.isNull()) {
						return;
					}
					Ref<Thread> thread = Thread::getCurrent();
					while (thread.isNull() || thread->isNotStopping()) {
						Ref<Socket> socketAccept;
						SocketAddress addr;
						if (socket->accept(socketAccept, addr)) {
							_onAccept(socketAccept, addr);
						} else {
							SocketError err = socket->getLastError();
							if (err != SocketError::WouldBlock) {
								_onError();
							}
							return;
						}
					}
				}
				
				void onEvent(EventDesc* pev) override
				{
					if (pev->flagIn) {
						onOrder();
					}
					if (pev->flagError) {
						_onError();
					}
				}
			};
			
			class AsyncUdpSocketInstanceImpl : public AsyncUdpSocketInstance
			{
			public:
				AsyncUdpSocketInstanceImpl()
				{
				}
				
				~AsyncUdpSocketInstanceImpl()
				{
					close();
				}
				
			public:
				static Ref<AsyncUdpSocketInstanceImpl> create(const Ref<Socket>& socket, const Memory& buffer)
				{
					if (socket.isNotNull()) {
						if (socket->setNonBlockingMode(sl_true)) {
							sl_file handle = (sl_file)(socket->getHandle());
							if (handle != SLIB_FILE_INVALID_HANDLE) {
								Ref<AsyncUdpSocketInstanceImpl> ret = new AsyncUdpSocketInstanceImpl();
								if (ret.isNotNull()) {
									ret->m_socket = socket;
									ret->setHandle(handle);
									ret->m_buffer = buffer;
									return ret;
								}
							}
						}
					}
					return sl_null;
				}
				
				void close() override
				{
					AsyncUdpSocketInstance::close();
					setHandle(SLIB_FILE_INVALID_HANDLE);
					m_socket.setNull();
				}
				
				void onOrder() override
				{
					processReceive();
				}
				
				void onEvent(EventDesc* pev) override
				{
					if (pev->flagIn) {
						processReceive();
					}
				}
				
				void processReceive()
				{
					Ref<Socket> socket = m_socket;
					if (socket.isNull()) {
						return;
					}
					if (!(socket->isOpened())) {
						return;
					}
					void* buf = m_buffer.getData();
					sl_uint32 sizeBuf = (sl_uint32)(m_buffer.getSize());
					
					Ref<Thread> thread = Thread::getCurrent();
					while (thread.isNull() || thread->isNotStopping()) {
						SocketAddress addr;
						sl_int32 n = socket->receiveFrom(addr, buf, sizeBuf);
						if (n > 0) {
							_onReceive(addr, n);
						} else {
							if (n < 0) {
								_onError();
							}
							break;
						}
					}
				}
				
			};


		}
	}

	Ref<AsyncTcpSocketInstance> AsyncTcpSocket::_createInstance(const Ref<Socket>& socket)
	{
		return priv::network_async::AsyncTcpSocketInstanceImpl::create(socket);
	}

	Ref<AsyncTcpServerInstance> AsyncTcpServer::_createInstance(const Ref<Socket>& socket)
	{
		return priv::network_async::AsyncTcpServerInstanceImpl::create(socket);
	}

	Ref<AsyncUdpSocketInstance> AsyncUdpSocket::_createInstance(const Ref<Socket>& socket, sl_uint32 packetSize)
	{
		Memory buffer = Memory::create(packetSize);
		if (buffer.isNotNull()) {
			return priv::network_async::AsyncUdpSocketInstanceImpl::create(socket, buffer);
		}
		return sl_null;
	}
	
}

#endif
