#ifndef _THRIFT_TRANSPORT_THTTPCLIENT_H_
#define _THRIFT_TRANSPORT_THTTPCLIENT_H_ 1

#include <transport/TTransportUtils.h>

namespace facebook { namespace thrift { namespace transport { 

/**
 * HTTP client implementation of the thrift transport. This was irritating
 * to write, but the alternatives in C++ land are daunting. Linking CURL
 * requires 23 dynamic libraries last time I checked (WTF?!?). All we have
 * here is a VERY basic HTTP/1.1 client which supports HTTP 100 Continue,
 * chunked transfer encoding, keepalive, etc. Tested against Apache.
 *
 * @author Mark Slee <mcslee@facebook.com>
 */
class THttpClient : public TTransport {
 public:
  THttpClient(boost::shared_ptr<TTransport> transport, std::string host, std::string path="");

  THttpClient(std::string host, int port, std::string path="");

  virtual ~THttpClient();

  void open() {
    transport_->open();
  }

  bool isOpen() {
    return transport_->isOpen();
  }
  
  bool peek() {    
    return transport_->peek();
  }

  void close() {
    transport_->close();
  }

  uint32_t read(uint8_t* buf, uint32_t len);

  void write(const uint8_t* buf, uint32_t len);
  
  void flush();

 private:
  void init();

 protected:

  boost::shared_ptr<TTransport> transport_;

  TMemoryBuffer writeBuffer_;
  TMemoryBuffer readBuffer_;

  std::string host_;
  std::string path_;

  bool readHeaders_;
  bool chunked_;
  uint32_t chunkSize_;
  uint32_t contentLength_;

  char* httpBuf_;
  uint32_t httpBufPos_;
  uint32_t httpBufSize_;

  uint32_t readMoreData();
  char* readLine(char* line, char** next);

  void readHeaders();
  void parseHeader(char* header);
  bool parseStatusLine(char* status);

  uint32_t readChunked();
  uint32_t parseChunkSize(char* line);

  char* readContent(char* pos, uint32_t size);

  char* refill();
  char* shift(char* pos);

};

}}} // facebook::thrift::transport

#endif // #ifndef _THRIFT_TRANSPORT_THTTPCLIENT_H_