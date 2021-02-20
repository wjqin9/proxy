//
// httpproxy.cpp
// ~~~~~~~~~~~~~~~~~~~
//
// Distributed under the Boost Software License, Version 1.0.
//
// The following is a flow and structural diagram depicting the
// various elements  (proxy, server  and client)  and how  they
// connect and interact with each other.

//
//                                    ---> upstream --->           +---------------+
//                                                     +---->------>               |
//                               +-----------+         |           | Remote Server |
//                     +--------->          [x]--->----+  +---<---[x]              |
//                     |         | TCP Proxy |            |        +---------------+
// +-----------+       |  +--<--[x] Server   <-----<------+
// |          [x]--->--+  |      +-----------+
// |  Client   |          |
// |           <-----<----+
// +-----------+
//                <--- downstream <---
//
//


#include <cstdlib>
#include <cstddef>
#include <iostream>
#include <string>

#include <boost/shared_ptr.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <boost/bind.hpp>
#include <boost/asio.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/algorithm/string.hpp>

namespace tcp_proxy
{
   namespace ip = boost::asio::ip;

   class bridge : public boost::enable_shared_from_this<bridge>
   {
   public:

      typedef ip::tcp::socket socket_type;
      typedef boost::shared_ptr<bridge> ptr_type;

      bridge(boost::asio::io_service& ios)
      : downstream_socket_(ios),
        upstream_socket_  (ios)
      {
      }
      ~bridge()
      {
      }

      socket_type& downstream_socket()
      {
         // Client socket
         return downstream_socket_;
      }

      socket_type& upstream_socket()
      {
         // Remote server socket
         return upstream_socket_;
      }
      void read_request()
      {
         downstream_socket_.async_read_some(boost::asio::buffer(downstream_data_, max_data_length), boost::bind(&bridge::handle_read, shared_from_this(),
         boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
      }

      void handle_read(const boost::system::error_code& error, std::size_t bytes_transferred)
      {
         if (!error && bytes_transferred > 0)
         {
            static const std::string header_end = "\r\n\r\n";
            int old_size = received_data_.size();
            received_data_.resize(received_data_.size() + bytes_transferred);
            std::copy(downstream_data_, downstream_data_ + bytes_transferred, received_data_.begin() + old_size);
            auto index = received_data_.find(header_end);
            if (index != std::string::npos)
            {
               std::string headers = received_data_.substr(0, index+4);
               std::string host, port;
               std::tie(host, port, is_http_) = parse_header(headers);
               start(host, port);
            }
            else
            {
               read_request();
            }
         }
         else
         {
            close();
         }
      }

      std::tuple<const std::string, const std::string, bool> parse_header(const std::string& header) const
      {
      // CONNECT www.reddit.com:443 HTTP/1.1
      // Host: www.reddit.com:443
      // Proxy-Connection: keep-alive
      // User-Agent: Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/88.0.4324.150 Safari/537.36

      // GET http://google.com/ HTTP/1.1
      // Host: google.com
      // Proxy-Connection: keep-alive
      // Upgrade-Insecure-Requests: 1
      // User-Agent: Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/88.0.4324.150 Safari/537.36
      // Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.9
      // Accept-Encoding: gzip, deflate
      // Accept-Language: en-US,en;q=0.9

         std::vector<std::string> headers;
         boost::split(headers, header, boost::is_any_of("\r\n"), boost::token_compress_on);
         std::string host, port;
         bool is_http = false;
         if (headers.size() > 0)
         {
            std::vector<std::string> splitvec;
            boost::split(splitvec, headers[0], boost::is_any_of(" "), boost::token_compress_on);
            if (splitvec.size() >= 3)
            {
               if (splitvec[0] != "CONNECT")
               {
                  is_http = true;
               }
               std::string uri = splitvec[1];
               if (uri.find("http://") == 0)
               {
                  uri.erase(0, 7);
                  size_t pos = uri.find("/");
                  if (pos != std::string::npos)
                  {
                     uri.erase(pos, uri.length() - pos);
                  }
               }
               std::vector<std::string> splitvec2;
               boost::split(splitvec2, uri, boost::is_any_of(":"), boost::token_compress_on);
               if (splitvec2.size() == 2) 
               {
                  host = splitvec2[0];
                  port = splitvec2[1];  
               }
               else if (splitvec2.size() == 1)
               {
                  host = splitvec2[0];
                  port = "80";               
               }
            }
         }
         return std::make_tuple(host, port, is_http);
      }

      void response(unsigned status, const std::string& message)
      {
         std::string reply = std::string("HTTP/1.0 " ) + std::to_string(status) + " " + message + "\r\n";
         reply += "Proxy-agent: proxy\r\n\r\n";
         downstream_socket_.async_send(boost::asio::buffer(reply, reply.size()), boost::bind(&bridge::handle_response_sent, shared_from_this(),
         boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
      }

      void upstream_response()
      {
         if (received_data_.size() > 0)
         {
            upstream_socket_.async_send(boost::asio::buffer(received_data_, received_data_.size()), boost::bind(&bridge::handle_response_sent, shared_from_this(),
            boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
         }
         else 
         {
            transact();
         }
      }

      void start(const std::string& upstream_host, const std::string& upstream_port)
      {
         // Attempt connection to remote server (upstream side)
         boost::asio::io_service io_service;
         boost::asio::ip::tcp::resolver resolver(io_service);
         boost::asio::ip::tcp::resolver::query query(upstream_host, upstream_port);
         boost::asio::ip::tcp::resolver::iterator iter = resolver.resolve(query);
         upstream_socket_.async_connect(
               iter->endpoint(),
               boost::bind(&bridge::handle_upstream_connect,
                    shared_from_this(),
                    boost::asio::placeholders::error));
      }

      void handle_upstream_connect(const boost::system::error_code& error)
      {
         if (!error)
         {
            if (is_http_)
            {
               upstream_response();
            }
            else
            {
               response(200, "Connection established");
            }
         }
      }

      void transact() {
         // Setup async read from remote server (upstream)
         upstream_socket_.async_read_some(
               boost::asio::buffer(upstream_data_,max_data_length),
               boost::bind(&bridge::handle_upstream_read,
                     shared_from_this(),
                     boost::asio::placeholders::error,
                     boost::asio::placeholders::bytes_transferred));

         // Setup async read from client (downstream)
         downstream_socket_.async_read_some(
               boost::asio::buffer(downstream_data_,max_data_length),
               boost::bind(&bridge::handle_downstream_read,
                     shared_from_this(),
                     boost::asio::placeholders::error,
                     boost::asio::placeholders::bytes_transferred));

      }

      void handle_response_sent(const boost::system::error_code& error, size_t bytes_transferred) 
      {
         if (!error) {
            transact();
         }
         else
         {
            response(404, "Connection failed");
            close();
         }
      }

   private:

      /*
         Section A: Remote Server --> Proxy --> Client
         Process data recieved from remote sever then send to client.
      */

      // Read from remote server complete, now send data to client
      void handle_upstream_read(const boost::system::error_code& error,
                                const size_t& bytes_transferred)
      {
         if (!error)
         {
            async_write(downstream_socket_,
                 boost::asio::buffer(upstream_data_,bytes_transferred),
                 boost::bind(&bridge::handle_downstream_write,
                      shared_from_this(),
                      boost::asio::placeholders::error));
         }
         else
         {
            close();
         }
      }

      // Write to client complete, Async read from remote server
      void handle_downstream_write(const boost::system::error_code& error)
      {
         if (!error)
         {
            upstream_socket_.async_read_some(
                 boost::asio::buffer(upstream_data_,max_data_length),
                 boost::bind(&bridge::handle_upstream_read,
                      shared_from_this(),
                      boost::asio::placeholders::error,
                      boost::asio::placeholders::bytes_transferred));
         }
         else
         {
            close();
         }
      }
      // *** End Of Section A ***


      /*
         Section B: Client --> Proxy --> Remove Server
         Process data recieved from client then write to remove server.
      */

      // Read from client complete, now send data to remote server
      void handle_downstream_read(const boost::system::error_code& error,
                                  const size_t& bytes_transferred)
      {
         if (!error)
         {
            async_write(upstream_socket_,
                  boost::asio::buffer(downstream_data_,bytes_transferred),
                  boost::bind(&bridge::handle_upstream_write,
                        shared_from_this(),
                        boost::asio::placeholders::error));
         }
         else
         {
            close();
         }
      }

      // Write to remote server complete, Async read from client
      void handle_upstream_write(const boost::system::error_code& error)
      {
         if (!error)
         {
            downstream_socket_.async_read_some(
                 boost::asio::buffer(downstream_data_,max_data_length),
                 boost::bind(&bridge::handle_downstream_read,
                      shared_from_this(),
                      boost::asio::placeholders::error,
                      boost::asio::placeholders::bytes_transferred));
         }
         else
         {
            close();
         }
      }
      // *** End Of Section B ***

      void close()
      {
         boost::mutex::scoped_lock lock(mutex_);
         if (downstream_socket_.is_open())
         {
            downstream_socket_.close();
         }

         if (upstream_socket_.is_open())
         {
            upstream_socket_.close();
         }
      }

      socket_type downstream_socket_;
      socket_type upstream_socket_;

      enum { max_data_length = 8192 }; //8KB
      unsigned char downstream_data_[max_data_length];
      unsigned char upstream_data_  [max_data_length];
      std::string received_data_;

      boost::mutex mutex_;

      bool is_http_ = false;

   public:

      class acceptor
      {
      public:

         acceptor(boost::asio::io_service& io_service,
                  const std::string& local_host, unsigned short local_port)
         : io_service_(io_service),
           localhost_address(boost::asio::ip::address_v4::from_string(local_host)),
           acceptor_(io_service_,ip::tcp::endpoint(localhost_address,local_port))
         {}

         bool accept_connections()
         {
            try
            {
               session_ = boost::shared_ptr<bridge>(new bridge(io_service_));

               acceptor_.async_accept(session_->downstream_socket(),
                    boost::bind(&acceptor::handle_accept,
                         this,
                         boost::asio::placeholders::error));
            }
            catch(std::exception& e)
            {
               std::cerr << "acceptor exception: " << e.what() << std::endl;
               return false;
            }

            return true;
         }

      private:

         void handle_accept(const boost::system::error_code& error)
         {
            if (!error)
            {
               session_->read_request();

               if (!accept_connections())
               {
                  std::cerr << "Failure during call to accept." << std::endl;
               }
            }
            else
            {
               std::cerr << "Error: " << error.message() << std::endl;
            }
         }

         boost::asio::io_service& io_service_;
         ip::address_v4 localhost_address;
         ip::tcp::acceptor acceptor_;
         ptr_type session_;
      };

   };
}

int main(int argc, char* argv[])
{
   if (argc != 3)
   {
      std::cerr << "usage: tcpproxy_server <local host ip> <local port>" << std::endl;
      return 1;
   }

   const unsigned short local_port   = static_cast<unsigned short>(::atoi(argv[2]));
   const std::string local_host      = argv[1];

   boost::asio::io_service ios;

   try
   {
      tcp_proxy::bridge::acceptor acceptor(ios,
                                           local_host, local_port);

      acceptor.accept_connections();

      ios.run();
   }
   catch(std::exception& e)
   {
      std::cerr << "Error: " << e.what() << std::endl;
      return 1;
   }

   return 0;
}
