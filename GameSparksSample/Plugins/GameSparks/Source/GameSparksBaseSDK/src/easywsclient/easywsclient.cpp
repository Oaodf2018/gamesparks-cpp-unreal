#include <GameSparks/GSPlatformDeduction.h>

#if !(((GS_TARGET_PLATFORM == GS_PLATFORM_WIN32) && !GS_WINDOWS_DESKTOP) || GS_TARGET_PLATFORM == GS_PLATFORM_XBOXONE)

#include <easywsclient/easywsclient.hpp>

#if (GS_TARGET_PLATFORM == GS_PLATFORM_WIN32 || GS_TARGET_PLATFORM == GS_PLATFORM_PS4 || GS_TARGET_PLATFORM == GS_PLATFORM_ANDROID)
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/net.h>
#include <mbedtls/error.h>
#include <mbedtls/platform.h>
#include <mbedtls/debug.h>
#elif (GS_TARGET_PLATFORM == GS_PLATFORM_NINTENDO_SDK)
#include <nn/os.h>
#include <nn/init.h>
#include <nn/nn_Log.h>
#include <nn/nn_Assert.h>
#include <nn/lmem/lmem_ExpHeap.h>
#include <nn/nifm.h>
#include <nn/nifm/nifm_ApiIpAddress.h>
#include <nn/socket.h>
#include <nn/ssl.h>
#endif
//#include <cassert>
//#include <cstdio>
//#include <iostream>

#include <GameSparks/GSLeakDetector.h>
#include <GameSparks/gsstl.h>

//test
#include <GameSparks/GSUtil.h>
//#include <iostream>
//#include <string.h>

#if defined(WIN32) && !defined(snprintf)
#   define snprintf _snprintf_s
#endif

// use std::thread in MSVC11 (2012) or newer
#if (((defined(_MSC_VER) && _MSC_VER >= 1700) || __cplusplus >= 201103L) && !defined(IW_SDK))
//#	include <thread>
//#	include <mutex>
#	define USE_STD_THREAD 1
#else
#	include <pthread.h>    /* POSIX Threads */
#	undef USE_STD_THREAD
#endif /* WIN32 */

namespace { // private module-only namespace

	namespace threading
	{
		typedef void *(*start_routine) (void *);

#ifdef USE_STD_THREAD
			typedef gsstl::mutex mutex;

			void mutex_init(mutex&)
			{
				// nothing to do here
			}

			void mutex_lock(mutex& mtx)
			{
				mtx.lock();
			}

			void mutex_unlock(mutex& mtx)
			{
				mtx.unlock();
			}

			typedef gsstl::thread thread;

			void thread_create(thread& t, start_routine f, void* arg)
			{
				t = thread(f, arg);
			}

			void thread_exit(thread&)
			{
				// nothing to do
			}

			void thread_join(thread& t)
			{
				t.join();
			}

			bool thread_is_joinable(const thread& t)
			{
				return t.joinable();
			}
#else
			typedef pthread_mutex_t mutex;

			void mutex_init(mutex& mutex)
			{
				pthread_mutex_init(&mutex, NULL);
			}

			void mutex_lock(mutex& mutex)
			{
				pthread_mutex_lock(&mutex);
			}

			void mutex_unlock(mutex& mutex)
			{
				pthread_mutex_unlock(&mutex);
			}

			typedef pthread_t thread;

			void thread_create(thread& thread, start_routine f, void* arg)
			{
				pthread_create(&thread, NULL, f, arg);		
			}

			void thread_exit(thread&)
			{
				pthread_exit(0);
			}

			void thread_join(thread& t)
			{
				if ( t != 0 )
				{
					pthread_join(t, NULL);
					t = 0;
				}
			}

			bool thread_is_joinable(const thread& t)
			{
				return t != 0;
			}
#endif
	}

	class _RealWebSocket : public easywsclient::WebSocket
	{
	public:
		// http://tools.ietf.org/html/rfc6455#section-5.2  Base Framing Protocol
		//
		//  0                   1                   2                   3
		//  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
		// +-+-+-+-+-------+-+-------------+-------------------------------+
		// |F|R|R|R| opcode|M| Payload len |    Extended payload length    |
		// |I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
		// |N|V|V|V|       |S|             |   (if payload len==126/127)   |
		// | |1|2|3|       |K|             |                               |
		// +-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
		// |     Extended payload length continued, if payload len == 127  |
		// + - - - - - - - - - - - - - - - +-------------------------------+
		// |                               |Masking-key, if MASK set to 1  |
		// +-------------------------------+-------------------------------+
		// | Masking-key (continued)       |          Payload Data         |
		// +-------------------------------- - - - - - - - - - - - - - - - +
		// :                     Payload Data continued ...                :
		// + - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - +
		// |                     Payload Data continued ...                |
		// +---------------------------------------------------------------+
		struct wsheader_type {
			unsigned header_size;
			bool fin;
			bool mask;
			enum opcode_type {
				CONTINUATION = 0x0,
				TEXT_FRAME = 0x1,
				BINARY_FRAME = 0x2,
				CLOSE = 8,
				PING = 9,
				PONG = 0xa
			} opcode;
			int N0;
			uint64_t N;
			uint8_t masking_key[4];
		};

		gsstl::vector<char> rxbuf;
		gsstl::vector<char> txbuf;

		volatile readyStateValues readyState;
        bool useMask;
		BaseSocket* socket;

#if !((GS_TARGET_PLATFORM == GS_PLATFORM_IOS || GS_TARGET_PLATFORM == GS_PLATFORM_MAC) && defined(__UNREAL__))
        threading::thread dns_thread;
        threading::mutex lock;
#endif

        _RealWebSocket(gsstl::string host, gsstl::string path, int port, gsstl::string url, gsstl::string origin, bool _useMask, bool _useSSL)
        {
            m_host = host;
            m_path = path;
            m_port = port;
            m_url = url;
            m_origin = origin;
            
            useMask = _useMask;

			socket = BaseSocket::create(_useSSL);       

            readyState = CONNECTING;
            ipLookup = keNone;
        }
        
        virtual ~_RealWebSocket()
        {
#if ((GS_TARGET_PLATFORM == GS_PLATFORM_IOS || GS_TARGET_PLATFORM == GS_PLATFORM_MAC) && defined(__UNREAL__))
            socket->abort();
            delete socket;
#else
			if (threading::thread_is_joinable(dns_thread))
			{
                socket->abort();
				threading::thread_join(dns_thread);
				delete socket;
			}
#endif
        }

		readyStateValues getReadyState() const {
			return readyState;
		}
       
		void poll(int timeout, WSErrorCallback errorCallback, void* userData)  // timeout in milliseconds
        {
			GS_CODE_TIMING_ASSERT();

        	(void)timeout; //unused

			using namespace easywsclient;

            if(readyState == CONNECTING)
            {
                if(ipLookup == keComplete)
                {
#if ((GS_TARGET_PLATFORM == GS_PLATFORM_IOS || GS_TARGET_PLATFORM == GS_PLATFORM_MAC) && defined(__UNREAL__))
                    assert(socket);
                    GS_CODE_TIMING_ASSERT();
                    // establish the ssl connection and do websocket handshaking
                    if (!socket->connect(m_host.c_str(), static_cast<short>(m_port)) || !doConnect2(errorCallback, userData))
                    {
                        GS_CODE_TIMING_ASSERT();
                        forceClose();
                    }
                    else
                    {
                        readyState = OPEN;
                    }
#else
					// join the dns_thread
					threading::thread_join(dns_thread);

                    #if defined(IW_SDK) // on marmalade, we're doing the TLS-Handshake blocking to avoid multithreaded memory management issues
					assert(socket);
					GS_CODE_TIMING_ASSERT();
					// establish the ssl connection and do websocket handshaking
					if (!socket->connect(m_host.c_str(), static_cast<short>(m_port)) || !doConnect2(errorCallback, userData))
                    {
						GS_CODE_TIMING_ASSERT();
                        forceClose();
                    }
                    else
                    {
                        readyState = OPEN;
                    }
					#else
					readyState = OPEN;
					#endif
#endif
                }
                else if( ipLookup == keFailed )
                {
#if !((GS_TARGET_PLATFORM == GS_PLATFORM_IOS || GS_TARGET_PLATFORM == GS_PLATFORM_MAC) && defined(__UNREAL__))
					threading::thread_join(dns_thread);
#endif
                    forceClose();
					using namespace easywsclient;

                    assert(ipLookupError.code != WSError::ALL_OK);
                    errorCallback(ipLookupError, userData);
                    ipLookupError = WSError();
                }
            }
            else if(ipLookup == keComplete)
            {           
				assert(timeout == 0); // not implemented yet: use mbedtls_net_recv_timeout et. all.

                if (readyState == CLOSED)
                {
                    return;
                }

				using namespace easywsclient;

                for(;;) // while(true), but without a warning about constant expression
                {
                    // FD_ISSET(0, &rfds) will be true
                    gsstl::vector<char>::size_type N = rxbuf.size();
					rxbuf.resize(N + 1500);

					assert(socket);

					int ret = socket->recv(&rxbuf[0] + N, 1500);

#if (GS_TARGET_PLATFORM == GS_PLATFORM_NINTENDO_SDK || GS_TARGET_PLATFORM == GS_PLATFORM_IOS || GS_TARGET_PLATFORM == GS_PLATFORM_MAC)
					if (ret < 0)
#else
                    if (ret < 0 && (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE))
#endif
					{
                        rxbuf.resize(N);
                        break;
                    }
                    else if (ret <= 0)
                    {
                        rxbuf.resize(N);
						socket->close();
                        readyState = CLOSED;
						if (ret < 0)
						{
							fputs("Connection error!\n", stderr);
							errorCallback(WSError(WSError::RECV_FAILED, "recv or SSL_read failed"), userData);
						}
						else
						{
							fputs("Connection closed!\n", stderr);
							errorCallback(WSError(WSError::CONNECTION_CLOSED, "Connection closed"), userData);
						}
                        break;
                    }
                    else
                    {
                        rxbuf.resize(static_cast<gsstl::vector<char>::size_type>(N + ret));
                    }
                }

				while (txbuf.size())
                {
					assert(socket);

					int ret = socket->send(&txbuf[0], txbuf.size());

#if (GS_TARGET_PLATFORM == GS_PLATFORM_NINTENDO_SDK || GS_TARGET_PLATFORM == GS_PLATFORM_IOS || GS_TARGET_PLATFORM == GS_PLATFORM_MAC)
					if (ret < 0)
#else
					if (ret < 0 && (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE))
#endif     
					{
                        break;
                    }
                    else if (ret <= 0)
                    {
						socket->close();
                        readyState = CLOSED;
						if (ret < 0)
						{
							fputs("Connection error!\n", stderr);
							errorCallback(WSError(WSError::SEND_FAILED, "send or SSL_write failed"), userData);
						}
						else
						{
							fputs("Connection closed!\n", stderr);
							errorCallback(WSError(WSError::CONNECTION_CLOSED, "Connection closed"), userData);
						}
                        break;
                    }
                    else
                    {
                        assert(ret <= (int)txbuf.size());
                        txbuf.erase(txbuf.begin(), txbuf.begin() + ret);
                    }
                }
            }
            
            if (!txbuf.size() && readyState == CLOSING)
            {
				socket->close();
                readyState = CLOSED;
				errorCallback(WSError(WSError::CONNECTION_CLOSED, "Connection closed"), userData);
            }
		}

		// Callable must have signature: void(const gsstl::string & message).
		// Should work with C functions, C++ functors, and C++11 std::function and
		// lambda:
		//template<class Callable>
		//void dispatch(Callable callable)
		virtual void _dispatch(WSMessageCallback messageCallback, WSErrorCallback errorCallback, void* userData)
        {
			GS_CODE_TIMING_ASSERT();

			if(readyState == CONNECTING) return;
            
			// TODO: consider acquiring a lock on rxbuf...
			//for(;;) // while (true) withoput warning about constant expression
            {
                
                wsheader_type ws;
                {
                    if (rxbuf.size() < 2) { return; /* Need at least 2 */ }
                    const uint8_t * data = (uint8_t *) &rxbuf[0]; // peek, but don't consume
                    ws.fin = (data[0] & 0x80) == 0x80;
                    ws.opcode = (wsheader_type::opcode_type) (data[0] & 0x0f);
                    ws.mask = (data[1] & 0x80) == 0x80;
                    ws.N0 = (data[1] & 0x7f);
                    ws.header_size = 2 + (ws.N0 == 126? 2 : 0) + (ws.N0 == 127? 8 : 0) + (ws.mask? 4 : 0);
                    if (rxbuf.size() < ws.header_size) { return; /* Need: ws.header_size - rxbuf.size() */ }
                    int data_offset = -1;
                    if (ws.N0 < 126) {
                        ws.N = ws.N0;
                        data_offset = 2;
                    }
                    else if (ws.N0 == 126) {
                        ws.N = 0;
                        ws.N |= ((uint64_t) data[2]) << 8;
                        ws.N |= ((uint64_t) data[3]) << 0;
                        data_offset = 4;
                    }
                    else if (ws.N0 == 127) {
                        ws.N = 0;
                        ws.N |= ((uint64_t) data[2]) << 56;
                        ws.N |= ((uint64_t) data[3]) << 48;
                        ws.N |= ((uint64_t) data[4]) << 40;
                        ws.N |= ((uint64_t) data[5]) << 32;
                        ws.N |= ((uint64_t) data[6]) << 24;
                        ws.N |= ((uint64_t) data[7]) << 16;
                        ws.N |= ((uint64_t) data[8]) << 8;
                        ws.N |= ((uint64_t) data[9]) << 0;
                        data_offset = 10;
                    }
                    if (ws.mask) {
                        assert(data_offset != -1);
                        ws.masking_key[0] = ((uint8_t) data[data_offset+0]) << 0;
                        ws.masking_key[1] = ((uint8_t) data[data_offset+1]) << 0;
                        ws.masking_key[2] = ((uint8_t) data[data_offset+2]) << 0;
                        ws.masking_key[3] = ((uint8_t) data[data_offset+3]) << 0;
                    }
                    else {
                        ws.masking_key[0] = 0;
                        ws.masking_key[1] = 0;
                        ws.masking_key[2] = 0;
                        ws.masking_key[3] = 0;
                    }
                    if (rxbuf.size() < ws.header_size+ws.N) { return; /* Need: ws.header_size+ws.N - rxbuf.size() */ }
                }


				// We got a whole message, now do something with it:
				if (ws.opcode == wsheader_type::TEXT_FRAME && ws.fin) {
					if (ws.mask) { for (size_t i = 0; i != ws.N; ++i) { rxbuf[static_cast<gsstl::vector<char>::size_type>(i+ws.header_size)] ^= ws.masking_key[i&0x3]; } }
					gsstl::string data(rxbuf.begin()+ws.header_size, rxbuf.begin()+ws.header_size+(size_t)ws.N);
					messageCallback((const gsstl::string) data, userData);
				}
				else if (ws.opcode == wsheader_type::PING) {
					if (ws.mask) { for (size_t i = 0; i != ws.N; ++i) { rxbuf[static_cast<gsstl::vector<char>::size_type>(i+ws.header_size)] ^= ws.masking_key[i&0x3]; } }
					gsstl::string data(rxbuf.begin()+ws.header_size, rxbuf.begin()+ws.header_size+(size_t)ws.N);
					sendData(wsheader_type::PONG, data);
				}
				else if (ws.opcode == wsheader_type::PONG)
                {
					messageCallback((const gsstl::string) "{ \"@class\" : \".pong\" }", userData);
                }
				else if (ws.opcode == wsheader_type::CLOSE)
                {
                    close();
                }
				else
                {
                    fprintf(stderr, "ERROR: Got unexpected WebSocket message.\n");
					using namespace easywsclient;
					errorCallback(WSError(WSError::UNEXPECTED_MESSAGE, "Got unexpected WebSocket message."), userData);
                    close();
                }

				rxbuf.erase(rxbuf.begin(), rxbuf.begin() + ws.header_size+(size_t)ws.N);
			}
		}

		void sendPing()
        {
            if(readyState == CONNECTING) return;
        	sendData(wsheader_type::PING, gsstl::string());
		}

		void send(const gsstl::string& message)
        {
			GS_CODE_TIMING_ASSERT();
            if(readyState == CONNECTING) return;
			sendData(wsheader_type::TEXT_FRAME, message);
		}

		void sendData(wsheader_type::opcode_type type, const gsstl::string& message)
        {
			GS_CODE_TIMING_ASSERT();
			// TODO:
			// Masking key should (must) be derived from a high quality random
			// number generator, to mitigate attacks on non-WebSocket friendly
			// middleware:
			const uint8_t masking_key[4] = { 0x12, 0x34, 0x56, 0x78 };
			// TODO: consider acquiring a lock on txbuf...
			if (readyState == CLOSING || readyState == CLOSED || readyState == CONNECTING) { return; }
			gsstl::vector<uint8_t> header;
			uint64_t message_size = message.size();
			header.assign(2 + (message_size >= 126 ? 2 : 0) + (message_size >= 65536 ? 6 : 0) + (useMask ? 4 : 0), 0);
			header[0] = uint8_t(0x80 | type);

			if (message_size < 126) {
				header[1] = (message_size & 0xff) | (useMask ? 0x80 : 0);
				if (useMask) {
					header[2] = masking_key[0];
					header[3] = masking_key[1];
					header[4] = masking_key[2];
					header[5] = masking_key[3];
				}
			}
			else if (message_size < 65536) {
				header[1] = 126 | (useMask ? 0x80 : 0);
				header[2] = (message_size >> 8) & 0xff;
				header[3] = (message_size >> 0) & 0xff;
				if (useMask) {
					header[4] = masking_key[0];
					header[5] = masking_key[1];
					header[6] = masking_key[2];
					header[7] = masking_key[3];
				}
			}
			else { // TODO: run coverage testing here
				header[1] = 127 | (useMask ? 0x80 : 0);
				header[2] = (message_size >> 56) & 0xff;
				header[3] = (message_size >> 48) & 0xff;
				header[4] = (message_size >> 40) & 0xff;
				header[5] = (message_size >> 32) & 0xff;
				header[6] = (message_size >> 24) & 0xff;
				header[7] = (message_size >> 16) & 0xff;
				header[8] = (message_size >>  8) & 0xff;
				header[9] = (message_size >>  0) & 0xff;
				if (useMask) {
					header[10] = masking_key[0];
					header[11] = masking_key[1];
					header[12] = masking_key[2];
					header[13] = masking_key[3];
				}
			}
			// N.B. - txbuf will keep growing until it can be transmitted over the socket:
			txbuf.insert(txbuf.end(), header.begin(), header.end());
			txbuf.insert(txbuf.end(), message.begin(), message.end());
			if (useMask) {
				for (size_t i = 0; i != message.size(); ++i) { *(txbuf.end() - message.size() + i) ^= masking_key[i&0x3]; }
			}
		}

		void close() {
			GS_CODE_TIMING_ASSERT();
			if(readyState == CLOSING || readyState == CLOSED) { return; }
			readyState = CLOSING;
			uint8_t closeFrame[6] = {0x88, 0x80, 0x00, 0x00, 0x00, 0x00}; // last 4 bytes are a masking key
			gsstl::vector<uint8_t> header(closeFrame, closeFrame+6);
			txbuf.insert(txbuf.end(), header.begin(), header.end());
		}
        
        void forceClose()
        {
			GS_CODE_TIMING_ASSERT();
            if(readyState == CLOSING || readyState == CLOSED) { return; }
            readyState = CLOSING;
            txbuf.clear();
        }

		static void dns_lookup_thread_error_dispatcher(const easywsclient::WSError& error, void* ptr)
		{
			_RealWebSocket *self = (_RealWebSocket*)ptr;
			assert(self);
			self->ipLookupError = error;
		}

        static void* _s_dns_Lookup(void *ptr)
        {
            _RealWebSocket *self = (_RealWebSocket*)ptr;

			assert(self->socket);

#if !((GS_TARGET_PLATFORM == GS_PLATFORM_IOS || GS_TARGET_PLATFORM == GS_PLATFORM_MAC) && defined(__UNREAL__))
			threading::mutex_lock(self->lock);
#endif

#if defined(IW_SDK) || ((GS_TARGET_PLATFORM == GS_PLATFORM_IOS || GS_TARGET_PLATFORM == GS_PLATFORM_MAC) && defined(__UNREAL__))
			// here we're calling TCPSocket::connect, because we only want to to dns lookup and the initial connect in this thread.
			// no TLS/SSL handshake is performed just yet. This is because we cannot allocate memory on platforms like marmalade in a different thread.
			if (!self->socket->connect(self->m_host.c_str(), static_cast<short>(self->m_port)))
            {
                assert(self->socket);
                self->ipLookupError = easywsclient::WSError(easywsclient::WSError::CONNECT_FAILED, self->socket->get_error_string());
                self->ipLookup = keFailed;
            }
            else
            {
                self->ipLookup = keComplete;
            }
#else
			// Note: we're passing nullptr to doConnect2() - those callbacks are called in poll()
			if (!self->socket->connect(self->m_host.c_str(), static_cast<short>(self->m_port)) || !self->doConnect2(dns_lookup_thread_error_dispatcher, self))
			{
                if(self->ipLookupError.code == easywsclient::WSError::ALL_OK) // it's not because doConnect2 failed, but because connect() failed
                {
                    assert(self->socket);
                    self->ipLookupError = easywsclient::WSError(easywsclient::WSError::CONNECT_FAILED, self->socket->get_error_string());
                }
				self->ipLookup = keFailed;
			}
			else
			{
				self->ipLookup = keComplete;
			}
#endif
            
#if !((GS_TARGET_PLATFORM == GS_PLATFORM_IOS || GS_TARGET_PLATFORM == GS_PLATFORM_MAC) && defined(__UNREAL__))
			threading::mutex_unlock(self->lock);
			threading::thread_exit(self->dns_thread);
#endif

			return 0;
        }
        
        bool doConnect()
        {
            readyState = CONNECTING;
            ipLookup = keInprogress;
			ipLookupError = easywsclient::WSError();

#if ((GS_TARGET_PLATFORM == GS_PLATFORM_IOS || GS_TARGET_PLATFORM == GS_PLATFORM_MAC) && defined(__UNREAL__))
            _s_dns_Lookup((void*)this);
#else
			threading::mutex_init(lock);
			threading::thread_create(dns_thread, _s_dns_Lookup, (void*)this);
#endif
            
            return true;
        }
        
		bool doConnect2(WSErrorCallback errorCallback, void* userData)
        {
#if defined(IW_SDK) // on non-marmalade, this is called from a background-thread
            GS_CODE_TIMING_ASSERT();
#endif

			using namespace easywsclient;

            #define SEND(buf)  socket->send(buf, strlen(buf))
            #define RECV(buf)  socket->recv(buf, 1)
    
            {
                // XXX: this should be done non-blocking,
                char line[256];
                int status;
                int i;
                snprintf(line, 256, "GET /%s HTTP/1.1\r\n", m_path.c_str()); SEND(line);
                if (m_port == 80) {
                    snprintf(line, 256, "Host: %s\r\n", m_host.c_str()); SEND(line);
                }
                else {
                    snprintf(line, 256, "Host: %s:%d\r\n", m_host.c_str(), m_port); SEND(line);
                }
                snprintf(line, 256, "Authorization: 15db07114504480519240fcc892fcd25e357cedf\r\n"); SEND(line);
                snprintf(line, 256, "Upgrade: websocket\r\n"); SEND(line);
                snprintf(line, 256, "Connection: Upgrade\r\n"); SEND(line);
                if (!m_origin.empty()) {
                    snprintf(line, 256, "Origin: %s\r\n", m_origin.c_str()); SEND(line);
                }
                snprintf(line, 256, "Sec-WebSocket-Key: x3JJHMbDL1EzLkh9GBhXDw==\r\n"); SEND(line);
                snprintf(line, 256, "Sec-WebSocket-Version: 13\r\n"); SEND(line);
                snprintf(line, 256, "\r\n"); SEND(line);
                for (i = 0; i < 2 || (i < 255 && line[i-2] != '\r' && line[i-1] != '\n'); ++i)
				{
					if (RECV(line+i) == 0)
					{
						if(errorCallback)
							errorCallback(WSError(WSError::CLOSED_DURING_WS_HANDSHAKE, "The connection was closed while the websocket handshake was in progress."), userData);
						return false;
					}
				}
                line[i] = 0;
				if (i == 255)
				{
					fprintf(stderr, "ERROR: Got invalid status line connecting to: %s\n", m_url.c_str());
					if (errorCallback)
						errorCallback(WSError(WSError::INVALID_STATUS_LINE_DURING_WS_HANDSHAKE, "Got invalid status line connecting to : " + m_url), userData);
					return false;
				}
				if (sscanf(line, "HTTP/1.1 %d", &status) != 1 || status != 101)
				{
					fprintf(stderr, "ERROR: Got bad status connecting to %s: %s", m_url.c_str(), line);
					if (errorCallback)
						errorCallback(WSError(WSError::BAD_STATUS_CODE, "Got bad status connecting to : " + m_url), userData);
					return false;
				}
                // TODO: verify response headers,
                for(;;) // while (true)
				{
                    for (i = 0; i < 2 || (i < 255 && line[i-2] != '\r' && line[i-1] != '\n'); ++i)
					{
						if (RECV(line+i) == 0)
						{
							if (errorCallback)
								errorCallback(WSError(WSError::CLOSED_DURING_WS_HANDSHAKE, "The connection was closed while the websocket handshake was in progress."), userData);
							return false;
						}
					}
                    if (line[0] == '\r' && line[1] == '\n')
					{
						break;
					}
                }
            }

			socket->set_blocking(false);
			fprintf(stderr, "Connected to: %s\n", m_url.c_str());
            
            return true;
        }
	};

	easywsclient::WebSocket::pointer from_url(const gsstl::string& url, bool useMask, const gsstl::string& origin)
    {
		char host[256];
		int port;
		char path[256];

        bool secure_connection = false;
		
        if (url.size() >= 256) {
			fprintf(stderr, "ERROR: url size limit exceeded: %s\n", url.c_str());
			return NULL;
		}
		if (origin.size() >= 200) {
			fprintf(stderr, "ERROR: origin size limit exceeded: %s\n", origin.c_str());
			return NULL;
		}

		if (sscanf(url.c_str(), "ws://%[^:/]:%d/%s", host, &port, path) == 3) {
		}
		else if (sscanf(url.c_str(), "ws://%[^:/]/%s", host, path) == 2) {
			port = 80;
		}
		else if (sscanf(url.c_str(), "ws://%[^:/]:%d", host, &port) == 2) {
			path[0] = '\0';
		}
		else if (sscanf(url.c_str(), "ws://%[^:/]", host) == 1) {
			port = 80;
			path[0] = '\0';
		}
		else if (sscanf(url.c_str(), "wss://%[^:/]:%d/%s", host, &port, path) == 3) {
			secure_connection = true;
		}
		else if (sscanf(url.c_str(), "wss://%[^:/]/%s", host, path) == 2) {
			port = 443;
			secure_connection = true;
		}
		else if (sscanf(url.c_str(), "wss://%[^:/]:%d", host, &port) == 2) {
			path[0] = '\0';
			secure_connection = true;
		}
		else if (sscanf(url.c_str(), "wss://%[^:/]", host) == 1) {
			port = 443;
			path[0] = '\0';
			secure_connection = true;
		}
		else
        {
			fprintf(stderr, "ERROR: Could not parse WebSocket url: %s\n", url.c_str());
			return NULL;
		}
		fprintf(stderr, "easywsclient: connecting: host=%s port=%d path=/%s\n", host, port, path);

        
        _RealWebSocket *nWebsocket = new _RealWebSocket(host, path, port, url, origin, useMask, secure_connection);
        if (!nWebsocket->doConnect())
        {
            nWebsocket = NULL;
        }
        
        return easywsclient::WebSocket::pointer(nWebsocket);
	}
} // end of module-only namespace

namespace easywsclient {
	WebSocket::pointer WebSocket::from_url(const gsstl::string& url, const gsstl::string& origin) {
		//return ::from_url("wss://untrusted-root.badssl.com/", true, origin);
		//return ::from_url("wss://echo.websocket.org/", true, origin);
		return ::from_url(url, true, origin);
	}

	WebSocket::pointer WebSocket::from_url_no_mask(const gsstl::string& url, const gsstl::string& origin) {
		//return ::from_url("wss://untrusted-root.badssl.com/", false, origin);
		//return ::from_url("wss://echo.websocket.org/", false, origin);
		return ::from_url(url, false, origin);
	}
} // namespace easywsclient

#endif
